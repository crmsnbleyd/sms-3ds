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
bool g_gxStats = true;  // gx_fifo.cpp times its vertex loader (g_decodeSeconds) for the HUD
extern double g_decodeSeconds;
double g_flushSeconds;
uint32_t g_traceLastVat;
}  // namespace gx

volatile uint32_t sms3ds_gx_batches_per_frame;
volatile uint32_t sms3ds_gx_vertices_per_frame;
volatile uint32_t sms3ds_gx_dropped;
volatile float sms3ds_gx_cpu_ms, sms3ds_gx_gpu_ms;
volatile uint32_t sms3ds_gx_tev_approx, sms3ds_gx_approx_why, sms3ds_gx_dump_request;
volatile float sms3ds_prof_frame_ms, sms3ds_prof_flush_ms, sms3ds_prof_xform_ms, sms3ds_prof_tex_ms,
    sms3ds_prof_wait_ms, sms3ds_prof_decode_ms;

namespace gx3ds {
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
DVLB_s* s_dvlb;
shaderProgram_s s_prog;  // side bars (passthru.v.pica)
DVLB_s* s_gxDvlb;
shaderProgram_s s_gxProg;  // the GX pipeline (gx.v.pica)
// Per-frame rings in linear memory (the GPU reads them when the frame is submitted).
const size_t kVertRing = 6u << 20, kIdxRing = 1u << 20;
uint8_t* s_vertRing;
uint8_t* s_idxRing;
size_t s_vertUsed, s_idxUsed;
bool s_inFrame;
bool s_clearPending = true;
u32 s_clearColor = 0x000000FF, s_clearDepth = 0xFFFFFF;
uint32_t s_frameBatches, s_frameVerts;
bool s_lastPixelValid;  // the pixel state set up for the previous batch still holds (flushBatch)
uint32_t s_frameWhy;    // AX_* approximations made this frame
bool s_efbDirty = true; // drawn or cleared since the last snapshot for a texture copy

// Per-frame profile (system ticks, 268 MHz), published to the HUD at each display copy.
u64 s_tFlush, s_tXform, s_tTex, s_tWait, s_lastPresent;  // xform: vertex set-up (attributes, uniforms)
double s_decodeAtPresent;
inline float ticksMs(u64 t) { return (float)((double)t * 1000.0 / SYSCLOCK_ARM11); }

const u32 kTransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

void freeRetired();

void beginFrame()
{
	if (s_inFrame)
		return;
	const u64 t0 = svcGetSystemTick();
	freeRetired();  // outside a frame (citro3d's rule); waits for the GPU to finish using them
	// No C3D_FRAME_SYNCDRAW: that also waits for the next vblank, and the game already paces itself
	// on its own retrace timer (vi.cpp). Waits only while the GPU still draws the previous frame.
	C3D_FrameBegin(0);
	s_tWait += svcGetSystemTick() - t0;
	C3D_FrameDrawOn(s_target);
	s_vertUsed = s_idxUsed = 0;
	if (s_clearPending) {
		C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, s_clearColor, s_clearDepth);
		s_clearPending = false;
		s_efbDirty = true;
	}
	s_inFrame = true;
}

// GX copy-clear values (BP 0x4F-0x51) as citro3d wants them (RGBA8 0xRRGGBBAA, 24-bit depth).
void readClearValues()
{
	const uint32_t ar = g.bp[BP_CLEAR_AR], gb = g.bp[BP_CLEAR_GB];
	const uint32_t r = ar & 255, a = (ar >> 8) & 255, gg = (gb >> 8) & 255, b = gb & 255;
	s_clearColor = r << 24 | gg << 16 | b << 8 | a;
	s_clearDepth = g.bp[BP_CLEAR_Z] & 0xFFFFFF;
}

// Per-fragment state from the BP registers (as gx_render.cpp applyGlState).
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
	if (cm & 1) {
		const bool sub = (cm >> 11) & 1;
		if (sub)
			C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
		else {
			GPU_BLENDFACTOR sf = noDstAlpha(kSrc[(cm >> 8) & 7]), df = noDstAlpha(kDst[(cm >> 5) & 7]);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, sf, df, sf, df);
		}
	} else {
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	}
	// Phase 2a: no culling until the winding through the tilted framebuffer is checked on screen.
	C3D_CullFace(GPU_CULL_NONE);
	// The picture is 4:3 in the middle of the 400-pixel width: keep draws out of the side bars.
	// Framebuffer coordinates are rotated (x along the screen's height, y along its width).
	C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, kLetterbox, kScreenH, kScreenW - kLetterbox);
}

// The game clears the whole render target (citro3d has no partial clear), so the 4:3 picture's side
// bars are painted black at the end of each frame.
void drawSideBars()
{
	if (s_vertUsed + 8 * sizeof(OutVertex) > kVertRing || s_idxUsed + 12 * sizeof(uint16_t) > kIdxRing)
		return;
	OutVertex* v = (OutVertex*)(s_vertRing + s_vertUsed);
	uint16_t* ix = (uint16_t*)(s_idxRing + s_idxUsed);
	// screen-space rectangles (x from -1 left to 1 right, y from 1 top to -1 bottom), tilted as xform.cpp
	const float edge = 2.0f * kLetterbox / kScreenW;
	const float rects[2][2] = {{-1.0f, -1.0f + edge}, {1.0f - edge, 1.0f}};
	for (int r = 0; r < 2; r++) {
		const float xs[2] = {rects[r][0], rects[r][1]}, ys[2] = {1.0f, -1.0f};
		for (int c = 0; c < 4; c++) {
			OutVertex& o = v[r * 4 + c];
			const float sx = xs[c & 1], sy = ys[c >> 1];
			o.pos[0] = sy;
			o.pos[1] = -sx;
			o.pos[2] = -0.5f;
			o.pos[3] = 1.0f;
			memset(o.clr, 0, 3);
			o.clr[3] = 255;
			memset(o.tc, 0, sizeof o.tc);
		}
		const uint16_t q[6] = {0, 1, 2, 1, 3, 2};
		for (int i = 0; i < 6; i++)
			ix[r * 6 + i] = (uint16_t)(r * 4 + q[i]);
	}
	s_vertUsed += (8 * sizeof(OutVertex) + 15) & ~(size_t)15;
	s_idxUsed += (12 * sizeof(uint16_t) + 15) & ~(size_t)15;
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
	C3D_DrawElements(GPU_TRIANGLES, 12, C3D_UNSIGNED_SHORT, ix);
	// back to the GX program; its uniforms (the passthru program's constant shares them) are resent
	C3D_BindProgram(&s_gxProg);
	vtxInvalidate();
	s_lastPixelValid = false;
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
};
std::unordered_map<const void*, CopyTex> s_copies;
std::vector<CopyTex> s_retired;  // replaced while a frame in flight may still sample them
C3D_Tex s_efbTex;
bool s_efbTexOk;

// Copy textures not copied to for two seconds (their buffers were freed: a new area allocates new
// ones) are released when VRAM runs low; render targets can only be deleted between frames.
void freeRetired()
{
	if (vramSpaceFree() < (2u << 20)) {
		for (auto it = s_copies.begin(); it != s_copies.end();) {
			if (g_displayFrames - it->second.frame > 120) {
				s_retired.push_back(it->second);
				it = s_copies.erase(it);
			} else {
				++it;
			}
		}
	}
	for (CopyTex& c : s_retired) {
		C3D_RenderTargetDelete(c.rt);
		C3D_TexDelete(&c.tex);
	}
	s_retired.clear();
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
	if (c.rt && (c.tex.width != pw || c.tex.height != ph)) {
		s_retired.push_back(c);
		c.rt = nullptr;
	}
	if (!c.rt) {
		if (!C3D_TexInitVRAM(&c.tex, (u16)pw, (u16)ph, GPU_RGBA8)) {
			fprintf(stderr, "[gx3ds] texture copy %ux%u to %p: no VRAM (%lu KiB free)\n", (unsigned)pw, (unsigned)ph,
			        dest, (unsigned long)(vramSpaceFree() >> 10));
			s_copies.erase(dest);
			return;
		}
		c.rt = C3D_RenderTargetCreateFromTex(&c.tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
		if (!c.rt) {
			C3D_TexDelete(&c.tex);
			s_copies.erase(dest);
			return;
		}
		fprintf(stderr, "[gx3ds] texture copy (%g,%g) %ux%u fmt %u%s%s to %p: %ux%u texture\n", (double)x0, (double)y0,
		        (unsigned)w, (unsigned)h, (unsigned)fmt, zcopy ? " depth" : "", half ? " half" : "", dest,
		        (unsigned)pw, (unsigned)ph);
	}
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
	// the frame so far, laid out as a texture
	if (s_efbDirty) {
		const u32 line = kScreenH * 8 * 4 / 16;  // one row of 8x8 RGBA8 tiles, in 16-byte units
		C3D_SyncTextureCopy((u32*)s_target->frameBuf.colorBuf, GX_BUFFER_DIM(line, 0), (u32*)s_efbTex.data,
		                    GX_BUFFER_DIM(line, (256 - kScreenH) * 8 * 4 / 16), kScreenH * kScreenW * 4,
		                    GX_TRANSFER_RAW_COPY(1));
		s_efbDirty = false;
	}

	OutVertex* v = (OutVertex*)(s_vertRing + s_vertUsed);
	uint16_t* ix = (uint16_t*)(s_idxRing + s_idxUsed);
	for (int i = 0; i < 4; i++) {
		const float X = (i & 1) ? 1.0f : -1.0f, Y = (i & 2) ? 1.0f : -1.0f;
		const float sx = (X + 1.0f) * 0.5f, ty = (Y + 1.0f) * 0.5f;  // the copy's (s, t) there
		const float ex = x0 + sx * w, ey = y0 + ty * h;
		const float wx = kScreenH - ey * 0.5f, wy = (kScreenW - kLetterbox) - ex * 0.5f;
		OutVertex& o = v[i];
		o.pos[0] = X;
		o.pos[1] = Y;
		o.pos[2] = -0.5f;
		o.pos[3] = 1.0f;
		memset(o.clr, 255, 4);
		memset(o.tc, 0, sizeof o.tc);
		o.tc[0][0] = wx / 256.0f;
		o.tc[0][1] = (wy + (512.0f - kScreenW)) / 512.0f;
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
	C3D_FrameDrawOn(s_target);
	C3D_BindProgram(&s_gxProg);
	vtxInvalidate();
	s_lastPixelValid = false;
}

// --- frame dump (hold L+R+X): the GX state of each distinct batch of one frame -------------------------
FILE* s_dump;
std::unordered_set<uint64_t> s_dumpSeen;
uint32_t s_dumpBatches;

// A dump is taken 10 s (300 display frames) after entering each area, once per area, on L+R+X, or
// when the file sdmc:/3ds/sms-3ds/dump exists (checked every second; removed when taken).
bool dumpDue()
{
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

// The BP state applyState and setupTev read. Consecutive batches often differ only in XF (matrices),
// so when this snapshot matches the previous batch's, the combiners, blending, depth and alpha test
// set up for it still hold and are not rebuilt.
constexpr int kPixelWords = 7 + 32 + 8 + 8 + 8 + 8;  // must match snapshotPixel
struct PixelState {
	uint32_t w[kPixelWords];
	bool operator==(const PixelState& o) const { return memcmp(w, o.w, sizeof w) == 0; }
};
PixelState s_lastPixel;
TevPlan s_lastPlan;

void snapshotPixel(PixelState& p)
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
	if (c.current && !unwrittenSince(ptr, c.bytes, c.clock))
		c.current = false;  // the CPU wrote there since: RAM is the texture (kept for the next copy)
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
	s_efbTexOk = C3D_TexInitVRAM(&s_efbTex, 256, 512, GPU_RGBA8);
	s_ready = s_target && s_vertRing && s_idxRing;
	fprintf(stderr, "[gx3ds] citro3d %s: 400x240 RGBA8 target, %u KiB vertex ring, linear free %lu KiB, vram free %lu KiB\n",
	        s_ready ? "ready" : "FAILED", (unsigned)(kVertRing >> 10), (unsigned long)(linearSpaceFree() >> 10),
	        (unsigned long)(vramSpaceFree() >> 10));
}

bool rendererReady() { return s_ready; }
void onStateChange() { flushBatch(); }
void markXfMemDirty() {}

PrimClass primClass(uint8_t op) { return op >= 0xB8 ? PRIM_POINTS : op >= 0xA8 ? PRIM_LINES : PRIM_TRIS; }

uint8_t* primitiveBegin(uint8_t op, uint32_t n, uint32_t fmt, uint32_t stride)
{
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
	s_class = cls;
	s_fmt = fmt;
	s_stride = stride;
	uint8_t* room = batchRoom(count, stride);
	if (!room)
		return;
	memcpy(room, verts, (size_t)count * stride);
	for (uint32_t i = 0; i < nidx; i++)
		s_idx.push_back(s_count + idx[i]);
	s_count += count;
}

// No display-list arena: cached runs come back through appendDecoded.
bool arenaEnabled() { return false; }
uint32_t arenaGeneration() { return 0; }
bool arenaUpload(const uint8_t*, uint32_t, uint32_t, const uint32_t*, uint32_t, ArenaRef&) { return false; }
void appendArena(PrimClass, uint32_t, uint32_t, const ArenaRef&, uint32_t, uint32_t, const uint8_t*) {}
bool batchUsesArena() { return false; }

void flushBatch()
{
	if (!s_ready || s_idx.empty() || !s_count || !s_batch) {
		resetBatch();
		return;
	}
	const u64 tStart = svcGetSystemTick();
	beginFrame();
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

	PixelState ps;
	snapshotPixel(ps);
	if (!s_lastPixelValid || !(ps == s_lastPixel)) {
		applyState();
		setupTev(s_lastPlan);
		s_lastPixel = ps;
		s_lastPixelValid = true;
	}
	const TevPlan& plan = s_lastPlan;
	const u64 tx0 = svcGetSystemTick();
	const bool ok = vtxSetup(plan, s_fmt, s_stride, s_batch);
	const uint32_t why = plan.why | g_vtxWhy;
	if (why)
		sms3ds_gx_tev_approx = sms3ds_gx_tev_approx + 1;
	s_frameWhy |= why;
	if (s_dump)
		dumpBatch(plan, why);
	const u64 tx1 = svcGetSystemTick();
	for (int u = 0; u < plan.units; u++)
		C3D_TexBind(u, textureForMap(plan.map[u]));
	const u64 tx2 = svcGetSystemTick();
	s_tXform += tx1 - tx0;
	s_tTex += tx2 - tx1;
	if (ok) {
		C3D_DrawElements(GPU_TRIANGLES, (int)s_idx.size(), C3D_UNSIGNED_SHORT, oi);
		s_efbDirty = true;
	}
	else
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
	s_frameBatches++;
	s_frameVerts += s_count;
	resetBatch();
	s_tFlush += svcGetSystemTick() - tStart;
}

void executeCopy(uint32_t ctrl)
{
	flushBatch();
	const bool disp = (ctrl >> 14) & 1, clear = (ctrl >> 11) & 1;
	if (disp) {
		g_displayFrames++;
		sms3ds_gx_frames = sms3ds_gx_frames + 1;
		if (s_inFrame) {
			drawSideBars();
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
		}
		s_lastPresent = now;
		s_decodeAtPresent = g_decodeSeconds;
		s_tFlush = s_tXform = s_tTex = s_tWait = 0;
		sms3ds_gx_batches_per_frame = s_frameBatches;
		sms3ds_gx_vertices_per_frame = s_frameVerts;
		sms3ds_gx_approx_why = s_frameWhy;
		s_frameBatches = s_frameVerts = s_frameWhy = 0;
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
		copyToTexture(ctrl);
		if (clear) {
			readClearValues();
			C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, s_clearColor, s_clearDepth);
			s_efbDirty = true;
		}
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
int GXPC_InitAuto(float efbScale)
{
	gx::rendererInit(efbScale);
	return gx::rendererReady() ? 1 : 0;
}
void GXPC_SetStretch2D(int) {}
void GXPC_SetWide2D(int) {}
void GXPC_SetHud(int) {}
void GXPC_HudPaneBegin(float, float) {}
void GXPC_HudPaneEnd(void) {}
}
