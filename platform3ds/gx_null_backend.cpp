// Null renderer for Phase 1: the GX front end (gx_api.cpp register encoding, gx_fifo.cpp command and
// vertex parsing, both from sms-pc-port) runs as on the PC, and this file fills the renderer seam
// declared in gx_internal.h with no-ops that only count what would have been drawn.
// Phase 2 replaces it with a citro3d backend behind the same seam.
// primClass/primitiveIndices follow sms-pc-port's gx_render.cpp (MIT, see NOTICE).
#include "gx_internal.h"
#include "sms_gx/gx_pc.h"

#include "sms3ds.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace gx {

uint32_t g_displayFrames;
bool g_gxStats;
double g_flushSeconds;
uint32_t g_traceLastVat;

namespace {
std::vector<uint8_t> s_verts;  // room primitiveBegin hands out; the loader decodes into it
}

// --- batches ------------------------------------------------------------------------------------
void rendererInit(float) {}
bool rendererReady() { return true; }
void flushBatch() {}
void onStateChange() {}
void markXfMemDirty() {}

PrimClass primClass(uint8_t op) { return op >= 0xB8 ? PRIM_POINTS : op >= 0xA8 ? PRIM_LINES : PRIM_TRIS; }

uint8_t* primitiveBegin(uint8_t, uint32_t n, uint32_t, uint32_t stride)
{
	size_t need = (size_t)n * stride;
	if (s_verts.size() < need)
		s_verts.resize(need);
	return s_verts.data();
}

void primitiveEnd(uint8_t, uint32_t n)
{
	if (n) {
		sms3ds_gx_primitives = sms3ds_gx_primitives + 1;
		sms3ds_gx_vertices = sms3ds_gx_vertices + n;
	}
}

uint32_t primitiveIndices(uint8_t op, uint32_t n, uint32_t base, uint32_t* w)
{
	uint32_t* const w0 = w;
	auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
		w[0] = base + a;
		w[1] = base + b;
		w[2] = base + c;
		w += 3;
	};
	switch (op & 0xF8) {
	case 0x80:
	case 0x88:  // quads
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

void appendDecoded(PrimClass, uint32_t, uint32_t, const uint8_t*, uint32_t count, const uint32_t*, uint32_t)
{
	sms3ds_gx_primitives = sms3ds_gx_primitives + 1;
	sms3ds_gx_vertices = sms3ds_gx_vertices + count;
}

// No display-list arena: the front end then replays cached runs through appendDecoded.
bool arenaEnabled() { return false; }
uint32_t arenaGeneration() { return 0; }
bool arenaUpload(const uint8_t*, uint32_t, uint32_t, const uint32_t*, uint32_t, ArenaRef&) { return false; }
void appendArena(PrimClass, uint32_t, uint32_t, const ArenaRef&, uint32_t count, uint32_t, const uint8_t*)
{
	sms3ds_gx_primitives = sms3ds_gx_primitives + 1;
	sms3ds_gx_vertices = sms3ds_gx_vertices + count;
}
bool batchUsesArena() { return false; }

// --- copies, peeks, metrics -----------------------------------------------------------------------
void executeCopy(uint32_t ctrl)
{
	if ((ctrl >> 14) & 1) {  // EFB -> XFB: a frame is shown
		g_displayFrames++;
		sms3ds_gx_frames = sms3ds_gx_frames + 1;
	} else {
		sms3ds_gx_copies = sms3ds_gx_copies + 1;
	}
}

uint32_t peekColor(int, int) { return 0; }
uint32_t peekZ(int, int) { return 0xFFFFFF; }  // far plane: nothing occludes (lens flares, sun)
void pixMetricClear() {}
uint32_t pixMetricRead() { return 0; }

// --- textures -------------------------------------------------------------------------------------
void textureInvalidateAll() {}

// --- tracing (gx_trace.cpp on the PC) -----------------------------------------------------------------
FILE* traceFile() { return NULL; }
void traceNoteBegin() {}
void traceNoteProjection(const float*) {}

}  // namespace gx

// --- host API the game's patched code calls ---------------------------------------------------------
extern "C" {
int GXPC_InitAuto(float) { return 1; }
void GXPC_SetStretch2D(int) {}
void GXPC_SetWide2D(int) {}
void GXPC_SetHud(int) {}
void GXPC_HudPaneBegin(float, float) {}
void GXPC_HudPaneEnd(void) {}
}
