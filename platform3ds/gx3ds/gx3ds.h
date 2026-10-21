// sms-3ds GX backend on citro3d (Phase 2). Internal interfaces between its files.
// Built with libctru's short enums (it includes <citro3d.h>); it shares only plain integers and
// floats with the GX front end (gx_internal.h).
#ifndef SMS_GX3DS_H
#define SMS_GX3DS_H

#include <citro3d.h>
#include <stdint.h>

namespace gx3ds {

// One vertex as the PICA reads it (passthru.v.pica: v0 position, v1 colour, v2-v4 texcoords).
struct OutVertex {
	float pos[4];     // clip space, already tilted for the top screen
	uint8_t clr[4];   // RGBA, from GX colour channel 0
	float tc[3][2];   // texture units 0..2
};
static_assert(sizeof(OutVertex) == 44, "PICA vertex layout");

// tev.cpp: what the current TEV needs, and the combiners set up for it (setupTev).
struct TevPlan {
	int units;       // texture units used (0..3)
	int map[3];      // GX texture map per unit
	int coord[3];    // GX texture coordinate per unit
};
void setupTev(TevPlan& plan);

// vtx.cpp: the GPU vertex pipeline (gx.v.pica). vtxSetup points the PICA at a batch of packed GX
// vertices of format `fmt` (gx_fifo.cpp's vtxFmtLayout) at `base` in linear memory and sets the
// uniforms from the current XF state for the texture units in `plan`; false: cannot be drawn.
void vtxInit(shaderProgram_s* prog);
void vtxInvalidate();  // another program changed the vertex uniforms
bool vtxSetup(const TevPlan& plan, uint32_t fmt, uint32_t stride, const void* base);

// texture.cpp: GX texture map `map` (0..7) as a PICA texture with its wrap/filter state, or white.
C3D_Tex* textureForMap(int map);

// The GameCube picture (640 x 480 of the 640 x 528 EFB) is shown 4:3, letterboxed in the 400 x 240
// top screen: EFB pixels are halved and offset by 40 screen columns.
enum { kScreenW = 400, kScreenH = 240, kLetterbox = 40 };

}  // namespace gx3ds

#endif
