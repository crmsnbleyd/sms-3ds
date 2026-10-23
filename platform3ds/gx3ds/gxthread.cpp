// The GX thread: the GX front end (FIFO parser, vertex loader, display-list cache) and the renderer
// run on the New 3DS's third core, beside the game, as the GameCube's GPU runs beside its CPU.
//
// The game's GX calls become one ordered command stream (patches/port/3ds-08-gx-async-api.patch,
// 3ds-05-gx-fifo.patch): register writes as the FIFO commands they are, vertices as the game writes
// them, and the PC port's direct state writes (host pointers, getter shadows) as private records.
// gx_fifo.cpp's pipe collects the stream and hands it over here in chunks; this thread parses them
// against gx::g, which it alone uses while it runs. The API keeps its own copy of the state.
//
// The game waits for the GPU only where the GameCube's would: GXDrawDone/GXWaitDrawDone (a fence)
// and draw sync tokens, whose callbacks run on the game's side as interrupts (port_irq), not here.
// Peeks and pixel metrics answer constants on the 3DS, so they need no fence.
//
// Without a third core (Old 3DS) or with sdmc:/3ds/sms-3ds/gx-sync present, GX runs synchronously
// on the game's thread as before.
#include <3ds.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "gx_internal.h"
#include "port_os.h"
#include "sms3ds.h"

// HUD (sms3ds.h): per game frame, the game thread's frame time, its waits for the GX thread, and
// the GX thread's busy time.
volatile float sms3ds_prof_cpu_ms, sms3ds_prof_gxwait_ms, sms3ds_prof_gxbusy_ms;
// the game thread's waits by cause (us): queue full, GXDrawDone fences, the frame boundary; fences
volatile uint32_t sms3ds_gx_waits[4];
// GX thread, per game frame: GXCallDisplayList calls and their time outside flushes and vertex
// decoding (the display-list cache's checks and replays), and the stream's size
volatile float sms3ds_prof_dl_ms;
volatile uint32_t sms3ds_gx_dl_calls, sms3ds_gx_stream_kb;

namespace gx {
void pipeSetAsync(bool on);  // gx_fifo.cpp (3ds-05-gx-fifo.patch)
void pipePushAll();
uint32_t parseStream(const uint8_t* p, uint32_t n);
bool gxAsync();
extern void (*drawSyncCallback)(uint16_t);
extern double g_flushSeconds, g_decodeSeconds;
enum : uint8_t {  // private records (gx_api.cpp, gx_fifo.cpp)
	PRIV_SIDE = 1, PRIV_CALLDL, PRIV_INVTEX, PRIV_ARRAY,
	PRIV_BE_ADD = 8, PRIV_BE_REMOVE, PRIV_ARRAY_BE, PRIV_DEFAULT_BE,
	PRIV_HUD = 16,  // render.cpp GXPC_SetHud
	PRIV_YUV,       // render.cpp sms3ds_gx_draw_yuv
};
void drawYuvFrame(const uint32_t* a);  // render.cpp
}  // namespace gx

extern "C" {
void GXPC_AddBigEndianRange(const void* p, uint32_t size);
void GXPC_RemoveBigEndianRange(const void* p);
void GXPC_SetArrayBigEndian(int attr, int bigEndian);
void GXPC_SetDefaultArrayBigEndian(int bigEndian);
void GXPC_SetHud(int on);
}

namespace {

using namespace gx;

State s_shadow;            // the API's copy of the state while the thread runs
State* s_api = &gx::g;     // what gx_api.cpp reads and writes (GA)
bool s_tried, s_threadOk;  // the thread was attempted / runs
Thread s_thread;

// --- the stream: chunks handed from the game (producer) to the GX thread (consumer) ----------------
const int kSlots = 64;
const uint32_t kSlotCap = 32u << 10;
struct Slot {
	uint8_t* data;
	uint32_t len;
};
Slot s_slots[kSlots];
int s_w, s_r;
LightSemaphore s_filled, s_free;
std::atomic<uint32_t> s_pushed{0}, s_done{0};  // chunks sent / run
std::atomic<bool> s_fenceWaiting{false};
u64 s_waitTicks, s_lastBoundary;     // the game's side
u64 s_waitBy[3];                      // s_waitTicks by cause: queue full, fence, frame boundary
std::atomic<uint32_t> s_busyTicks{0};  // the GX thread's (ticks / 64, so a frame fits in 32 bits)
std::atomic<uint32_t> s_dlTicks{0}, s_dlCalls{0}, s_streamBytes{0};  // the GX thread's, as above
LightEvent s_doneEvent;
std::vector<uint8_t> s_carry;  // the incomplete command at the end of the last chunk, and what follows
uint32_t s_carryLen;

// --- draw sync tokens, reached here, run on the game's side as interrupts ---------------------------
const uint32_t kTokens = 64;
uint16_t s_tokens[kTokens];
std::atomic<uint32_t> s_tokW{0}, s_tokR{0};
void (*s_gameSyncCb)(uint16_t);

void postToken(uint16_t token)  // the GX thread: in place of the game's callback
{
	const uint32_t w = s_tokW.load();
	s_tokens[w % kTokens] = token;
	s_tokW.store(w + 1);
	port_irq_kick();
}

void pollTokens()  // the game's side, in interrupt context (port_irq source)
{
	for (uint32_t r = s_tokR.load(); r != s_tokW.load(); r = s_tokR.load()) {
		const uint16_t t = s_tokens[r % kTokens];
		s_tokR.store(r + 1);
		if (s_gameSyncCb)
			s_gameSyncCb(t);
	}
}

void runChunk(const uint8_t* p, uint32_t n)
{
	if (!s_carryLen) {  // usually: parse in place, keep only an incomplete tail
		const uint32_t used = parseStream(p, n);
		p += used;
		n -= used;
		if (!n)
			return;
	}
	if (s_carry.size() < s_carryLen + n)
		s_carry.resize((s_carryLen + n) * 2);
	memcpy(s_carry.data() + s_carryLen, p, n);
	s_carryLen += n;
	const uint32_t used = parseStream(s_carry.data(), s_carryLen);
	memmove(s_carry.data(), s_carry.data() + used, s_carryLen - used);
	s_carryLen -= used;
}

void gxThread(void*)
{
	for (;;) {
		LightSemaphore_Acquire(&s_filled, 1);
		Slot& s = s_slots[s_r];
		const u64 t0 = svcGetSystemTick();
		runChunk(s.data, s.len);
		s_streamBytes.fetch_add(s.len);
		s_busyTicks.fetch_add(uint32_t((svcGetSystemTick() - t0) >> 6));
		s_r = (s_r + 1) % kSlots;
		LightSemaphore_Release(&s_free, 1);
		s_done.fetch_add(1);
		if (s_fenceWaiting.load())
			LightEvent_Signal(&s_doneEvent);
	}
}

bool startThread()
{
	FILE* f = fopen("sdmc:/3ds/sms-3ds/gx-sync", "r");
	if (f) {
		fclose(f);
		fprintf(stderr, "[gx3ds] gx-sync present: GX runs on the game's thread\n");
		return false;
	}
	for (int i = 0; i < kSlots; i++) {
		s_slots[i].data = (uint8_t*)malloc(kSlotCap);
		if (!s_slots[i].data)
			return false;
	}
	LightSemaphore_Init(&s_filled, 0, kSlots);
	LightSemaphore_Init(&s_free, kSlots, kSlots);
	LightEvent_Init(&s_doneEvent, RESET_STICKY);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_thread = threadCreate(gxThread, nullptr, 256 * 1024, prio, 2, true);
	fprintf(stderr, "[gx3ds] GX thread %s\n", s_thread ? "on core 2" : "unavailable (no core 2): synchronous");
	if (!s_thread)
		return false;
	port_irq_add_source(pollTokens);
	return true;
}

}  // namespace

namespace gx {

State* apiState() { return s_api; }
bool gxqOnGxThread() { return s_threadOk && threadGetCurrent() == s_thread; }

// The game's side: the stream so far, in chunks (blocks while the GX thread is a queue behind).
void gxqPush(const uint8_t* p, uint32_t n)
{
	while (n) {
		const uint32_t m = n < kSlotCap ? n : kSlotCap;
		if (LightSemaphore_TryAcquire(&s_free, 1)) {  // the queue is full: wait for the GX thread
			const u64 t0 = svcGetSystemTick();
			LightSemaphore_Acquire(&s_free, 1);
			s_waitTicks += svcGetSystemTick() - t0;
			s_waitBy[0] += svcGetSystemTick() - t0;
		}
		Slot& s = s_slots[s_w];
		memcpy(s.data, p, m);
		s.len = m;
		s_w = (s_w + 1) % kSlots;
		s_pushed.fetch_add(1);
		LightSemaphore_Release(&s_filled, 1);
		p += m;
		n -= m;
	}
}

namespace {
uint32_t s_prevFrameEnd;  // chunks up to the previous frame's display copy
uint32_t s_fences;        // gxqFence calls this frame

void waitDone(uint32_t target, int cause)
{
	if (int32_t(s_done.load() - target) >= 0)
		return;
	const u64 t0 = svcGetSystemTick();
	LightEvent_Clear(&s_doneEvent);
	s_fenceWaiting.store(true);
	while (int32_t(s_done.load() - target) < 0)
		LightEvent_Wait(&s_doneEvent);
	s_fenceWaiting.store(false);
	s_waitTicks += svcGetSystemTick() - t0;
	s_waitBy[cause] += svcGetSystemTick() - t0;
}
}  // namespace

// A position in the stream (everything the game has sent so far) and whether the GX thread has
// run past it: lets the game reuse memory a draw read without waiting for the whole frame
// (patches/decomp/3ds-09: the THP player's texture sets).
extern "C" uint32_t sms3ds_gx_mark(void)
{
	if (!gxAsync())
		return 0;
	pipePushAll();
	return s_pushed.load();
}

extern "C" int sms3ds_gx_reached(uint32_t mark)
{
	return !gxAsync() || int32_t(s_done.load() - mark) >= 0;
}

void gxqFence()
{
	if (!gxAsync())
		return;
	pipePushAll();
	s_fences++;
	waitDone(s_pushed.load(), 1);
}

// After a display copy: the GX thread may still draw this frame, but must have finished the one
// before (one frame in flight, as on the GameCube, whose games double-buffer per-frame data).
void gxqFrameBoundary()
{
	if (!gxAsync())
		return;
	pipePushAll();
	const uint32_t prev = s_prevFrameEnd;
	s_prevFrameEnd = s_pushed.load();
	waitDone(prev, 2);
	const u64 now = svcGetSystemTick();
	if (s_lastBoundary) {
		const double ms = 1000.0 / SYSCLOCK_ARM11;
		sms3ds_prof_cpu_ms = (float)((now - s_lastBoundary) * ms);
		sms3ds_prof_gxwait_ms = (float)(s_waitTicks * ms);
		for (int i = 0; i < 3; i++)
			sms3ds_gx_waits[i] = (uint32_t)(s_waitBy[i] * 1000000ull / SYSCLOCK_ARM11);
		sms3ds_gx_waits[3] = s_fences;
		sms3ds_prof_gxbusy_ms = (float)((u64)s_busyTicks.exchange(0) * 64 * ms);
		sms3ds_prof_dl_ms = (float)((u64)s_dlTicks.exchange(0) * 64 * ms);
		sms3ds_gx_dl_calls = s_dlCalls.exchange(0);
		sms3ds_gx_stream_kb = s_streamBytes.exchange(0) >> 10;
	}
	s_lastBoundary = now;
	s_waitTicks = 0;
	s_waitBy[0] = s_waitBy[1] = s_waitBy[2] = 0;
	s_fences = 0;
}

void gxqStop()
{
	if (!gxAsync())
		return;
	gxqFence();
	pipeSetAsync(false);
	s_api = &gx::g;
	drawSyncCallback = s_gameSyncCb;
}

void gxqStart()
{
	if (!s_tried) {
		s_tried = true;
		s_threadOk = startThread();
	}
	if (!s_threadOk)
		return;
	s_shadow = gx::g;
	s_api = &s_shadow;
	drawSyncCallback = s_gameSyncCb ? postToken : nullptr;
	pipeSetAsync(true);
}

uint16_t gxqDrawSyncToken() { return gx::g.drawSyncToken; }

void* gxqSetDrawSyncCallback(void* cb)
{
	void* old = (void*)s_gameSyncCb;
	s_gameSyncCb = (void (*)(uint16_t))cb;
	drawSyncCallback = gxAsync() ? (s_gameSyncCb ? postToken : nullptr) : s_gameSyncCb;
	return old;
}

// A private record, on the GX thread.
void gxqPrivate(uint8_t kind, const uint8_t* payload, uint32_t n)
{
	uint32_t a[3] = {};
	memcpy(a, payload, n < sizeof a ? n : sizeof a);
	switch (kind) {
	case PRIV_SIDE: {  // offset, flush, bytes
		uint8_t* dst = reinterpret_cast<uint8_t*>(&gx::g) + a[0];
		const uint8_t* src = payload + 8;
		const uint32_t len = n - 8;
		if (n < 8 || a[0] + len > sizeof(State))
			break;
		if (a[1] && memcmp(dst, src, len))
			onStateChange();
		memcpy(dst, src, len);
		break;
	}
	case PRIV_CALLDL: {
		const u64 t0 = svcGetSystemTick();
		const double f0 = g_flushSeconds, d0 = g_decodeSeconds;
		callDisplayList(reinterpret_cast<const uint8_t*>(uintptr_t(a[0])), a[1]);
		const double inner = (g_flushSeconds - f0) + (g_decodeSeconds - d0);
		const s64 t = (s64)(svcGetSystemTick() - t0) - (s64)(inner * SYSCLOCK_ARM11);
		if (t > 0)
			s_dlTicks.fetch_add(uint32_t(t >> 6));
		s_dlCalls.fetch_add(1);
		break;
	}
	case PRIV_INVTEX:
		flushBatch();
		textureInvalidateAll();
		break;
	case PRIV_ARRAY: {  // slot, base, stride (as GXSetArray)
		const uint8_t* base = reinterpret_cast<const uint8_t*>(uintptr_t(a[1]));
		g.arrayBase[a[0]] = base;
		g.arrayStride[a[0]] = a[2];
		g.arrayBigEndian[a[0]] = g_defaultArrayBE || isBigEndianData(base);
		g_arrayGen++;
		break;
	}
	case PRIV_BE_ADD: GXPC_AddBigEndianRange(reinterpret_cast<const void*>(uintptr_t(a[0])), a[1]); break;
	case PRIV_BE_REMOVE: GXPC_RemoveBigEndianRange(reinterpret_cast<const void*>(uintptr_t(a[0]))); break;
	case PRIV_ARRAY_BE: GXPC_SetArrayBigEndian((int)a[0], (int)a[1]); break;
	case PRIV_DEFAULT_BE: GXPC_SetDefaultArrayBigEndian((int)a[0]); break;
	case PRIV_HUD: GXPC_SetHud((int)a[0]); break;
	case PRIV_YUV: {  // nine words: render.cpp sms3ds_gx_draw_yuv
		uint32_t w[9] = {};
		memcpy(w, payload, n < sizeof w ? n : sizeof w);
		drawYuvFrame(w);
		break;
	}
	default: logmsg("unknown GX thread record %u", kind); break;
	}
}

}  // namespace gx
