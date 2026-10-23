// Debug display on the bottom screen: live counters and the newest log lines, shown while L+R+X
// toggles it on. Otherwise the renderer draws the game's HUD there (gx3ds/render.cpp). Runs on its own thread on the system core (core 1), so it keeps updating
// while the game's threads hold the application core.
#include "sms3ds.h"

#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

volatile uint32_t sms3ds_gx_frames;
volatile uint32_t sms3ds_gx_copies;
volatile uint32_t sms3ds_gx_primitives;
volatile uint32_t sms3ds_gx_vertices;
volatile uint32_t sms3ds_hud_debug_shown;
// debugging: azahar.py writes an EFB point to sms3ds_peek_probe[0..1] and sets [2]; the HUD thread
// answers GXPeekZ's value there in [3] (and clears [2])
volatile uint32_t sms3ds_peek_probe[4];
namespace gx {
uint32_t peekZ(int x, int y);  // gx3ds/render.cpp
}
extern "C" volatile Sms3dsHid sms3ds_remote_pad;  // hid_3ds.cpp: scripts/azahar.py's buttons

namespace {

PrintConsole s_bottom;
const devoptab_t* s_con;  // the console's stdout devoptab, saved before stdout goes to the log
Thread s_thread;
u64 s_start;

void put(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 1)
		n = sizeof buf - 1;
	if (n > 0 && s_con)
		s_con->write_r(_REENT, NULL, buf, (size_t)n);
}

void drawLog()
{
	static char tail[10][64];
	int n = sms3ds_log_tail(tail, 10);
	for (int i = 0; i < 10; i++)  // 40 columns on the bottom screen; clear the rest of each row
		put("\x1b[%d;1H%-40.40s", i + 18, i < n ? tail[i] : "");
}

void drawBottom()
{
	consoleSelect(&s_bottom);
	u64 ms = osGetTime() - s_start;
	put("\x1b[1;1H\x1b[7m sms-3ds  phase 2                        \x1b[0m");
	put("\x1b[3;1Huptime     %6llu.%01llu s", ms / 1000, (ms / 100) % 10);
	put("\x1b[4;1Hframes     %10lu", (unsigned long)sms3ds_gx_frames);
	// last frame: vertices decoded vs replayed from the display-list cache; the flushes' parts; the
	// game's own drawing (draw phase less GX decoding, flushes and replays) and time outside the phases
	put("\x1b[5;1Hfresh %7lu verts replay %7lu   ", (unsigned long)sms3ds_gx_fresh_verts,
	    (unsigned long)sms3ds_gx_replay_verts);
	put("\x1b[6;1Hbeg%3.0f state%3.0f idx%3.0f dc%3.0f repl%3.0f ", (double)sms3ds_prof_begin_ms,
	    (double)sms3ds_prof_state_ms, (double)sms3ds_prof_idx_ms, (double)sms3ds_prof_draw_ms,
	    (double)sms3ds_prof_replay_ms);
	// the game thread's frame, its waits for the GX thread, and the GX thread's busy time
	put("\x1b[7;1Hcpu %4.0f  gx wait %4.0f  gx busy %4.0f  ", (double)sms3ds_prof_cpu_ms,
	    (double)sms3ds_prof_gxwait_ms, (double)sms3ds_prof_gxbusy_ms);
	put("\x1b[8;1Hper frame  %5lu batches %7lu verts  ", (unsigned long)sms3ds_gx_batches_per_frame,
	    (unsigned long)sms3ds_gx_vertices_per_frame);
	put("\x1b[9;1Hdl %4lu calls %4.0f ms  stream %5lu KiB  ", (unsigned long)sms3ds_gx_dl_calls,
	    (double)sms3ds_prof_dl_ms, (unsigned long)sms3ds_gx_stream_kb);
	put("\x1b[10;1Happrox %9lu  why %05lx    ", (unsigned long)sms3ds_gx_tev_approx,
	    (unsigned long)sms3ds_gx_approx_why);
	{
		const float fr = sms3ds_prof_frame_ms, fl = sms3ds_prof_flush_ms, dc = sms3ds_prof_decode_ms,
		            wt = sms3ds_prof_wait_ms;
		// GX thread: what its busy time holds beside the flushes, decoding and copies (parsing, the
		// display-list cache, private records)
		float other = sms3ds_prof_gxbusy_ms - fl - dc - sms3ds_prof_copy_ms;
		if (other < 0.0f)
			other = 0.0f;
		// ms per frame: render = backend flushes (xform = CPU transform, tex = texture work)
		put("\x1b[13;1Hframe %4.0f render %4.0f xform %4.0f  ", (double)fr, (double)fl, (double)sms3ds_prof_xform_ms);
		put("\x1b[14;1Htex %3.0f dec %3.0f copy %3.0f other %4.0f  ", (double)sms3ds_prof_tex_ms, (double)dc,
		    (double)sms3ds_prof_copy_ms, (double)other);
	}
	{
		const struct mallinfo mi = mallinfo();
		put("\x1b[11;1Hheap used %5lu of %5lu KiB    ", (unsigned long)(mi.uordblks >> 10),
		    (unsigned long)(envGetHeapSize() >> 10));
		put("\x1b[12;1Hlinear free %5lu of %5lu KiB  ", (unsigned long)(linearSpaceFree() >> 10),
		    (unsigned long)(envGetLinearHeapSize() >> 10));
	}
	put("\x1b[16;1Hhit%3.0f move%3.0f anim%3.0f draw%3.0f mix%3.0f ", (double)sms3ds_prof_phase_ms[0],
	    (double)sms3ds_prof_phase_ms[1], (double)sms3ds_prof_phase_ms[2], (double)sms3ds_prof_phase_ms[3],
	    (double)sms3ds_prof_phase_ms[4]);
	put("\x1b[15;1Hstate %d area %d ep %d file %d  pad %04x   ", sms3ds_state, sms3ds_area, sms3ds_episode,
	    sms3ds_save_file, (unsigned)sms3ds_pad_buttons);
	put("\x1b[17;1H\x1b[7m log  (START+SELECT quit, L+R+X hide)    \x1b[0m");
}

void hudThread(void*)
{
	// Input is polled every 50 ms so a short L+R+X press is seen; the display redraws every 250 ms.
	// Only held state is used (the game thread scans HID too, so "keys down" is unreliable here).
	bool shown = false, comboWas = false;
	for (unsigned tick = 0;; tick++) {
		hidScanInput();
		u32 held = hidKeysHeld();
		if (!aptMainLoop() || ((held & KEY_START) && (held & KEY_SELECT))) {
			fflush(stderr);
			svcExitProcess();
		}
		if (sms3ds_peek_probe[2]) {
			sms3ds_peek_probe[3] = gx::peekZ((int)sms3ds_peek_probe[0], (int)sms3ds_peek_probe[1]);
			sms3ds_peek_probe[2] = 0;
		}
		const uint32_t remote = sms3ds_remote_pad.keys;
		const bool combo = (held & (KEY_L | KEY_R | KEY_X)) == (KEY_L | KEY_R | KEY_X) ||
		                   (remote & (SMS3DS_L | SMS3DS_R | SMS3DS_X)) == (SMS3DS_L | SMS3DS_R | SMS3DS_X);
		const bool toggled = combo && !comboWas;
		comboWas = combo;
		if (toggled) {
			shown = !shown;
			sms3ds_hud_debug_shown = shown;
			if (shown)  // the renderer lets go of the bottom screen at its next frame
				svcSleepThread(100 * 1000 * 1000LL);
		}
		if (shown && (toggled || tick % 5 == 0)) {
			consoleSelect(&s_bottom);
			if (toggled)
				consoleClear();
			drawBottom();
			drawLog();
			gfxFlushBuffers();
			gfxScreenSwapBuffers(GFX_BOTTOM, false);
		}
		svcSleepThread(50 * 1000 * 1000LL);
	}
}

}  // namespace

extern "C" void sms3ds_hud_init(void)
{
	s_start = osGetTime();
	consoleInit(GFX_BOTTOM, &s_bottom);
	s_con = devoptab_list[STD_OUT];
}

extern "C" void sms3ds_hud_start(void)
{
	// The system core gives applications a share of its time only when asked.
	APT_SetAppCpuTimeLimit(30);
	s_thread = threadCreate(hudThread, NULL, 32 * 1024, 0x18, 1, true);
	if (!s_thread)  // no system core time: run on the application core instead
		s_thread = threadCreate(hudThread, NULL, 32 * 1024, 0x18, 0, true);
}

extern "C" void sms3ds_hud_fatal(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	sms3ds_hud_debug_shown = 1;
	svcSleepThread(100 * 1000 * 1000LL);
	consoleSelect(&s_bottom);
	consoleClear();
	put("\x1b[14;1H\x1b[31m%.40s\x1b[0m", msg);
	while (aptMainLoop()) {
		drawLog();
		gfxFlushBuffers();
		gfxScreenSwapBuffers(GFX_BOTTOM, false);
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}
	gfxExit();
	svcExitProcess();
}
