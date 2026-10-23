// GX renderer on citro3d. Fills the renderer seam declared in sms-pc-port's gx_internal.h; the GX
// front end (gx_api.cpp, gx_fifo.cpp) is unchanged.
//
// A batch is a run of primitives with unchanged GX state (the front end calls onStateChange before
// any state write), as in sms-pc-port's gx_render.cpp. The front end's vertex loader writes the
// batch's packed vertices straight into a per-frame ring in linear memory, where the PICA reads
// them in place: transform, lighting and texgen run in the vertex shader (gx.v.pica, set up by
// vtx.cpp), the TEV in the texture combiners (tev.cpp). A display copy (GXCopyDisp) ends the
// citro3d frame, which shows the render target on the top screen; its clear applies when the next
// frame begins.
#include "gx3ds.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gx_internal.h"
#include "sms3ds.h"
#include "gx3ds_gx_shbin.h"
#include "gx3ds_passthru_shbin.h"

namespace gx {
uint32_t g_displayFrames;
bool g_gxStats = false;  // gx_fifo.cpp times its vertex loader (g_decodeSeconds) for the HUD
extern double g_decodeSeconds;
double g_flushSeconds;
uint32_t g_traceLastVat;
bool gxAsync();          // gx_fifo.cpp (3ds-05-gx-fifo.patch)
bool gxqOnGxThread();    // gxthread.cpp
void streamPrivate(uint8_t kind, const void* a, uint32_t na, const void* b, uint32_t nb);  // gx_fifo.cpp
enum : uint8_t { PRIV_HUD = 16 };  // gxthread.cpp
}  // namespace gx

volatile uint32_t sms3ds_gx_batches_per_frame;
volatile uint32_t sms3ds_gx_vertices_per_frame;
volatile uint32_t sms3ds_gx_dropped;
volatile float sms3ds_gx_cpu_ms, sms3ds_gx_gpu_ms;
volatile uint32_t sms3ds_gx_tev_approx, sms3ds_gx_approx_why, sms3ds_gx_dump_request;
volatile uint32_t sms3ds_gx_hud[3];
volatile uint32_t sms3ds_gx_skip_image;  // debug builds: draws sampling this texture address are skipped
volatile float sms3ds_prof_frame_ms, sms3ds_prof_flush_ms, sms3ds_prof_xform_ms, sms3ds_prof_tex_ms,
    sms3ds_prof_wait_ms, sms3ds_prof_decode_ms;
volatile float sms3ds_prof_phase_ms[SMS3DS_PROF_PHASES];
volatile float sms3ds_prof_replay_ms, sms3ds_prof_begin_ms, sms3ds_prof_state_ms, sms3ds_prof_idx_ms,
    sms3ds_prof_draw_ms, sms3ds_prof_copy_ms;
volatile uint32_t sms3ds_gx_fresh_verts, sms3ds_gx_replay_verts;
volatile uint32_t sms3ds_gx_count[SMS3DS_CNT_N];
namespace {
u64 s_phaseStart[SMS3DS_PROF_PHASES], s_phaseTicks[SMS3DS_PROF_PHASES];
}
extern "C" void sms3ds_prof_begin(int phase) { s_phaseStart[phase] = svcGetSystemTick(); }
extern "C" void sms3ds_prof_end(int phase) { s_phaseTicks[phase] += svcGetSystemTick() - s_phaseStart[phase]; }

namespace gx3ds {

float g_efbScale = 0.5f, g_efbBar = 40.0f, g_efbTop = 0.0f;  // EFB -> top screen (gx3ds.h)
float g_targetW = kScreenW;  // the width of the screen being drawn: kBottomW while the HUD draws there
namespace {

using namespace gx;

// --- the batch being built: packed GX vertices in the vertex ring, 32-bit indices ---------------------
uint8_t* s_batch;                   // first vertex of the batch in the ring
std::vector<uint8_t> s_scratch;     // where the loader writes when the ring is full (not drawn)
std::vector<uint32_t> s_idx;
uint32_t s_count, s_fmt, s_stride;
PrimClass s_class = PRIM_TRIS;
uint32_t s_tmpIdx[3 * 65536];
bool s_dropping;  // the current primitive went to the scratch buffer

// --- citro3d ---------------------------------------------------------------------------------------
bool s_ready;
C3D_RenderTarget* s_target;
// The bottom screen shows the game's HUD unless the debug display (hud.cpp, L+R+X) is up: then the
// HUD stays on the top screen and the bottom screen is the console's.
C3D_RenderTarget* s_bottom;
bool s_bottomOut;   // s_bottom is the bottom screen's output this frame
bool s_hudBottom;   // drawing the HUD there now (between GXPC_SetHud(1) and (0))
C3D_RenderTarget* drawTarget() { return s_hudBottom ? s_bottom : s_target; }
DVLB_s* s_dvlb;
shaderProgram_s s_prog;  // side bars (passthru.v.pica)
DVLB_s* s_gxDvlb;
shaderProgram_s s_gxProg;  // the GX pipeline (gx.v.pica)
// Per-frame rings in linear memory (the GPU reads them when the frame is submitted): freshly decoded
// vertices (cached display lists draw from the arena instead).
const size_t kVertRing = 2u << 20, kIdxRing = 1u << 20;
uint8_t* s_vertRing;
uint8_t* s_idxRing;
size_t s_vertUsed, s_idxUsed;
// Display-list arena: the vertices and 16-bit indices of cached display-list runs (gx_fifo.cpp's DL
// cache), uploaded once and drawn in place every frame. Filled by bumping; when full it starts over
// at the next frame start (the GPU has finished the previous frame by then) under a new generation,
// so runs upload again as they are drawn.
size_t s_arenaVertCap, s_arenaIdxCap;
uint8_t* s_arenaVerts;
uint8_t* s_arenaIdx;
size_t s_arenaVertUsed, s_arenaIdxUsed;
uint32_t s_arenaGen = 1;
bool s_arenaFull;
uint32_t s_arenaResets;
bool s_inFrame;
bool s_clearPending = true;
u32 s_clearColor = 0x000000FF, s_clearDepth = 0xFFFFFF;
uint32_t s_frameBatches, s_frameVerts;
bool s_lastPixelValid;  // the pixel state set up for the previous batch still holds (flushBatch)
// bumped by every state write (onStateChange) and new EFB copy: unchanged, the previous batch's pixel
// state is known to hold without comparing a snapshot
uint32_t s_stateVersion = 1, s_pixelVersion;
uint32_t s_frameWhy;    // AX_* approximations made this frame
bool s_efbDirty = true; // drawn or cleared since the last snapshot for a texture copy

// Per-frame profile (system ticks, 268 MHz), published to the HUD at each display copy.
u64 s_tFlush, s_tXform, s_tTex, s_tWait, s_lastPresent;
// finer split for the HUD: cached display-list runs copied in (outside flushes), write-backs and frame
// start, pixel state (snapshot, blend/depth, TEV), index conversion, draw calls
u64 s_tReplay, s_tBegin, s_tState, s_tIdx, s_tDraw, s_tCopy;  // copy: EFB texture copies
uint32_t s_freshVerts, s_replayVerts;  // xform: vertex set-up (attributes, uniforms)
double s_decodeAtPresent;
inline float ticksMs(u64 t) { return (float)((double)t * 1000.0 / SYSCLOCK_ARM11); }

const u32 kTransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
u32 s_bottomFlags;  // the same into the bottom screen's format (the console's RGB565)

void freeRetired();
void storeWriteBacks();

void beginFrame()
{
	if (s_inFrame)
		return;
	const u64 t0 = svcGetSystemTick();
	const double flush0 = g_flushSeconds;
	freeRetired();  // outside a frame (citro3d's rule); waits for the GPU to finish using them
	const bool bottomOut = s_bottom && !sms3ds_hud_debug_shown;
	if (bottomOut != s_bottomOut) {  // outside a frame too
		C3D_RenderTargetSetOutput(bottomOut ? s_bottom : nullptr, GFX_BOTTOM, GFX_LEFT, s_bottomFlags);
		s_bottomOut = bottomOut;
	}
	// No C3D_FRAME_SYNCDRAW: that also waits for the next vblank, and the game already paces itself
	// on its own retrace timer (vi.cpp). Waits only while the GPU still draws the previous frame.
	C3D_FrameBegin(0);
	const u64 tw = svcGetSystemTick();
	s_tWait += tw - t0;
	storeWriteBacks();
	const u64 twb = svcGetSystemTick();
	g_gxCount[SMS3DS_CNT_WBUS] += (uint32_t)((twb - tw) / (SYSCLOCK_ARM11 / 1000000));
	g_gxCount[SMS3DS_CNT_BEGINWAITUS] += (uint32_t)((tw - t0) / (SYSCLOCK_ARM11 / 1000000));
	if (s_bottomOut)  // black where the HUD draws nothing
		C3D_RenderTargetClear(s_bottom, C3D_CLEAR_ALL, 0x000000FF, s_clearDepth);
	C3D_FrameDrawOn(drawTarget());
	s_vertUsed = s_idxUsed = 0;
	if (s_arenaFull) {
		s_arenaGen++;
		s_arenaVertUsed = s_arenaIdxUsed = 0;
		s_arenaFull = false;
		s_arenaResets++;
	}
	if (s_clearPending) {
		C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, s_clearColor, s_clearDepth);
		s_clearPending = false;
		s_efbDirty = true;
	}
	s_inFrame = true;
	// counted as backend time wherever it is first reached (often the frame's first vertex decode)
	const u64 t = svcGetSystemTick() - t0;
	s_tBegin += t;
	s_tFlush += t;
	g_flushSeconds = flush0 + (double)t / SYSCLOCK_ARM11;
}

// GX copy-clear values (BP 0x4F-0x51) as citro3d wants them (RGBA8 0xRRGGBBAA, 24-bit depth).
void readClearValues()
{
	const uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB];
	const uint32_t r = ar & 255, a = (ar >> 8) & 255, gg = (gb >> 8) & 255, b = gb & 255;
	s_clearColor = r << 24 | gg << 16 | b << 8 | a;
	s_clearDepth = g.bp[BP_CLEAR_Z] & 0xFFFFFF;
}

void drawClipRects(const float (*r)[4], int n, const uint8_t rgba[4], float z, bool depth);

// A copy's clear (BP 0x4F-0x51) over the copied rectangle only: the whole target when it covers the
// EFB, else a quad with the clear colour and depth (citro3d clears whole targets).
void clearCopyRect()
{
	const uint32_t tl = g.bp[BP_COPY_SRC_TL], wh = g.bp[BP_COPY_SRC_WH];
	const float x0 = float(tl & 0x3FF), y0 = float((tl >> 10) & 0x3FF);
	const float w = float((wh & 0x3FF) + 1), h = float(((wh >> 10) & 0x3FF) + 1);
	if (x0 <= 0 && y0 <= 0 && w >= 640 && h >= 448) {
		C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, s_clearColor, s_clearDepth);
		return;
	}
	const float r[1][4] = {{efbClipX(y0 + h), efbClipX(y0), efbClipY(x0 + w), efbClipY(x0)}};
	const uint8_t rgba[4] = {uint8_t(s_clearColor >> 24), uint8_t(s_clearColor >> 16), uint8_t(s_clearColor >> 8),
	                         uint8_t(s_clearColor)};
	drawClipRects(r, 1, rgba, -float(s_clearDepth) / 16777215.0f, true);
}

// Per-fragment state from the BP registers (as gx_render.cpp applyGlState).
void applyScissor();

void applyState()
{
	static const GPU_TESTFUNC kCmp[8] = {GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
	                                     GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
	const uint32_t pix = g.bp[BP_PE_CONTROL] & 7;
	const bool hasAlpha = pix == 1;  // GX_PF_RGBA6_Z24
	const uint32_t cm = g.bp[BP_CMODE0];
	int mask = 0;
	if ((cm >> 3) & 1)
		mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
	if (((cm >> 4) & 1) && hasAlpha)
		mask |= GPU_WRITE_ALPHA;
	const uint32_t z = g.bp[BP_ZMODE];
	if (z & 1) {
		if ((z >> 4) & 1)
			mask |= GPU_WRITE_DEPTH;
		C3D_DepthTest(true, kCmp[(z >> 1) & 7], (GPU_WRITEMASK)mask);
	} else {
		C3D_DepthTest(false, GPU_ALWAYS, (GPU_WRITEMASK)mask);
	}

	static const GPU_BLENDFACTOR kSrc[8] = {GPU_ZERO, GPU_ONE, GPU_DST_COLOR, GPU_ONE_MINUS_DST_COLOR,
	                                        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA};
	static const GPU_BLENDFACTOR kDst[8] = {GPU_ZERO, GPU_ONE, GPU_SRC_COLOR, GPU_ONE_MINUS_SRC_COLOR,
	                                        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA};
	auto noDstAlpha = [&](GPU_BLENDFACTOR f) {
		if (hasAlpha)
			return f;
		return f == GPU_DST_ALPHA ? GPU_ONE : f == GPU_ONE_MINUS_DST_ALPHA ? GPU_ZERO : f;
	};
	// constant alpha 0 (CMODE1): the stored alpha is src * 0 + dst * 0 (other values: tev.cpp)
	const uint32_t cm1 = g.bp[BP_CMODE1];
	const bool alphaZero = ((cm1 >> 8) & 1) && (cm1 & 0xFF) == 0;
	if (cm & 1) {
		const bool sub = (cm >> 11) & 1;
		if (sub)
			C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE,
			               alphaZero ? GPU_ZERO : GPU_ONE, alphaZero ? GPU_ZERO : GPU_ONE);
		else {
			GPU_BLENDFACTOR sf = noDstAlpha(kSrc[(cm >> 8) & 7]), df = noDstAlpha(kDst[(cm >> 5) & 7]);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, sf, df, alphaZero ? GPU_ZERO : sf, alphaZero ? GPU_ZERO : df);
		}
	} else {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, alphaZero ? GPU_ZERO : GPU_ONE, GPU_ZERO);
	}
	// GX cull mode (GENMODE): 1 front, 2 back, 3 everything (flushBatch skips the draw). GX front
	// faces are clockwise on screen (sms-pc-port gx_render.cpp); through the rotated top-screen
	// framebuffer they reach the PICA clockwise too (seen in Azahar: the opposite choice drew only the
	// inner faces of closed models and dropped the sky dome and 2D quads).
	const uint32_t cull = (g.bp[BP_GENMODE] >> 14) & 3;
	C3D_CullFace(cull == 1 ? GPU_CULL_FRONT_CCW : cull == 2 ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);
	applyScissor();
}

// The GX scissor (BP 0x20/0x21: EFB coordinates + 342, inclusive), within the picture (out of the
// side bars). Framebuffer coordinates are rotated: x = 240 - screen row, y = width - screen column
// (gx3ds.h). The scissor box offset (BP 0x59) is not applied, as in sms-pc-port.
uint32_t s_scissorTL = ~0u, s_scissorBR = ~0u;  // as last applied

void applyScissor()
{
	const uint32_t tl = g.bp[BP_SCISSOR_TL], br = g.bp[BP_SCISSOR_BR];
	s_scissorTL = tl;
	s_scissorBR = br;
	const float top = float(int(tl & 0x7FF) - 342), left = float(int((tl >> 12) & 0x7FF) - 342);
	const float bottom = float(int(br & 0x7FF) - 341), right = float(int((br >> 12) & 0x7FF) - 341);
	const float s = g_efbScale, W = g_targetW, bar = g_efbBar;
	const float row0 = (top - g_efbTop) * s, row1 = (bottom - g_efbTop) * s;
	const float col0 = bar + left * s, col1 = bar + right * s;
	auto clampTo = [](float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; };
	const u32 x0 = (u32)clampTo(floorf(kScreenH - row1), 0.0f, kScreenH);
	const u32 x1 = (u32)clampTo(ceilf(kScreenH - row0), (float)x0, kScreenH);
	const u32 y0 = (u32)clampTo(floorf(W - col1), bar, W - bar);
	const u32 y1 = (u32)clampTo(ceilf(W - col0), (float)y0, W - bar);
	C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, y0, x1, y1);
}

// Solid rectangles in PICA clip space ({x0, x1, y0, y1}; x along the screen's height, y along its
// width), drawn over everything with the pass-through program.
void drawClipRects(const float (*r)[4], int n, const uint8_t rgba[4], float z = -0.5f, bool depth = false)
{
	if (s_vertUsed + 4 * n * sizeof(OutVertex) > kVertRing || s_idxUsed + 6 * n * sizeof(uint16_t) > kIdxRing)
		return;
	OutVertex* v = (OutVertex*)(s_vertRing + s_vertUsed);
	uint16_t* ix = (uint16_t*)(s_idxRing + s_idxUsed);
	for (int q = 0; q < n; q++) {
		for (int c = 0; c < 4; c++) {
			OutVertex& o = v[q * 4 + c];
			o.pos[0] = r[q][c & 1];
			o.pos[1] = r[q][2 + (c >> 1)];
			o.pos[2] = z;
			o.pos[3] = 1.0f;
			memcpy(o.clr, rgba, 4);
			memset(o.tc, 0, sizeof o.tc);
		}
		const uint16_t k[6] = {0, 1, 2, 1, 3, 2};
		for (int i = 0; i < 6; i++)
			ix[q * 6 + i] = (uint16_t)(q * 4 + k[i]);
	}
	s_vertUsed += (4 * n * sizeof(OutVertex) + 15) & ~(size_t)15;
	s_idxUsed += (6 * n * sizeof(uint16_t) + 15) & ~(size_t)15;
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
	C3D_DepthTest(depth, GPU_ALWAYS, depth ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	for (int st = 1; st < 6; st++) {
		C3D_TexEnv* e = C3D_GetTexEnv(st);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
	}
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, v, sizeof(OutVertex), 5, 0x43210);
	C3D_DrawElements(GPU_TRIANGLES, 6 * n, C3D_UNSIGNED_SHORT, ix);
	// back to the GX program; its uniforms (the passthru program's constant shares them) are resent
	C3D_BindProgram(&s_gxProg);
	vtxInvalidate();
	s_lastPixelValid = false;
}

// The game clears the whole render target (citro3d has no partial clear), so the picture's side
// bars (`bar` screen columns each) are painted black at the end of each frame.
void drawSideBars(float bar)
{
	const float e = 2.0f * bar / kScreenW;
	const float bars[2][4] = {{-1.0f, 1.0f, 1.0f - e, 1.0f}, {-1.0f, 1.0f, -1.0f, -1.0f + e}};
	const uint8_t black[4] = {0, 0, 0, 255};
	drawClipRects(bars, 2, black);
}

// --- EFB texture copies (GXCopyTex) ---------------------------------------------------------------
// The copied rectangle of the render target is drawn into a texture registered by its destination
// address, and textureForMap samples that texture in place of the RAM there until the CPU writes to
// that RAM. The render target cannot be sampled in place (240-pixel rows are not a texture width),
// so GX_TextureCopy first lays its tiles into s_efbTex, a 256 x 512 texture with 16 spare columns.
//
// Orientation: a render target's window row y is memory row H - 1 - y (as the top screen shows it),
// and a texture's coordinate t samples memory row (1 - t) * H, so what is drawn at window row y is
// sampled at t = y / H. EFB pixel (ex, ey) is at window (240 - ey / 2, 360 - ex / 2) (vtx.cpp setFinal).
struct CopyTex {
	C3D_Tex tex;
	C3D_RenderTarget* rt = nullptr;
	uint32_t clock = 0, bytes = 0;
	uint32_t frame = 0;  // display frame of the last copy here
	bool current = false;  // no CPU write to the destination since the copy
	bool writesBack = false;  // RAM receives this copy (the CPU may read it)
	// write-back to RAM (the game reads some copies on the CPU): the copy texture's tiles are
	// moved into `lin` by the GPU at the end of the copy, and encoded into `dest` next frame
	uint8_t* lin = nullptr;
	bool pending = false;
	// compared byte for byte rather than hashed (64-bit hashing is slow on the ARM11: the goop map's
	// two hashes and encode cost 4 ms a frame)
	std::vector<uint8_t> stored;  // the tiles last encoded into `dest` (unchanged: not encoded again)
	std::vector<uint8_t> ram;     // `dest` after the last write-back (ramKnown)
	bool ramKnown = false;
	const void* dest = nullptr;
	uint32_t layout = 0, tw = 0, th = 0;
};
std::unordered_map<const void*, CopyTex> s_copies;
FILE* s_dump;  // the frame dump being written (debug builds; see dumpDue)
// s_copies' destinations as a short array (copyMaps scans it for every draw); rebuilt when stale
const void* s_copyDests[32];
int s_copyDestCount = -1;  // -1: rebuild
std::vector<CopyTex> s_retired;  // replaced while a frame in flight may still sample them
C3D_Tex s_efbTex;
bool s_efbTexOk;
bool s_copyFlipS, s_copyFlipT;  // measured at start-up (calibrateCopies)
bool s_calibrating;              // no write-back: the calibration's destination is a small probe

// Copy textures not copied to for two seconds (their buffers were freed: a new area allocates new
// ones) are released when VRAM runs low; render targets can only be deleted between frames.
void freeRetired()
{
	if (vramSpaceFree() < (2u << 20)) {
		for (auto it = s_copies.begin(); it != s_copies.end();) {
			if (g_displayFrames - it->second.frame > 120) {
				s_retired.push_back(it->second);
				it = s_copies.erase(it);
				s_copyDestCount = -1;
			} else {
				++it;
			}
		}
	}
	for (CopyTex& c : s_retired) {
		C3D_RenderTargetDelete(c.rt);
		C3D_TexDelete(&c.tex);
		if (c.lin)
			linearFree(c.lin);
	}
	s_retired.clear();
}

// Texel (x, y) of a PICA texture w texels wide: 8x8 tiles in rows, Morton order inside a tile.
uint32_t mortonOffset(uint32_t x, uint32_t y, uint32_t w)
{
	uint32_t m = 0;
	for (int b = 0; b < 3; b++)
		m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
	return (((y >> 3) * (w >> 3) + (x >> 3)) << 6) + m;
}

uint32_t potClamp(uint32_t v)
{
	uint32_t p = 8;
	while (p < v && p < 1024)
		p <<= 1;
	return p;
}

// The GX texture layout a copy format writes (sms-pc-port gx_texture.cpp copyLayout).
uint32_t copyLayout(uint32_t f, bool z)
{
	switch (f) {
	case 0: return 0;
	case 2: return 2;
	case 3: case 11: case 12: return 3;
	case 4: return z ? 3 : 4;
	case 5: return 5;
	case 6: return 6;
	default: return 1;
	}
}

// The frame so far laid out as a texture (s_efbTex), unless nothing was drawn since the last time.
void snapshotEfb()
{
	if (!s_efbDirty)
		return;
	const u32 line = kScreenH * 8 * 4 / 16;  // one row of 8x8 RGBA8 tiles, in 16-byte units
	C3D_SyncTextureCopy((u32*)s_target->frameBuf.colorBuf, GX_BUFFER_DIM(line, 0), (u32*)s_efbTex.data,
	                    GX_BUFFER_DIM(line, (256 - kScreenH) * 8 * 4 / 16), kScreenH * kScreenW * 4,
	                    GX_TRANSFER_RAW_COPY(1));
	s_efbDirty = false;
}

// s_efbTex texture coordinates of EFB (ex, ey), as rendered (gx3ds.h) and laid out by snapshotEfb.
void efbTexCoord(float ex, float ey, float tc[2])
{
	const float wx = kScreenH - (ey - g_efbTop) * g_efbScale, wy = (kScreenW - g_efbBar) - ex * g_efbScale;
	tc[0] = wx / 256.0f;
	tc[1] = (wy + (512.0f - kScreenW)) / 512.0f;
}

void copyToTexture(uint32_t ctrl)
{
	const void* dest = g.copyDest;
	if (!dest || !s_efbTexOk)
		return;
	const uint32_t tl = g.bp[BP_COPY_SRC_TL], wh = g.bp[BP_COPY_SRC_WH];
	const float x0 = float(tl & 0x3FF), y0 = float((tl >> 10) & 0x3FF);
	const uint32_t w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
	const bool half = (ctrl >> 9) & 1;
	const uint32_t fmt = ((ctrl >> 3) & 1) << 3 | ((ctrl >> 4) & 7);
	const bool zcopy = (g.bp[BP_PE_CONTROL] & 7) == 3;
	const bool alphaOne = (g.bp[BP_PE_CONTROL] & 7) != 1;  // no alpha in the EFB: copies read 1
	const uint32_t tw = half ? (w + 1) / 2 : w, th = half ? (h + 1) / 2 : h;
	// the copy at the render target's resolution (half the EFB's), as a power-of-two texture
	const uint32_t pw = potClamp((w + 1) / 2), ph = potClamp((h + 1) / 2);

	CopyTex& c = s_copies[dest];
	s_copyDestCount = -1;
	if (c.rt && (c.tex.width != pw || c.tex.height != ph)) {
		s_retired.push_back(c);  // it owns the textures and the write-back buffer now
		c.rt = nullptr;
		c.lin = nullptr;
		c.pending = false;
		c.stored.clear();
		c.ramKnown = false;
	}
	if (!c.rt) {
		if (!C3D_TexInitVRAM(&c.tex, (u16)pw, (u16)ph, GPU_RGBA8)) {
			fprintf(stderr, "[gx3ds] texture copy %ux%u to %p: no VRAM (%lu KiB free)\n", (unsigned)pw, (unsigned)ph,
			        dest, (unsigned long)(vramSpaceFree() >> 10));
			s_copies.erase(dest);
			s_copyDestCount = -1;
			return;
		}
		c.rt = C3D_RenderTargetCreateFromTex(&c.tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
		if (!c.rt) {
			C3D_TexDelete(&c.tex);
			s_copies.erase(dest);
			s_copyDestCount = -1;
			return;
		}
		fprintf(stderr, "[gx3ds] texture copy (%g,%g) %ux%u fmt %u%s%s to %p: %ux%u texture\n", (double)x0, (double)y0,
		        (unsigned)w, (unsigned)h, (unsigned)fmt, zcopy ? " depth" : "", half ? " half" : "", dest,
		        (unsigned)pw, (unsigned)ph);
	}
	if (s_dump)
		fprintf(s_dump, "\ncopy (%g,%g) %ux%u fmt %u%s%s clear %u to %p\n", (double)x0, (double)y0, (unsigned)w,
		        (unsigned)h, (unsigned)fmt, zcopy ? " depth" : "", half ? " half" : "", (unsigned)((ctrl >> 11) & 1), dest);
	c.bytes = gxTexBytes(copyLayout(fmt, zcopy), tw, th);
	c.clock = writeClock();
	c.frame = g_displayFrames;
	c.current = true;

	if (zcopy) {  // depth copies: far everywhere for now
		C3D_RenderTargetClear(c.rt, C3D_CLEAR_COLOR, 0xFFFFFFFF, 0);
		s_frameWhy |= AX_COPY;
		return;
	}
	// what the destination format keeps: one channel (I4/I8 layouts read it as I = A), I + A, or colour
	const bool intensity = ((ctrl >> 15) & 3) == 3;
	const uint32_t layout = copyLayout(fmt, zcopy);
	enum { CH_R, CH_G, CH_B, CH_A, CH_Y, CH_RGB } ch = CH_RGB;
	if (layout <= 3) {
		if (intensity && fmt <= 3)
			ch = CH_Y;
		else if (fmt == 7)
			ch = CH_A;
		else if (fmt == 9 || fmt == 12)
			ch = CH_G;
		else if (fmt == 10)
			ch = CH_B;
		else
			ch = CH_R;
	}
	const bool oneChannel = layout <= 1;  // I4 / I8: alpha is the same value

	if (s_vertUsed + 4 * sizeof(OutVertex) > kVertRing || s_idxUsed + 6 * sizeof(uint16_t) > kIdxRing)
		return;
	snapshotEfb();

	OutVertex* v = (OutVertex*)(s_vertRing + s_vertUsed);
	uint16_t* ix = (uint16_t*)(s_idxRing + s_idxUsed);
	for (int i = 0; i < 4; i++) {
		const float X = (i & 1) ? 1.0f : -1.0f, Y = (i & 2) ? 1.0f : -1.0f;
		float sx = (X + 1.0f) * 0.5f, ty = (Y + 1.0f) * 0.5f;  // the copy's (s, t) there
		if (s_copyFlipS)
			sx = 1.0f - sx;
		if (s_copyFlipT)
			ty = 1.0f - ty;
		const float ex = x0 + sx * w, ey = y0 + ty * h;
		OutVertex& o = v[i];
		o.pos[0] = X;
		o.pos[1] = Y;
		o.pos[2] = -0.5f;
		o.pos[3] = 1.0f;
		memset(o.clr, 255, 4);
		memset(o.tc, 0, sizeof o.tc);
		efbTexCoord(ex, ey, o.tc[0]);
	}
	const uint16_t q[6] = {0, 1, 2, 1, 3, 2};
	memcpy(ix, q, sizeof q);
	s_vertUsed += (4 * sizeof(OutVertex) + 15) & ~(size_t)15;
	s_idxUsed += (6 * sizeof(uint16_t) + 15) & ~(size_t)15;

	C3D_FrameDrawOn(c.rt);
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	C3D_TexSetFilter(&s_efbTex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_efbTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	C3D_TexBind(0, &s_efbTex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	int first = 1;
	if (ch == CH_Y) {
		// Y = 0.257 R + 0.504 G + 0.098 B + 16/255 (GX's RGB -> Y). DOT3 gives 4 * sum((t - 0.5)(k - 0.5)),
		// so k = 0.5 + w / 4 yields sum(w t) - sum(w) / 2; the second stage adds that back with the 16.
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_DOT3_RGBA);
		C3D_TexEnvColor(env, 144u | 160u << 8 | 134u << 16 | 255u << 24);
		C3D_TexEnv* e1 = C3D_GetTexEnv(1);
		C3D_TexEnvInit(e1);
		C3D_TexEnvSrc(e1, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e1, C3D_Both, GPU_ADD);
		C3D_TexEnvColor(e1, 0x7D7D7D7Du);  // 0.4295 + 16/255 = 0.492
		first = 2;
	} else {
		static const GPU_TEVOP_RGB kRgb[4] = {GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B,
		                                      GPU_TEVOP_RGB_SRC_ALPHA};
		static const GPU_TEVOP_A kA[4] = {GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B,
		                                  GPU_TEVOP_A_SRC_ALPHA};
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
		C3D_TexEnvOpRgb(env, ch == CH_RGB ? GPU_TEVOP_RGB_SRC_COLOR : kRgb[ch], GPU_TEVOP_RGB_SRC_COLOR,
		                GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
		const bool alphaFromChannel = oneChannel && ch != CH_RGB;
		const bool alphaConst = !alphaFromChannel && (alphaOne || ch == CH_A);
		C3D_TexEnvSrc(env, C3D_Alpha, alphaConst ? GPU_CONSTANT : GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
		C3D_TexEnvOpAlpha(env, alphaFromChannel ? kA[ch] : GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA,
		                  GPU_TEVOP_A_SRC_ALPHA);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(env, 0xFFFFFFFF);
	}
	for (int st = first; st < 6; st++) {
		C3D_TexEnv* e = C3D_GetTexEnv(st);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
	}
	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, v, sizeof(OutVertex), 5, 0x43210);
	C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ix);
	C3D_FrameDrawOn(drawTarget());
	C3D_BindProgram(&s_gxProg);
	vtxInvalidate();
	s_lastPixelValid = false;

	// Write-back for copies the CPU may read: small ones (the sun's visibility check) and the
	// one-channel ones (the goop maps, GX_CTF_R8, read by TPollutionLayer::isPolluted). The large
	// colour copies (the screen texture, the mirror) are only ever sampled by the GPU; encoding them
	// on the CPU every frame cost more than 10 ms.
	c.writesBack = !s_calibrating && c.bytes && (c.bytes <= (64u << 10) || layout <= 1);
	if (c.writesBack) {
		const size_t size = c.tex.size;
		if (!c.lin)
			c.lin = (uint8_t*)linearAlloc(size);
		if (c.lin) {
			const u32 line = (u32)c.tex.width * 8 * 4 / 16;  // one row of tiles, in 16-byte units
			C3D_SyncTextureCopy((u32*)c.tex.data, GX_BUFFER_DIM(line, 0), (u32*)c.lin, GX_BUFFER_DIM(line, 0),
			                    (u32)size, GX_TRANSFER_RAW_COPY(1));
			c.pending = true;
			c.dest = dest;
			c.layout = layout;
			c.tw = tw;
			c.th = th;
		}
	}
}

// The rows the display copy takes (GXSetDispCopySrc: 0-447 in game) fill the screen. Everything is
// drawn at half the EFB's resolution (gx3ds.h), which keeps EFB copies exact (the goop map goes
// through the EFB every frame and is only round at exactly 0.5); at the end of the frame the picture
// is stretched from its 224 framebuffer lines to the screen's 240, keeping its shape between the
// side bars.
float s_dispTop = 0.0f, s_dispRows = 480.0f;

void presentPicture()
{
	const float rows = s_dispRows, top = s_dispTop;
	const float P = kScreenH / rows, bar = (kScreenW - 640.0f * P) * 0.5f;
	const bool stretch = !(top == 0.0f && rows >= 480.0f);
	if (stretch && s_vertUsed + 4 * sizeof(OutVertex) <= kVertRing && s_idxUsed + 6 * sizeof(uint16_t) <= kIdxRing) {
		snapshotEfb();
		OutVertex* v = (OutVertex*)(s_vertRing + s_vertUsed);
		uint16_t* ix = (uint16_t*)(s_idxRing + s_idxUsed);
		for (int i = 0; i < 4; i++) {
			const float ex = (i & 1) ? 640.0f : 0.0f, ey = (i & 2) ? top + rows : top;
			OutVertex& o = v[i];
			o.pos[0] = 1.0f - 2.0f * P * (ey - top) / kScreenH;
			o.pos[1] = 1.0f - 2.0f * (bar + ex * P) / kScreenW;
			o.pos[2] = -0.5f;
			o.pos[3] = 1.0f;
			memset(o.clr, 255, 4);
			memset(o.tc, 0, sizeof o.tc);
			efbTexCoord(ex, ey, o.tc[0]);
		}
		const uint16_t q[6] = {0, 1, 2, 1, 3, 2};
		memcpy(ix, q, sizeof q);
		s_vertUsed += (4 * sizeof(OutVertex) + 15) & ~(size_t)15;
		s_idxUsed += (6 * sizeof(uint16_t) + 15) & ~(size_t)15;

		C3D_FrameDrawOn(s_target);
		C3D_BindProgram(&s_prog);
		C3D_AttrInfo* ai = C3D_GetAttrInfo();
		AttrInfo_Init(ai);
		AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
		AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
		AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
		AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
		AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
		C3D_CullFace(GPU_CULL_NONE);
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		C3D_TexSetFilter(&s_efbTex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&s_efbTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		C3D_TexBind(0, &s_efbTex);
		for (int st = 0; st < 6; st++) {
			C3D_TexEnv* e = C3D_GetTexEnv(st);
			C3D_TexEnvInit(e);
			if (st == 0) {
				C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
				C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
			} else {
				C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
				C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
			}
		}
		C3D_BufInfo* bi = C3D_GetBufInfo();
		BufInfo_Init(bi);
		BufInfo_Add(bi, v, sizeof(OutVertex), 5, 0x43210);
		C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ix);
		C3D_BindProgram(&s_gxProg);
		vtxInvalidate();
		s_lastPixelValid = false;
	}
	drawSideBars(stretch ? bar : g_efbBar);
}

std::vector<uint32_t> s_wbRgba;
std::vector<uint8_t> s_wbDirty;  // storeWriteBacks: changed tiles of the copy texture  // a write-back's copy texture untiled: GX row order, 0xAABBGGRR

// A write-back (the copy texture's tiles in c.lin) encoded into `out` in the GX layout c.layout
// (gx_texture.cpp copyLayout), resampled from the copy texture's size to the copy's.
// Only the GX tiles reading a changed 8x8 tile of the copy texture (bit per tile in `dirty`, tiles
// in memory order) are encoded when `dirty` is given; the rest of `out` is left as it is.
void encodeCopy(const CopyTex& c, uint8_t* out, const std::vector<uint8_t>* dirty = nullptr)
{
	const uint32_t L = c.layout, w = c.tw, h = c.th, pw = c.tex.width, ph = c.tex.height;
	static const uint8_t kTw[7] = {8, 8, 8, 4, 4, 4, 4}, kTh[7] = {8, 4, 4, 4, 4, 4, 4};
	const uint32_t tileW = kTw[L], tileH = kTh[L], cols = (w + tileW - 1) / tileW, rows = (h + tileH - 1) / tileH;
	// untile once: row y of the GX image is memory row ph - 1 - y; tiles hold 8x8 in Morton order
	s_wbRgba.resize((size_t)pw * ph);
	const uint32_t* src = (const uint32_t*)c.lin;
	for (uint32_t y = 0; y < ph; y++) {
		const uint32_t my = ph - 1 - y, rowBase = (my >> 3) * (pw >> 3) * 64;
		uint32_t* dst = &s_wbRgba[(size_t)y * pw];
		for (uint32_t x = 0; x < pw; x++) {
			const uint32_t m = (x & 1) | (my & 1) << 1 | (x & 2) << 1 | (my & 2) << 2 | (x & 4) << 2 | (my & 4) << 3;
			const uint32_t v = src[rowBase + (x >> 3) * 64 + m];  // bytes A, B, G, R
			dst[x] = __builtin_bswap32(v);                        // -> R, G, B, A in memory order
		}
	}
	std::vector<uint32_t> colOf(w);
	for (uint32_t x = 0; x < w; x++)
		colOf[x] = x * pw / w;
	const uint32_t tilesX = pw >> 3;
	// is any copy-texture tile under GX texels [x0, x0 + tileW) x [y0, y0 + tileH) dirty?
	auto changed = [&](uint32_t x0, uint32_t y0) {
		if (!dirty)
			return true;
		const uint32_t xa = colOf[x0 < w ? x0 : w - 1] >> 3, xb = colOf[x0 + tileW - 1 < w ? x0 + tileW - 1 : w - 1] >> 3;
		const uint32_t ya = y0 < h ? y0 : h - 1, yb = y0 + tileH - 1 < h ? y0 + tileH - 1 : h - 1;
		// GX row y is memory row ph - 1 - y * ph / h
		const uint32_t ma = (ph - 1 - yb * ph / h) >> 3, mb = (ph - 1 - ya * ph / h) >> 3;
		for (uint32_t my = ma; my <= mb; my++)
			for (uint32_t mx = xa; mx <= xb; mx++)
				if ((*dirty)[my * tilesX + mx])
					return true;
		return false;
	};
	const uint32_t tileBytes = L == 6 ? 64 : 32;
	auto texel = [&](uint32_t x, uint32_t y, uint8_t* t) {  // r, g, b, a of GX texel (x, y)
		if (x >= w || y >= h) {
			memset(t, 0, 4);
			return;
		}
		memcpy(t, &s_wbRgba[(size_t)(y * ph / h) * pw + colOf[x]], 4);
	};
	auto n4 = [](uint8_t v) { return (uint32_t)(v * 15 + 127) / 255; };
	uint8_t* p = out;
	uint8_t t[4], u[4];
	for (uint32_t ty = 0; ty < rows; ty++)
		for (uint32_t tx = 0; tx < cols; tx++) {
			const uint32_t x0 = tx * tileW, y0 = ty * tileH;
			if (!changed(x0, y0)) {
				p += tileBytes;
				continue;
			}
			switch (L) {
			case 0:  // I4
				for (uint32_t i = 0; i < 64; i += 2) {
					texel(x0 + (i & 7), y0 + (i >> 3), t);
					texel(x0 + ((i + 1) & 7), y0 + ((i + 1) >> 3), u);
					*p++ = (uint8_t)(n4(t[0]) << 4 | n4(u[0]));
				}
				break;
			case 1:  // I8
				for (uint32_t i = 0; i < 32; i++) {
					texel(x0 + (i & 7), y0 + (i >> 3), t);
					*p++ = t[0];
				}
				break;
			case 2:  // IA4
				for (uint32_t i = 0; i < 32; i++) {
					texel(x0 + (i & 7), y0 + (i >> 3), t);
					*p++ = (uint8_t)(n4(t[3]) << 4 | n4(t[0]));
				}
				break;
			case 3:  // IA8
				for (uint32_t i = 0; i < 16; i++) {
					texel(x0 + (i & 3), y0 + (i >> 2), t);
					*p++ = t[3];
					*p++ = t[0];
				}
				break;
			case 4:  // RGB565
				for (uint32_t i = 0; i < 16; i++) {
					texel(x0 + (i & 3), y0 + (i >> 2), t);
					const uint32_t v = (t[0] >> 3) << 11 | (t[1] >> 2) << 5 | (t[2] >> 3);
					*p++ = (uint8_t)(v >> 8);
					*p++ = (uint8_t)v;
				}
				break;
			case 5:  // RGB5A3
				for (uint32_t i = 0; i < 16; i++) {
					texel(x0 + (i & 3), y0 + (i >> 2), t);
					uint32_t v;
					if (t[3] >= 0xE0)
						v = 0x8000 | (t[0] >> 3) << 10 | (t[1] >> 3) << 5 | (t[2] >> 3);
					else
						v = (uint32_t)(t[3] >> 5) << 12 | (t[0] >> 4) << 8 | (t[1] >> 4) << 4 | (t[2] >> 4);
					*p++ = (uint8_t)(v >> 8);
					*p++ = (uint8_t)v;
				}
				break;
			default:  // RGBA8: AR plane, then GB plane
				for (uint32_t i = 0; i < 16; i++) {
					texel(x0 + (i & 3), y0 + (i >> 2), t);
					p[2 * i] = t[3];
					p[2 * i + 1] = t[0];
					p[32 + 2 * i] = t[1];
					p[32 + 2 * i + 1] = t[2];
				}
				p += 64;
				break;
			}
		}
}

// The previous frame's write-backs, now that the GPU has finished it (beginFrame).
void storeWriteBacks()
{
	for (auto& kv : s_copies) {
		CopyTex& c = kv.second;
		if (!c.pending)
			continue;
		c.pending = false;
		// the CPU changed RAM there after the copy (stamped and different from what was stored): on
		// the GameCube its edit lands after the copy, so it wins, and RAM becomes the texture again
		if (c.ramKnown && !unwrittenSince(c.dest, c.bytes, c.clock) &&
		    (c.ram.size() != c.bytes || memcmp(c.dest, c.ram.data(), c.bytes))) {
			c.current = false;
			c.ramKnown = false;
			continue;
		}
		GSPGPU_InvalidateDataCache(c.lin, c.tex.size);
		// 8x8 tiles of the copy texture (256 bytes each) that differ from what was last encoded
		const size_t tiles = c.tex.size / 256;
		const bool all = !c.ramKnown || c.stored.size() != c.tex.size;
		s_wbDirty.assign(tiles, all ? 1 : 0);
		uint32_t nd = all ? (uint32_t)tiles : 0;
		if (!all)
			for (size_t i = 0; i < tiles; i++)
				if (memcmp(&c.stored[i * 256], c.lin + i * 256, 256)) {
					s_wbDirty[i] = 1;
					nd++;
				}
		if (nd) {
			c.stored.assign(c.lin, c.lin + c.tex.size);
			encodeCopy(c, (uint8_t*)const_cast<void*>(c.dest), all ? nullptr : &s_wbDirty);
			g_gxCount[SMS3DS_CNT_WBTILES] += nd;
			c.ram.assign((const uint8_t*)c.dest, (const uint8_t*)c.dest + c.bytes);
		}
		c.ramKnown = true;
		c.clock = writeClock();
	}
}

// Where a copied texel lands is measured rather than trusted: a red quad over the EFB's top-left
// quadrant goes through copyToTexture, and the copy texture is read back to see which quadrant
// came out red. A copy whose red is elsewhere sets s_copyFlipS / s_copyFlipT.

void calibrateCopies()
{
	static uint8_t probe[64];
	const uint32_t saved[3] = {g.bp[BP_COPY_SRC_TL], g.bp[BP_COPY_SRC_WH], g.bp[BP_PE_CONTROL]};
	void* savedDest = g.copyDest;
	beginFrame();  // clears to black
	const float quad[1][4] = {{efbClipX(240.0f), efbClipX(0.0f), efbClipY(320.0f), efbClipY(0.0f)}};  // EFB x < 320, y < 240
	const uint8_t red[4] = {255, 0, 0, 255};
	drawClipRects(quad, 1, red);
	s_efbDirty = true;
	g.bp[BP_COPY_SRC_TL] = 0;
	g.bp[BP_COPY_SRC_WH] = 639u | 479u << 10;
	g.bp[BP_PE_CONTROL] = 0;
	g.copyDest = probe;
	s_calibrating = true;
	copyToTexture(6u << 4);  // RGBA8
	s_calibrating = false;
	C3D_FrameEnd(0);
	s_inFrame = false;
	C3D_FrameBegin(0);  // waits for the GPU
	C3D_FrameEnd(0);
	auto it = s_copies.find(probe);
	int found = -1;
	if (it != s_copies.end()) {
		C3D_Tex& t = it->second.tex;
		GSPGPU_InvalidateDataCache(t.data, t.size);
		const uint32_t pw = t.width, ph = t.height;
		for (int q = 0; q < 4; q++) {  // GX quadrant q: (s, t) = (0.25 or 0.75, 0.25 or 0.75)
			const uint32_t x = (q & 1) ? pw * 3 / 4 : pw / 4, y = (q & 2) ? ph * 3 / 4 : ph / 4;
			const uint8_t* d = (const uint8_t*)t.data + 4 * mortonOffset(x, ph - 1 - y, pw);
			if (d[3] > 128 && d[2] < 64 && d[1] < 64)  // RGBA8 is stored A, B, G, R
				found = found < 0 ? q : 4;
		}
		s_retired.push_back(it->second);
		s_copies.erase(it);
		s_copyDestCount = -1;
	}
	if (found >= 0 && found < 4) {
		s_copyFlipS = found & 1;
		s_copyFlipT = (found & 2) != 0;
	}
	fprintf(stderr, "[gx3ds] copy orientation: red quadrant %d%s, flip s %d t %d\n", found,
	        found < 0 || found > 3 ? " (not measured: copies keep the computed orientation)" : "", s_copyFlipS,
	        s_copyFlipT);
	g.bp[BP_COPY_SRC_TL] = saved[0];
	g.bp[BP_COPY_SRC_WH] = saved[1];
	g.bp[BP_PE_CONTROL] = saved[2];
	g.copyDest = savedDest;
	s_clearPending = true;
}

// --- frame dump: the GX state of each distinct batch of one frame -------------------------
std::unordered_set<uint64_t> s_dumpSeen;
uint32_t s_dumpBatches;

// A dump is taken 10 s (300 display frames) after entering each area, once per area, on sms3ds_gx_dump_request, or
// when the file sdmc:/3ds/sms-3ds/dump exists (checked every second; removed when taken). Debug
// builds only: a release never dumps.
bool dumpDue()
{
#if !SMS3DS_DEBUG
	return false;
#endif
	static int lastArea = -1, lastState = -1;
	static uint32_t since;
	static uint64_t dumpedAreas;
	if (sms3ds_area != lastArea || sms3ds_state != lastState) {
		lastArea = sms3ds_area;
		lastState = sms3ds_state;
		since = 0;
	}
	since++;
	if (sms3ds_gx_dump_request)
		return true;
	if (since == 300 && sms3ds_state == 5 && sms3ds_area >= 0 && sms3ds_area < 64 &&
	    !(dumpedAreas & (1ull << sms3ds_area))) {
		dumpedAreas |= 1ull << sms3ds_area;
		return true;
	}
	if (g_displayFrames % 30 == 0 && remove("sdmc:/3ds/sms-3ds/dump") == 0)
		return true;
	return false;
}

float xff(int reg)
{
	float f;
	memcpy(&f, &g.xfReg[reg], 4);
	return f;
}

void dumpBatch(const TevPlan& plan, uint32_t why)
{
	s_dumpBatches++;
	uint32_t key[160];
	int n = 0;
	for (int r = 0x09; r <= 0x12; r++)
		key[n++] = g.xfReg[r];
	key[n++] = g.xfReg[XFR_NUMTEXGENS];
	for (int i = 0; i < 8; i++)
		key[n++] = g.xfReg[XFR_TEXGEN + i];
	for (int r = 0; r < 0x100; r++)
		if ((r >= 0xC0 && r < 0x100) || r == BP_GENMODE || (r >= BP_ZMODE && r <= BP_PE_CONTROL) || r == 0x28 ||
		    r == 0x29 || r == 0x2A || r == 0x2B)
			if (n < 150)
				key[n++] = g.bp[r];
	key[n++] = s_fmt;
	for (int u = 0; u < plan.units; u++) {
		key[n++] = g.bp[bpTexReg(BP_TX_IMAGE0, plan.map[u])];
		key[n++] = (uint32_t)(uintptr_t)g.texImage[plan.map[u]];
	}
	const uint64_t h = hashBytes(key, (size_t)n * 4);
	if (!s_dumpSeen.insert(h).second)
		return;
	FILE* f = s_dump;
	fprintf(f, "\nbatch %lu: fmt %05lx stride %lu verts %lu why %05lx\n", (unsigned long)s_dumpBatches,
	        (unsigned long)s_fmt, (unsigned long)s_stride, (unsigned long)s_count, (unsigned long)why);
	fprintf(f, " xf chans %lu c0 %04lx a0 %04lx c1 %04lx a1 %04lx amb0 %08lx mat0 %08lx amb1 %08lx mat1 %08lx\n",
	        (unsigned long)g.xfReg[XFR_NUMCHANS], (unsigned long)g.xfReg[XFR_COLOR0CTRL],
	        (unsigned long)g.xfReg[XFR_ALPHA0CTRL], (unsigned long)g.xfReg[XFR_COLOR0CTRL + 1],
	        (unsigned long)g.xfReg[XFR_ALPHA0CTRL + 1], (unsigned long)g.xfReg[XFR_AMB0],
	        (unsigned long)g.xfReg[XFR_MAT0], (unsigned long)g.xfReg[XFR_AMB0 + 1], (unsigned long)g.xfReg[XFR_MAT0 + 1]);
	const uint32_t ntg = g.xfReg[XFR_NUMTEXGENS] & 15;
	fprintf(f, " texgens %lu:", (unsigned long)ntg);
	for (uint32_t i = 0; i < ntg && i < 8; i++)
		fprintf(f, " %06lx", (unsigned long)g.xfReg[XFR_TEXGEN + i]);
	fprintf(f, "  matidx %08lx %08lx\n", (unsigned long)g.xfReg[XFR_MATIDX_A], (unsigned long)g.xfReg[XFR_MATIDX_B]);
	const uint32_t gen = g.bp[BP_GENMODE], nst = ((gen >> 10) & 15) + 1;
	fprintf(f, " genmode %06lx stages %lu ind %lu zmode %06lx cmode0 %06lx cmode1 %06lx pe %06lx alpha %06lx fog3 %06lx\n",
	        (unsigned long)gen, (unsigned long)nst, (unsigned long)((gen >> 16) & 7), (unsigned long)g.bp[BP_ZMODE],
	        (unsigned long)g.bp[BP_CMODE0], (unsigned long)g.bp[BP_CMODE1], (unsigned long)g.bp[BP_PE_CONTROL],
	        (unsigned long)g.bp[BP_ALPHACOMPARE], (unsigned long)g.bp[BP_FOG3]);
	fprintf(f, " vp %g %g %g %g %g %g proj%s %g %g %g %g %g %g scissor %06lx %06lx\n", (double)xff(XFR_VIEWPORT),
	        (double)xff(XFR_VIEWPORT + 1), (double)xff(XFR_VIEWPORT + 2), (double)xff(XFR_VIEWPORT + 3),
	        (double)xff(XFR_VIEWPORT + 4), (double)xff(XFR_VIEWPORT + 5), (g.xfReg[XFR_PROJ + 6] & 1) ? " ortho" : "",
	        (double)xff(XFR_PROJ), (double)xff(XFR_PROJ + 1), (double)xff(XFR_PROJ + 2), (double)xff(XFR_PROJ + 3),
	        (double)xff(XFR_PROJ + 4), (double)xff(XFR_PROJ + 5), (unsigned long)g.bp[BP_SCISSOR_TL],
	        (unsigned long)g.bp[BP_SCISSOR_BR]);
	if ((gen >> 16) & 7) {
		fprintf(f, " ind: iref %06lx ss %06lx %06lx mtx", (unsigned long)g.bp[0x27], (unsigned long)g.bp[0x25],
		        (unsigned long)g.bp[0x26]);
		for (int r = 0x06; r <= 0x0E; r++)
			fprintf(f, " %06lx", (unsigned long)(g.bp[r] & 0xFFFFFF));
		fprintf(f, " cmd");
		for (uint32_t st = 0; st < nst; st++)
			fprintf(f, " %06lx", (unsigned long)(g.bp[0x10 + st] & 0xFFFFFF));
		fprintf(f, "\n");
	}
	for (uint32_t st = 0; st < nst; st++) {
		const uint32_t ord = (g.bp[BP_TREF + (st >> 1)] >> ((st & 1) * 12)) & 0xFFF;
		const uint32_t ks = g.bp[BP_TEV_KSEL + (st >> 1)];
		const uint32_t ksel = (st & 1) ? ((ks >> 14) & 0x3FF) : ((ks >> 4) & 0x3FF);
		fprintf(f, "  stage %lu: order %03lx color %06lx alpha %06lx kc %02lx ka %02lx\n", (unsigned long)st,
		        (unsigned long)ord, (unsigned long)(g.bp[BP_TEV_COLOR_ENV + 2 * st] & 0xFFFFFF),
		        (unsigned long)(g.bp[BP_TEV_COLOR_ENV + 2 * st + 1] & 0xFFFFFF), (unsigned long)(ksel & 31),
		        (unsigned long)(ksel >> 5));
	}
	fprintf(f, " regs");
	for (int i = 0; i < 8; i++)
		fprintf(f, " %06lx", (unsigned long)(g.bp[BP_TEV_REG + i] & 0xFFFFFF));
	fprintf(f, "\n konst");
	for (int i = 0; i < 8; i++)
		fprintf(f, " %06lx", (unsigned long)(g.kreg[i] & 0xFFFFFF));
	fprintf(f, "\n swap");
	for (int i = 0; i < 8; i++)
		fprintf(f, " %06lx", (unsigned long)(g.bp[BP_TEV_KSEL + i] & 0xFFFFFF));
	fprintf(f, "\n");
	for (int u = 0; u < plan.units; u++) {
		const int m = plan.map[u];
		const uint32_t img0 = g.bp[bpTexReg(BP_TX_IMAGE0, m)];
		const void* p = g.texImage[m];
		fprintf(f, "  unit %d: map %d coord %d %lux%lu fmt %lu mode0 %06lx at %p%s\n", u, m, plan.coord[u],
		        (unsigned long)((img0 & 0x3FF) + 1), (unsigned long)(((img0 >> 10) & 0x3FF) + 1),
		        (unsigned long)((img0 >> 20) & 15), (unsigned long)g.bp[bpTexReg(BP_TX_MODE0, m)], p,
		        copyTexture(p) ? " (EFB copy)" : "");
	}
}

// The BP state applyState and compileTev read. Consecutive batches often differ only in XF (matrices),
// so when this snapshot matches the previous batch's, the combiners, blending, depth and alpha test
// set up for it still hold and are not rebuilt; other snapshots find their compiled TEV in s_tevCache.
constexpr int kPixelWords = 7 + 32 + 8 + 8 + 8 + 8 + 4 + 1;  // must match snapshotPixel
struct PixelState {
	uint32_t w[kPixelWords];
	bool operator==(const PixelState& o) const { return memcmp(w, o.w, sizeof w) == 0; }
};
PixelState s_lastPixel;
const TevPlan* s_lastPlan;  // in s_tevCache (valid until the next tevPlanFor)

// Compiled TEV plans by pixel state (a frame at the plaza changes state ~400 times between a few
// hundred distinct states; compiling took ~10 us each). Open addressing; cleared when 3/4 full.
struct TevCacheEntry {
	bool used;
	uint32_t hash;
	PixelState key;
	TevPlan plan;
};
constexpr uint32_t kTevCache = 1024;
TevCacheEntry* s_tevCache;
uint32_t s_tevCacheUsed;

uint32_t hashPixel(const PixelState& p)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < kPixelWords; i++)
		h = (h ^ p.w[i]) * 16777619u;
	return h ^ (h >> 15);
}

const TevPlan& tevPlanFor(const PixelState& ps)
{
	if (!s_tevCache)
		s_tevCache = (TevCacheEntry*)calloc(kTevCache, sizeof(TevCacheEntry));
	const uint32_t h = hashPixel(ps);
	uint32_t i = h & (kTevCache - 1);
	for (; s_tevCache[i].used; i = (i + 1) & (kTevCache - 1))
		if (s_tevCache[i].hash == h && s_tevCache[i].key == ps)
			return s_tevCache[i].plan;
	if (s_tevCacheUsed >= kTevCache * 3 / 4) {
		memset(s_tevCache, 0, kTevCache * sizeof(TevCacheEntry));
		s_tevCacheUsed = 0;
		i = h & (kTevCache - 1);
	}
	TevCacheEntry& e = s_tevCache[i];
	e.used = true;
	e.hash = h;
	e.key = ps;
	compileTev(e.plan, ps.w[kPixelWords - 1]);
	s_tevCacheUsed++;
	g_gxCount[SMS3DS_CNT_TEVCOMPILE]++;
	return e.plan;
}

// GX maps (bit per map) whose current texture is an EFB copy.
uint32_t copyMaps()
{
	if (s_copyDestCount < 0) {
		s_copyDestCount = 0;
		for (auto& kv : s_copies)
			if (s_copyDestCount < 32)
				s_copyDests[s_copyDestCount++] = kv.first;
	}
	uint32_t m = 0;
	for (int i = 0; i < 8; i++) {
		const void* t = g.texImage[i];
		if (!t)
			continue;
		bool maybe = s_copies.size() > 32;  // more than the array holds: copyTexture looks it up
		for (int k = 0; k < s_copyDestCount && !maybe; k++)
			maybe = s_copyDests[k] == t;
		if (maybe && copyTexture(t))
			m |= 1u << i;
	}
	return m;
}

void snapshotPixel(PixelState& p, uint32_t maps)
{
	int n = 0;
	p.w[n++] = g.bp[BP_GENMODE];
	p.w[n++] = g.bp[BP_ZMODE];
	p.w[n++] = g.bp[BP_CMODE0];
	p.w[n++] = g.bp[BP_CMODE1];
	p.w[n++] = g.bp[BP_PE_CONTROL];
	p.w[n++] = g.bp[BP_ALPHACOMPARE];
	p.w[n++] = g.bp[BP_FOG3];
	for (int i = 0; i < 32; i++)
		p.w[n++] = g.bp[BP_TEV_COLOR_ENV + i];
	for (int i = 0; i < 8; i++)
		p.w[n++] = g.bp[BP_TREF + i];
	for (int i = 0; i < 8; i++)
		p.w[n++] = g.bp[BP_TEV_KSEL + i];
	for (int i = 0; i < 8; i++)
		p.w[n++] = g.bp[BP_TEV_REG + i];
	for (int i = 0; i < 8; i++)
		p.w[n++] = g.kreg[i];
	p.w[n++] = g.xfReg[XFR_NUMCHANS];  // colour channel 1, read by setupTev
	p.w[n++] = g.xfReg[XFR_COLOR0CTRL + 1];
	p.w[n++] = g.xfReg[XFR_ALPHA0CTRL + 1];
	p.w[n++] = g.xfReg[XFR_MAT0 + 1];
	p.w[n++] = maps;  // copyMaps(): EFB-copy textures take compares by arithmetic, not baking
	if (n != kPixelWords)
		svcBreak(USERBREAK_PANIC);
}

void resetBatch()
{
	s_count = 0;
	s_batch = nullptr;
	s_idx.clear();
}

// Room for `n` more vertices of `stride` bytes in the current batch, in the ring.
uint8_t* batchRoom(uint32_t n, uint32_t stride)
{
	beginFrame();
	if (!s_batch)
		s_batch = s_vertRing + s_vertUsed;
	const size_t end = (size_t)(s_batch - s_vertRing) + (size_t)(s_count + n) * stride;
	if (end > kVertRing) {
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
		if (s_scratch.size() < (size_t)n * stride)
			s_scratch.resize((size_t)n * stride);
		return nullptr;
	}
	return s_batch + (size_t)s_count * stride;
}

}  // namespace

C3D_Tex* copyTexture(const void* ptr)
{
	if (!ptr || s_copies.empty())
		return nullptr;
	auto it = s_copies.find(ptr);
	if (it == s_copies.end())
		return nullptr;
	CopyTex& c = it->second;
	if (c.current && !unwrittenSince(ptr, c.bytes, c.clock)) {
		// pages there were flushed since; stamps are per page, so neighbours' flushes count too:
		// when RAM still holds exactly what was written back, the copy stays the texture; one not
		// written back (RAM never held it) stays the texture while the game keeps copying it
		if (c.ramKnown ? c.ram.size() == c.bytes && !memcmp(ptr, c.ram.data(), c.bytes) : !c.writesBack && g_displayFrames - c.frame <= 2)
			c.clock = writeClock();
		else
			c.current = false;  // the CPU wrote there: RAM is the texture (kept for the next copy)
	}
	return c.current ? &c.tex : nullptr;
}

}  // namespace gx3ds

namespace gx {
using namespace gx3ds;

void rendererInit(float)
{
	if (s_ready)
		return;
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);  // uniform uploads per batch (skinned models) take room
	s_target = C3D_RenderTargetCreate(kScreenH, kScreenW, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, kTransferFlags);
	s_bottom = C3D_RenderTargetCreate(kScreenH, kBottomW, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	// GSP framebuffer formats and transfer formats share their numbering
	s_bottomFlags = (kTransferFlags & ~GX_TRANSFER_OUT_FORMAT(7)) |
	                GX_TRANSFER_OUT_FORMAT((GX_TRANSFER_FORMAT)gfxGetScreenFormat(GFX_BOTTOM));
	s_dvlb = DVLB_ParseFile((u32*)gx3ds_passthru_shbin, gx3ds_passthru_shbin_size);
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_gxDvlb = DVLB_ParseFile((u32*)gx3ds_gx_shbin, gx3ds_gx_shbin_size);
	shaderProgramInit(&s_gxProg);
	shaderProgramSetVsh(&s_gxProg, &s_gxDvlb->DVLE[0]);
	C3D_BindProgram(&s_gxProg);
	vtxInit(&s_gxProg);
	s_vertRing = (uint8_t*)linearAlloc(kVertRing);
	s_idxRing = (uint8_t*)linearAlloc(kIdxRing);
	for (size_t mb = 16; mb >= 4 && !s_arenaVerts; mb -= 2) {  // what linear memory allows
		s_arenaVerts = (uint8_t*)linearAlloc(mb << 20);
		s_arenaIdx = s_arenaVerts ? (uint8_t*)linearAlloc(mb << 18) : nullptr;
		if (s_arenaVerts && !s_arenaIdx) {
			linearFree(s_arenaVerts);
			s_arenaVerts = nullptr;
		}
		if (s_arenaVerts) {
			s_arenaVertCap = mb << 20;
			s_arenaIdxCap = mb << 18;  // up to ~6 bytes of 16-bit indices per vertex of ~50
		}
	}
	s_efbTexOk = C3D_TexInitVRAM(&s_efbTex, 256, 512, GPU_RGBA8);
	s_ready = s_target && s_vertRing && s_idxRing;
	if (s_ready && s_efbTexOk)
		calibrateCopies();
	fprintf(stderr, "[gx3ds] citro3d %s: 400x240 RGBA8 target, %u KiB vertex ring, %u KiB display-list arena, "
	        "linear free %lu KiB, vram free %lu KiB\n",
	        s_ready ? "ready" : "FAILED", (unsigned)(kVertRing >> 10), (unsigned)(s_arenaVertCap >> 10),
	        (unsigned long)(linearSpaceFree() >> 10),
	        (unsigned long)(vramSpaceFree() >> 10));
}

bool rendererReady() { return s_ready; }
void onStateChange()
{
	s_stateVersion++;
	flushBatch();
}
void markXfMemDirty() { g_xfMemVersion++; }

PrimClass primClass(uint8_t op) { return op >= 0xB8 ? PRIM_POINTS : op >= 0xA8 ? PRIM_LINES : PRIM_TRIS; }

uint8_t* primitiveBegin(uint8_t op, uint32_t n, uint32_t fmt, uint32_t stride)
{
	s_freshVerts += n;
	PrimClass cls = primClass(op);
	if (s_count && (cls != s_class || fmt != s_fmt || s_count + n > 65535))
		flushBatch();
	s_class = cls;
	s_fmt = fmt;
	s_stride = stride;
	uint8_t* room = batchRoom(n, stride);
	s_dropping = !room;
	return room ? room : s_scratch.data();
}

void primitiveEnd(uint8_t op, uint32_t n)
{
	if (!n)
		return;
	sms3ds_gx_primitives = sms3ds_gx_primitives + 1;
	sms3ds_gx_vertices = sms3ds_gx_vertices + n;
	if (s_class != PRIM_TRIS || s_dropping) {  // lines and points: not drawn yet (triangles only)
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
		return;
	}
	uint32_t ni = primitiveIndices(op, n, s_count, s_tmpIdx);
	s_idx.insert(s_idx.end(), s_tmpIdx, s_tmpIdx + ni);
	s_count += n;
}

uint32_t primitiveIndices(uint8_t op, uint32_t n, uint32_t base, uint32_t* w)
{
	// as sms-pc-port's gx_render.cpp: quads, strips and fans become triangle lists
	uint32_t* const w0 = w;
	auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
		w[0] = base + a;
		w[1] = base + b;
		w[2] = base + c;
		w += 3;
	};
	switch (op & 0xF8) {
	case 0x80:
	case 0x88:
		for (uint32_t i = 0; i + 3 < n; i += 4) {
			tri(i, i + 1, i + 2);
			tri(i, i + 2, i + 3);
		}
		break;
	case 0x90:
		for (uint32_t i = 0; i + 2 < n; i += 3)
			tri(i, i + 1, i + 2);
		break;
	case 0x98:
		for (uint32_t i = 2; i < n; i++) {
			if (i & 1)
				tri(i - 1, i - 2, i);
			else
				tri(i - 2, i - 1, i);
		}
		break;
	case 0xA0:
		for (uint32_t i = 2; i < n; i++)
			tri(0, i - 1, i);
		break;
	case 0xA8:
		for (uint32_t i = 0; i + 1 < n; i += 2) {
			*w++ = base + i;
			*w++ = base + i + 1;
		}
		break;
	case 0xB0:
		for (uint32_t i = 1; i < n; i++) {
			*w++ = base + i - 1;
			*w++ = base + i;
		}
		break;
	default:
		for (uint32_t i = 0; i < n; i++)
			*w++ = base + i;
		break;
	}
	return (uint32_t)(w - w0);
}

void appendDecoded(PrimClass cls, uint32_t fmt, uint32_t stride, const uint8_t* verts, uint32_t count,
                   const uint32_t* idx, uint32_t nidx)
{
	sms3ds_gx_primitives = sms3ds_gx_primitives + 1;
	sms3ds_gx_vertices = sms3ds_gx_vertices + count;
	if (cls != PRIM_TRIS) {
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
		return;
	}
	if (s_count && (cls != s_class || fmt != s_fmt || s_count + count > 65535))
		flushBatch();
	if (count > 65535) {
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
		return;
	}
	const u64 t0 = svcGetSystemTick(), f0 = s_tFlush;
	s_replayVerts += count;
	s_class = cls;
	s_fmt = fmt;
	s_stride = stride;
	uint8_t* room = batchRoom(count, stride);
	if (room) {
		memcpy(room, verts, (size_t)count * stride);
		for (uint32_t i = 0; i < nidx; i++)
			s_idx.push_back(s_count + idx[i]);
		s_count += count;
	}
	s_tReplay += svcGetSystemTick() - t0 - (s_tFlush - f0);
}

namespace {
// Draws `nidx` indices at `idx` (GPU-readable) over vertices at `base` in the current batch format,
// with the current GX state.
// The game's HUD (GXPC_SetHud: TGCConsole2 and its particles): what it costs per frame.
bool s_hud;
uint32_t s_hudBatches, s_hudVerts;
u64 s_hudTicks;

void drawWith(const uint8_t* base, const uint16_t* idx, uint32_t nidx, uint32_t nverts)
{
	const u64 tHud = svcGetSystemTick();
	const u64 ts = svcGetSystemTick();
	// unchanged unless something was written since, or a map's EFB copy went stale (copyTexture
	// decides that on use, without a state write)
	const uint32_t maps = copyMaps();
	if (!s_lastPixelValid || s_pixelVersion != s_stateVersion || maps != s_lastPixel.w[kPixelWords - 1]) {
		PixelState ps;
		snapshotPixel(ps, maps);
		if (!s_lastPixelValid || !(ps == s_lastPixel)) {
			applyState();
			s_lastPlan = &tevPlanFor(ps);
			applyTev(*s_lastPlan);
			g_gxCount[SMS3DS_CNT_PIXEL]++;
			s_lastPixel = ps;
			s_lastPixelValid = true;
		} else if (g.bp[BP_SCISSOR_TL] != s_scissorTL || g.bp[BP_SCISSOR_BR] != s_scissorBR) {
			applyScissor();
		}
		s_pixelVersion = s_stateVersion;
	}
	const TevPlan& plan = *s_lastPlan;
	const u64 tx0 = svcGetSystemTick();
	s_tState += tx0 - ts;
	const bool ok = vtxSetup(plan, s_fmt, s_stride, base);
	const uint32_t why = plan.why | g_vtxWhy;
	if (why)
		sms3ds_gx_tev_approx = sms3ds_gx_tev_approx + 1;
	s_frameWhy |= why;
	if (s_dump)
		dumpBatch(plan, why);
	const u64 tx1 = svcGetSystemTick();
	for (int u = 0; u < plan.units; u++) {
		// unit 0 divides its coordinates by texcoord0w (1 unless the texgen is projective, gx.v.pica)
		C3D_Tex* t = textureForMap(plan.map[u], plan.cmp[u]);
		t->param = (t->param & ~GPU_TEXTURE_MODE(7)) | GPU_TEXTURE_MODE(u == 0 ? GPU_TEX_PROJECTION : GPU_TEX_2D);
		C3D_TexBind(u, t);
		g_gxCount[SMS3DS_CNT_TEXBIND]++;
	}
	const u64 tx2 = svcGetSystemTick();
	s_tXform += tx1 - tx0;
	s_tTex += tx2 - tx1;
	bool skip = false;
#if SMS3DS_DEBUG
	// debugging: skip draws that sample sms3ds_gx_skip_image (written from outside, e.g. azahar.py)
	for (int u = 0; u < plan.units && sms3ds_gx_skip_image; u++)
		skip |= (uint32_t)(uintptr_t)g.texImage[plan.map[u]] == sms3ds_gx_skip_image;
#endif
	if (ok && !skip && ((g.bp[BP_GENMODE] >> 14) & 3) != 3) {
		const u64 td = svcGetSystemTick();
		C3D_DrawElements(GPU_TRIANGLES, (int)nidx, C3D_UNSIGNED_SHORT, idx);
		s_tDraw += svcGetSystemTick() - td;
		s_efbDirty = true;
	}
	else
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
	s_frameBatches++;
	s_frameVerts += nverts;
	if (s_hud) {
		s_hudBatches++;
		s_hudVerts += nverts;
		s_hudTicks += svcGetSystemTick() - tHud;
	}
}

void endFlush(u64 tStart)
{
	const u64 t = svcGetSystemTick() - tStart;
	s_tFlush += t;
	g_flushSeconds += (double)t / SYSCLOCK_ARM11;  // gx_fifo.cpp takes flushes out of its decode time
}
}  // namespace

bool arenaEnabled() { return s_arenaVerts != nullptr; }
uint32_t arenaGeneration() { return s_arenaGen; }

bool arenaUpload(const uint8_t* verts, uint32_t count, uint32_t stride, const uint32_t* idx, uint32_t nidx,
                 ArenaRef& out)
{
	if (!s_arenaVerts || s_arenaFull || count > 65536)
		return false;
	const size_t vbytes = ((size_t)count * stride + 15) & ~(size_t)15;
	const size_t ibytes = ((size_t)nidx * 2 + 15) & ~(size_t)15;
	if (s_arenaVertUsed + vbytes > s_arenaVertCap || s_arenaIdxUsed + ibytes > s_arenaIdxCap) {
		s_arenaFull = true;  // drawn by copying until the next frame starts the arena over
		if (s_arenaResets < 3 || (s_arenaResets & 255) == 0)
			fprintf(stderr, "[gx3ds] display-list arena full (%u KiB vertices), starting over (%lu times)\n",
			        (unsigned)(s_arenaVertUsed >> 10), (unsigned long)s_arenaResets + 1);
		return false;
	}
	uint8_t* v = s_arenaVerts + s_arenaVertUsed;
	uint16_t* ix = (uint16_t*)(s_arenaIdx + s_arenaIdxUsed);
	memcpy(v, verts, (size_t)count * stride);
	for (uint32_t i = 0; i < nidx; i++)
		ix[i] = (uint16_t)idx[i];
	GSPGPU_FlushDataCache(v, vbytes);
	GSPGPU_FlushDataCache(ix, ibytes);
	out.gen = s_arenaGen;
	out.baseVertex = (int32_t)s_arenaVertUsed;  // here: byte offset of the run's vertices
	out.iOff = s_arenaIdxUsed;
	s_arenaVertUsed += vbytes;
	g_gxCount[SMS3DS_CNT_ARENAKB] += (uint32_t)(vbytes >> 10);
	s_arenaIdxUsed += ibytes;
	return true;
}

// A cached run drawn in place from the arena, as its own draw after the batch built so far.
void appendArena(PrimClass cls, uint32_t fmt, uint32_t stride, const ArenaRef& ref, uint32_t count, uint32_t nidx,
                 const uint8_t*)
{
	sms3ds_gx_primitives = sms3ds_gx_primitives + 1;
	sms3ds_gx_vertices = sms3ds_gx_vertices + count;
	s_replayVerts += count;
	if (cls != PRIM_TRIS) {
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
		return;
	}
	flushBatch();
	beginFrame();
	const u64 tStart = svcGetSystemTick();
	s_class = cls;
	s_fmt = fmt;
	s_stride = stride;
	drawWith(s_arenaVerts + ref.baseVertex, (const uint16_t*)(s_arenaIdx + ref.iOff), nidx, count);
	endFlush(tStart);
}

// Arena runs are drawn as they come, not kept in a batch.
bool batchUsesArena() { return false; }

void flushBatch()
{
	if (!s_ready || s_idx.empty() || !s_count || !s_batch) {
		resetBatch();
		return;
	}
	beginFrame();
	const u64 tStart = svcGetSystemTick(), tb = tStart;
	const size_t vbytes = (size_t)s_count * s_stride, ibytes = s_idx.size() * sizeof(uint16_t);
	if (s_idxUsed + ibytes > kIdxRing) {
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
		resetBatch();
		return;
	}
	uint16_t* oi = (uint16_t*)(s_idxRing + s_idxUsed);
	for (size_t i = 0; i < s_idx.size(); i++)
		oi[i] = (uint16_t)s_idx[i];
	s_vertUsed = (size_t)(s_batch - s_vertRing) + ((vbytes + 15) & ~(size_t)15);
	s_idxUsed += (ibytes + 15) & ~(size_t)15;
	s_tIdx += svcGetSystemTick() - tb;
	drawWith(s_batch, oi, (uint32_t)s_idx.size(), s_count);
	resetBatch();
	endFlush(tStart);
}

void executeCopy(uint32_t ctrl)
{
	flushBatch();
	s_stateVersion++;  // copyMaps() may change
	const bool disp = (ctrl >> 14) & 1, clear = (ctrl >> 11) & 1;
	if (disp) {
		// the rows this display copy takes: presentPicture fills the screen with them
		{
			const uint32_t tl = g.bp[BP_COPY_SRC_TL], wh = g.bp[BP_COPY_SRC_WH];
			const float rows = float(((wh >> 10) & 0x3FF) + 1);
			if (rows >= 64.0f) {
				s_dispTop = float((tl >> 10) & 0x3FF);
				s_dispRows = rows;
			}
		}
		g_displayFrames++;
		sms3ds_gx_frames = sms3ds_gx_frames + 1;
		if (s_inFrame) {
			presentPicture();  // also paints the side bars
			// the CPU wrote this frame's vertices and indices through the data cache
			GSPGPU_FlushDataCache(s_vertRing, s_vertUsed);
			GSPGPU_FlushDataCache(s_idxRing, s_idxUsed);
			C3D_FrameEnd(0);
			s_inFrame = false;
			sms3ds_gx_cpu_ms = C3D_GetProcessingTime();
			sms3ds_gx_gpu_ms = C3D_GetDrawingTime();
		}
		const u64 now = svcGetSystemTick();
		if (s_lastPresent) {
			sms3ds_prof_frame_ms = ticksMs(now - s_lastPresent);
			sms3ds_prof_flush_ms = ticksMs(s_tFlush);
			sms3ds_prof_xform_ms = ticksMs(s_tXform);
			sms3ds_prof_tex_ms = ticksMs(s_tTex);
			sms3ds_prof_wait_ms = ticksMs(s_tWait);
			sms3ds_prof_decode_ms = (float)((g_decodeSeconds - s_decodeAtPresent) * 1000.0);
			for (int i = 0; i < SMS3DS_PROF_PHASES; i++)
				sms3ds_prof_phase_ms[i] = ticksMs(s_phaseTicks[i]);
			sms3ds_prof_replay_ms = ticksMs(s_tReplay);
			sms3ds_prof_begin_ms = ticksMs(s_tBegin);
			sms3ds_prof_state_ms = ticksMs(s_tState);
			sms3ds_prof_idx_ms = ticksMs(s_tIdx);
			sms3ds_prof_draw_ms = ticksMs(s_tDraw);
			sms3ds_prof_copy_ms = ticksMs(s_tCopy);
			sms3ds_gx_fresh_verts = s_freshVerts;
			sms3ds_gx_replay_verts = s_replayVerts;
			for (int i = 0; i < SMS3DS_CNT_N; i++)
				sms3ds_gx_count[i] = g_gxCount[i];
		}
		s_lastPresent = now;
		s_decodeAtPresent = g_decodeSeconds;
		s_tFlush = s_tXform = s_tTex = s_tWait = 0;
		memset(s_phaseTicks, 0, sizeof s_phaseTicks);
		s_tReplay = s_tBegin = s_tState = s_tIdx = s_tDraw = s_tCopy = 0;
		s_freshVerts = s_replayVerts = 0;
		memset(g_gxCount, 0, sizeof g_gxCount);
		sms3ds_gx_batches_per_frame = s_frameBatches;
		sms3ds_gx_vertices_per_frame = s_frameVerts;
		sms3ds_gx_approx_why = s_frameWhy;
		s_frameBatches = s_frameVerts = s_frameWhy = 0;
		sms3ds_gx_hud[0] = s_hudBatches;
		sms3ds_gx_hud[1] = s_hudVerts;
		sms3ds_gx_hud[2] = (uint32_t)(s_hudTicks * 1000000ull / SYSCLOCK_ARM11);
		s_hudBatches = s_hudVerts = 0;
		s_hudTicks = 0;
		if (s_dump) {
			fclose(s_dump);
			s_dump = nullptr;
			fprintf(stderr, "[gx3ds] frame dump: %lu batches, %lu distinct, in frame-dump.txt\n",
			        (unsigned long)s_dumpBatches, (unsigned long)s_dumpSeen.size());
		} else if (dumpDue()) {
			sms3ds_gx_dump_request = 0;
			char name[64];
			snprintf(name, sizeof name, "sdmc:/3ds/sms-3ds/frame-dump-area%d.txt", sms3ds_area);
			s_dump = fopen(name, "w");
			fprintf(stderr, "[gx3ds] frame dump of area %d%s\n", sms3ds_area, s_dump ? "" : ": cannot open the file");
			s_dumpSeen.clear();
			s_dumpBatches = 0;
			if (s_dump)
				fprintf(s_dump, "sms-3ds frame dump (display frame %lu)\n", (unsigned long)g_displayFrames);
		}
		if (clear) {
			readClearValues();
			s_clearPending = true;
		}
	} else {
		sms3ds_gx_copies = sms3ds_gx_copies + 1;
		beginFrame();
		const u64 t0 = svcGetSystemTick();
		copyToTexture(ctrl);
		if (clear) {
			readClearValues();
			clearCopyRect();
			s_efbDirty = true;
		}
		s_tCopy += svcGetSystemTick() - t0;
	}
}

uint32_t peekColor(int, int) { return 0; }
uint32_t peekZ(int, int) { return 0xFFFFFF; }  // far plane: nothing occludes (lens flares, sun)
void pixMetricClear() {}
uint32_t pixMetricRead() { return 0; }

void textureInvalidateAll() {}

FILE* traceFile() { return NULL; }
void traceNoteBegin() {}
void traceNoteProjection(const float*) {}

}  // namespace gx

extern "C" {
void GXPC_MemoryWritten(const void* p, uint32_t size);  // gx_fifo.cpp: stamps the pages written

// The game's data-cache flushes (DCFlushRange, DCStoreRange: platform/os/os_core.cpp) say the CPU
// rewrote memory: its pages are stamped, so cached textures and EFB copies there are re-checked.
void GXPC_InvalidateRange(const void* p, uint32_t size) { GXPC_MemoryWritten(p, size); }

int GXPC_InitAuto(float efbScale)
{
	gx::rendererInit(efbScale);
	return gx::rendererReady() ? 1 : 0;
}
void GXPC_SetStretch2D(int) {}
void GXPC_SetWide2D(int) {}
void GXPC_SetHud(int on)
{
	// called by the game: done on the GX thread, in stream order (gxthread.cpp PRIV_HUD)
	if (gx::gxAsync() && !gx::gxqOnGxThread()) {
		const uint32_t a = (uint32_t)on;
		gx::streamPrivate(gx::PRIV_HUD, &a, sizeof a, nullptr, 0);
		return;
	}
	gx::flushBatch();
	gx::s_hud = on != 0;
	using namespace gx3ds;
	const bool bottom = gx::s_hud && s_bottomOut;
	if (bottom == s_hudBottom)
		return;
	s_hudBottom = bottom;
	g_targetW = bottom ? kBottomW : kScreenW;
	g_efbBar = bottom ? 0.0f : 40.0f;
	g_efbTop = bottom ? -16.0f : 0.0f;
	if (s_inFrame)
		C3D_FrameDrawOn(drawTarget());
	vtxInvalidate();
	s_lastPixelValid = false;
}
void GXPC_HudPaneBegin(float, float) {}
void GXPC_HudPaneEnd(void) {}
}
