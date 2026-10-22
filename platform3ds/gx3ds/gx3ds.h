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

// Why a batch is drawn approximately (TevPlan::why, g_vtxWhy); the HUD and the frame dump show them.
enum : uint32_t {
	AX_STAGES = 1u << 0,      // more than 6 TEV stages
	AX_INDIRECT = 1u << 1,    // indirect texturing
	AX_UNITS = 1u << 2,       // more than 3 texture (map, coordinate) pairs
	AX_SWAP = 1u << 3,        // a swap table that is neither identity nor a broadcast
	AX_CONST2 = 1u << 4,      // two different constants in one stage
	AX_SCALE_HALF = 1u << 5,  // scale 1/2
	AX_COMPARE = 1u << 6,     // compare mode
	AX_BIAS_HALF = 1u << 7,   // bias +1/2
	AX_SIGNED = 1u << 8,      // bias -1/2 outside d + v
	AX_SUB = 1u << 9,         // d - a*b
	AX_LERP_D = 1u << 10,     // d + lerp(a, b, c): d dropped
	AX_CHAN1 = 1u << 11,      // rasterised colour from channel 1 (shown as channel 0)
	AX_DEST = 1u << 12,       // a stage writes C0..C2
	AX_ALPHA_TEST = 1u << 13, // two alpha tests
	AX_FOG = 1u << 14,        // fog
	AX_LIGHT = 1u << 15,      // vtx.cpp: lighting
	AX_TEXGEN = 1u << 16,     // vtx.cpp: texgen
	AX_COPY = 1u << 17,       // render.cpp: an EFB copy's format (depth, intensity)
};

// A TEV compare-mode stage between a texture and a constant, baked into the texture: each texel
// becomes the comparison's result (0 or 255 per channel). `op` is GX's (0 R8 GT, 1 R8 EQ, 2 GR16 GT,
// 3 GR16 EQ, 4 BGR24 GT, 5 BGR24 EQ, 6 RGB8 GT, 7 RGB8 EQ; alpha: A8 for 6/7).
struct TexCmp {
	bool on;
	uint8_t op;
	int8_t sel;      // which texture components feed the compare: -1 rgb, 0..3 one broadcast
	bool texIsB;     // the texture is the right-hand operand
	uint8_t ref[4];  // the constant operand
	bool operator==(const TexCmp& o) const
	{
		return on == o.on && (!on || (op == o.op && sel == o.sel && texIsB == o.texIsB && ref[0] == o.ref[0] &&
		                              ref[1] == o.ref[1] && ref[2] == o.ref[2] && ref[3] == o.ref[3]));
	}
};

// tev.cpp: what the current TEV needs, and the combiners set up for it (setupTev).
struct TevPlan {
	int units;       // texture units used (0..3)
	int map[3];      // GX texture map per unit
	int coord[3];    // GX texture coordinate per unit
	TexCmp cmp[3];   // per unit: a compare baked into the texture
	uint32_t why;    // AX_* approximations made
};
void setupTev(TevPlan& plan, uint32_t copyMaps);  // copyMaps: GX maps whose texture is an EFB copy

// vtx.cpp: the GPU vertex pipeline (gx.v.pica). vtxSetup points the PICA at a batch of packed GX
// vertices of format `fmt` (gx_fifo.cpp's vtxFmtLayout) at `base` in linear memory and sets the
// uniforms from the current XF state for the texture units in `plan`; false: cannot be drawn.
void vtxInit(shaderProgram_s* prog);
void vtxInvalidate();  // another program changed the vertex uniforms
extern uint32_t g_xfMemVersion;  // bumped on every change to XF memory (markXfMemDirty)
bool vtxSetup(const TevPlan& plan, uint32_t fmt, uint32_t stride, const void* base);
extern uint32_t g_vtxWhy;  // AX_* approximations made by the last vtxSetup

// texture.cpp: GX texture map `map` (0..7) as a PICA texture with its wrap/filter state, or white.
C3D_Tex* textureForMap(int map, const TexCmp& cmp);
uint32_t gxTexBytes(uint32_t fmt, uint32_t w, uint32_t h);  // bytes of one GX texture level

// render.cpp: the texture an EFB copy (GXCopyTex) last drew for destination `ptr`, or null.
C3D_Tex* copyTexture(const void* ptr);

// The GameCube picture (640 x 480 of the 640 x 528 EFB) is shown 4:3, letterboxed in the 400 x 240
// top screen: EFB pixels are halved and offset by 40 screen columns.
enum { kScreenW = 400, kScreenH = 240, kLetterbox = 40 };

}  // namespace gx3ds

#endif
