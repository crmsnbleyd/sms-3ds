// GX renderer on citro3d (Phase 2a: geometry and vertex colour). Fills the renderer seam declared
// in sms-pc-port's gx_internal.h; the GX front end (gx_api.cpp, gx_fifo.cpp) is unchanged.
//
// A batch is a run of primitives with unchanged GX state (the front end calls onStateChange before
// any state write), as in sms-pc-port's gx_render.cpp. flushBatch transforms the batch's packed
// vertices on the CPU (xform.cpp) into a per-frame ring in linear memory and draws them with u16
// indices. A display copy (GXCopyDisp) ends the citro3d frame, which shows the render target on the
// top screen; its clear applies when the next frame begins.
#include "gx3ds.h"

#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include <vector>

#include "gx_internal.h"
#include "sms3ds.h"
#include "gx3ds_passthru_shbin.h"

namespace gx {
uint32_t g_displayFrames;
bool g_gxStats;
double g_flushSeconds;
uint32_t g_traceLastVat;
}  // namespace gx

volatile uint32_t sms3ds_gx_batches_per_frame;
volatile uint32_t sms3ds_gx_vertices_per_frame;
volatile uint32_t sms3ds_gx_dropped;
volatile float sms3ds_gx_cpu_ms, sms3ds_gx_gpu_ms;

namespace gx3ds {
namespace {

using namespace gx;

// --- the batch being built (packed GX vertices, 32-bit indices) -------------------------------------
std::vector<uint8_t> s_verts;
std::vector<uint32_t> s_idx;
uint32_t s_count, s_fmt, s_stride;
PrimClass s_class = PRIM_TRIS;
uint32_t s_tmpIdx[3 * 65536];

// --- citro3d ---------------------------------------------------------------------------------------
bool s_ready;
C3D_RenderTarget* s_target;
DVLB_s* s_dvlb;
shaderProgram_s s_prog;
// Per-frame rings in linear memory (the GPU reads them when the frame is submitted).
const size_t kVertRing = 6u << 20, kIdxRing = 1u << 20;
uint8_t* s_vertRing;
uint8_t* s_idxRing;
size_t s_vertUsed, s_idxUsed;
bool s_inFrame;
bool s_clearPending = true;
u32 s_clearColor = 0x000000FF, s_clearDepth = 0xFFFFFF;
uint32_t s_frameBatches, s_frameVerts;

const u32 kTransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                           GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

void beginFrame()
{
	if (s_inFrame)
		return;
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C3D_FrameDrawOn(s_target);
	s_vertUsed = s_idxUsed = 0;
	if (s_clearPending) {
		C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, s_clearColor, s_clearDepth);
		s_clearPending = false;
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
}

void resetBatch()
{
	s_count = 0;
	s_idx.clear();
}

}  // namespace

}  // namespace gx3ds

namespace gx {
using namespace gx3ds;

void rendererInit(float)
{
	if (s_ready)
		return;
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
	s_target = C3D_RenderTargetCreate(kScreenH, kScreenW, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(s_target, GFX_TOP, GFX_LEFT, kTransferFlags);
	s_dvlb = DVLB_ParseFile((u32*)gx3ds_passthru_shbin, gx3ds_passthru_shbin_size);
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	C3D_BindProgram(&s_prog);
	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);          // position
	AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);  // colour
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);          // texcoords, units 0..2
	AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
	s_vertRing = (uint8_t*)linearAlloc(kVertRing);
	s_idxRing = (uint8_t*)linearAlloc(kIdxRing);
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
	size_t need = (size_t)(s_count + n) * stride;
	if (s_verts.size() < need)
		s_verts.resize(need + (need >> 1));
	return s_verts.data() + (size_t)s_count * stride;
}

void primitiveEnd(uint8_t op, uint32_t n)
{
	if (!n)
		return;
	sms3ds_gx_primitives = sms3ds_gx_primitives + 1;
	sms3ds_gx_vertices = sms3ds_gx_vertices + n;
	if (s_class != PRIM_TRIS) {  // lines and points: not drawn yet (PICA has triangles only)
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
	size_t need = (size_t)(s_count + count) * stride;
	if (s_verts.size() < need)
		s_verts.resize(need + (need >> 1));
	memcpy(s_verts.data() + (size_t)s_count * stride, verts, (size_t)count * stride);
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
	if (!s_ready || s_idx.empty() || !s_count) {
		resetBatch();
		return;
	}
	beginFrame();
	const size_t vbytes = (size_t)s_count * sizeof(OutVertex), ibytes = s_idx.size() * sizeof(uint16_t);
	if (s_vertUsed + vbytes > kVertRing || s_idxUsed + ibytes > kIdxRing) {
		sms3ds_gx_dropped = sms3ds_gx_dropped + 1;
		resetBatch();
		return;
	}
	applyState();
	TevPlan plan;
	setupTev(plan);
	OutVertex* ov = (OutVertex*)(s_vertRing + s_vertUsed);
	transform(s_verts.data(), s_count, s_fmt, s_stride, plan.coord, plan.units, ov);
	for (int u = 0; u < plan.units; u++)
		C3D_TexBind(u, textureForMap(plan.map[u]));
	uint16_t* oi = (uint16_t*)(s_idxRing + s_idxUsed);
	for (size_t i = 0; i < s_idx.size(); i++)
		oi[i] = (uint16_t)s_idx[i];
	s_vertUsed += (vbytes + 15) & ~(size_t)15;
	s_idxUsed += (ibytes + 15) & ~(size_t)15;

	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, ov, sizeof(OutVertex), 5, 0x43210);
	C3D_DrawElements(GPU_TRIANGLES, (int)s_idx.size(), C3D_UNSIGNED_SHORT, oi);
	s_frameBatches++;
	s_frameVerts += s_count;
	resetBatch();
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
			C3D_FrameEnd(0);
			s_inFrame = false;
			sms3ds_gx_cpu_ms = C3D_GetProcessingTime();
			sms3ds_gx_gpu_ms = C3D_GetDrawingTime();
		}
		sms3ds_gx_batches_per_frame = s_frameBatches;
		sms3ds_gx_vertices_per_frame = s_frameVerts;
		s_frameBatches = s_frameVerts = 0;
		if (clear) {
			readClearValues();
			s_clearPending = true;
		}
	} else {
		sms3ds_gx_copies = sms3ds_gx_copies + 1;  // texture copies: Phase 2d
		if (clear && s_inFrame) {
			readClearValues();
			C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, s_clearColor, s_clearDepth);
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
