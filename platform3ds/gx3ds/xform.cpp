// CPU vertex transform and lighting for the PICA backend (Phase 2a), following sms-pc-port's GLSL
// vertex shader (platform/gx/src/gx_shader.cpp genVS, emitLight; MIT, see NOTICE) term for term:
// position through the 3x4 position matrix in XF memory, perspective or orthographic projection
// (XF 0x1020), the viewport (XF 0x101A) in EFB pixels, then lighting channel 0 (XF 0x100E/0x1010).
// Phase 4 moves this into a PICA vertex shader.
#include "gx3ds.h"

#include "gx_internal.h"

#include <math.h>
#include <string.h>

namespace gx3ds {
namespace {

inline float xmem(uint32_t word)  // XF memory holds host floats as bits
{
	float f;
	memcpy(&f, &gx::g.xfMem[word], 4);
	return f;
}
inline float xreg(uint32_t reg)
{
	float f;
	memcpy(&f, &gx::g.xfReg[reg], 4);
	return f;
}
inline void unpackRGBA(uint32_t v, float out[4])
{
	out[0] = float((v >> 24) & 255) * (1.0f / 255.0f);
	out[1] = float((v >> 16) & 255) * (1.0f / 255.0f);
	out[2] = float((v >> 8) & 255) * (1.0f / 255.0f);
	out[3] = float(v & 255) * (1.0f / 255.0f);
}
inline float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Light block in XF memory: 0x600 + 16 * light; colour at +3, cos attenuation at +4, distance
// attenuation at +7, position at +10, direction at +13.
struct Light {
	float color[4], a[3], k[3], pos[3], dir[3];
};
void loadLight(int l, Light& L)
{
	uint32_t b = 0x600 + 16 * l;
	unpackRGBA(gx::g.xfMem[b + 3], L.color);
	for (int i = 0; i < 3; i++) {
		L.a[i] = xmem(b + 4 + i);
		L.k[i] = xmem(b + 7 + i);
		L.pos[i] = xmem(b + 10 + i);
		L.dir[i] = xmem(b + 13 + i);
	}
}

// One channel part (colour or alpha) as genVS does; returns the lit value for components [c0,c1).
void litChannel(uint32_t ctrl, const float vtxClr[4], const float amb[4], const float mat[4], const float vpos[3],
                const float N[3], const Light* lights, float out[4], int c0, int c1)
{
	const float* m = (ctrl & 1) ? vtxClr : mat;
	if (!(ctrl & 2)) {
		for (int i = c0; i < c1; i++)
			out[i] = m[i];
		return;
	}
	float sum[4];
	const float* a = (ctrl & 64) ? vtxClr : amb;
	for (int i = 0; i < 4; i++)
		sum[i] = a[i];
	uint32_t mask = ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
	uint32_t diff = (ctrl >> 7) & 3;
	bool attn = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
	for (int l = 0; l < 8; l++) {
		if (!(mask & (1u << l)))
			continue;
		const Light& L = lights[l];
		float Lv[3] = {L.pos[0] - vpos[0], L.pos[1] - vpos[1], L.pos[2] - vpos[2]};
		float at = 1.0f, nl;
		if (!attn) {
			float d = sqrtf(Lv[0] * Lv[0] + Lv[1] * Lv[1] + Lv[2] * Lv[2]);
			if (d > 0.0f)
				for (int i = 0; i < 3; i++)
					Lv[i] /= d;
			nl = N[0] * Lv[0] + N[1] * Lv[1] + N[2] * Lv[2];
		} else if (spot) {
			float d2 = Lv[0] * Lv[0] + Lv[1] * Lv[1] + Lv[2] * Lv[2], d = sqrtf(d2);
			if (d > 0.0f)
				for (int i = 0; i < 3; i++)
					Lv[i] /= d;
			nl = N[0] * Lv[0] + N[1] * Lv[1] + N[2] * Lv[2];
			float cs = Lv[0] * L.dir[0] + Lv[1] * L.dir[1] + Lv[2] * L.dir[2];
			if (cs < 0.0f)
				cs = 0.0f;
			float num = L.a[0] + L.a[1] * cs + L.a[2] * cs * cs, den = L.k[0] + L.k[1] * d + L.k[2] * d2;
			at = (num > 0.0f ? num : 0.0f) / (den > 1e-30f ? den : 1e-30f);
		} else {  // specular: the light's position holds a direction
			float d = sqrtf(L.pos[0] * L.pos[0] + L.pos[1] * L.pos[1] + L.pos[2] * L.pos[2]);
			for (int i = 0; i < 3; i++)
				Lv[i] = d > 0.0f ? L.pos[i] / d : 0.0f;
			nl = N[0] * Lv[0] + N[1] * Lv[1] + N[2] * Lv[2];
			float t = 0.0f;
			if (nl > 0.0f) {
				t = N[0] * L.dir[0] + N[1] * L.dir[1] + N[2] * L.dir[2];
				if (t < 0.0f)
					t = 0.0f;
			}
			float num = L.a[0] + L.a[1] * t + L.a[2] * t * t, den = L.k[0] + L.k[1] * t + L.k[2] * t * t;
			at = (num > 0.0f ? num : 0.0f) / (den > 1e-30f ? den : 1e-30f);
		}
		float df = diff == 0 ? 1.0f : diff == 1 ? nl : (nl > 0.0f ? nl : 0.0f);
		for (int i = 0; i < 4; i++)
			sum[i] += at * df * L.color[i];
	}
	for (int i = c0; i < c1; i++)
		out[i] = m[i] * clamp01(sum[i]);
}

}  // namespace

void transform(const uint8_t* verts, uint32_t count, uint32_t fmt, uint32_t stride, OutVertex* out)
{
	using namespace gx;
	const VtxFmtLayout& lay = vtxFmtLayout(fmt);

	// projection (XF 0x1020..0x1026) and viewport (XF 0x101A..0x101F)
	float p[6];
	for (int i = 0; i < 6; i++)
		p[i] = xreg(XFR_PROJ + i);
	const bool ortho = g.xfReg[XFR_PROJ + 6] & 1;
	float vp[6];
	for (int i = 0; i < 6; i++)
		vp[i] = xreg(XFR_VIEWPORT + i);
	const float vpx = vp[3] - 342.0f, vpy = vp[4] - 342.0f;

	// lighting channel 0 (colour and alpha); with no channel the TEV has no rasterised colour:
	// Phase 2a shows white so geometry stays visible.
	const uint32_t numChans = g.xfReg[XFR_NUMCHANS] & 3;
	const uint32_t cctrl = g.xfReg[XFR_COLOR0CTRL] & 0x7FFF, actrl = g.xfReg[XFR_ALPHA0CTRL] & 0x7FFF;
	float amb[4], mat[4];
	unpackRGBA(g.xfReg[XFR_AMB0], amb);
	unpackRGBA(g.xfReg[XFR_MAT0], mat);
	const bool lit = numChans && ((cctrl | actrl) & 2);
	Light lights[8];
	if (lit) {
		uint32_t mask = (((cctrl >> 2) & 15) | ((cctrl >> 11) & 15) << 4) | (((actrl >> 2) & 15) | ((actrl >> 11) & 15) << 4);
		for (int l = 0; l < 8; l++)
			if (mask & (1u << l))
				loadLight(l, lights[l]);
	}

	// default matrix indices when the vertices carry none (XF 0x1018/0x1019)
	const uint32_t defPos = g.xfReg[XFR_MATIDX_A] & 63;

	for (uint32_t v = 0; v < count; v++) {
		const uint8_t* src = verts + v * stride;
		float pos[3];
		memcpy(pos, src, 12);
		uint32_t pm = (fmt & VF_MTX) ? src[lay.mtx] : defPos;
		const uint32_t mw = pm * 4;
		float vpos[3];
		for (int r = 0; r < 3; r++)
			vpos[r] = xmem(mw + 4 * r) * pos[0] + xmem(mw + 4 * r + 1) * pos[1] + xmem(mw + 4 * r + 2) * pos[2] +
			          xmem(mw + 4 * r + 3);

		float cx, cy, cz, cw;
		if (!ortho) {
			cx = p[0] * vpos[0] + p[1] * vpos[2];
			cy = p[2] * vpos[1] + p[3] * vpos[2];
			cz = p[4] * vpos[2] + p[5];
			cw = -vpos[2];
		} else {
			cx = p[0] * vpos[0] + p[1];
			cy = p[2] * vpos[1] + p[3];
			cz = p[4] * vpos[2] + p[5];
			cw = 1.0f;
		}
		// EFB pixel coordinates times w, and GX depth (0 near .. 1 far) times w
		const float X = vpx * cw + vp[0] * cx;
		const float Y = vpy * cw + vp[1] * cy;
		const float Z = (vp[5] * cw + vp[2] * cz) * (1.0f / 16777215.0f);
		// letterboxed screen: sx = 40 + X/2 (of 400), sy = Y/2 (of 240, row 0 at the top)
		const float sxn = (kLetterbox * cw + 0.5f * X) * (2.0f / kScreenW) - cw;  // -w (left) .. w (right)
		const float syn = cw - (0.5f * Y) * (2.0f / kScreenH);                     // w (top) .. -w (bottom)
		// The top screen's framebuffer is rotated: GPU x runs along the screen's height and GPU y
		// along its width (as citro3d's Mtx_OrthoTilt). PICA depth: z from 0 (near) to -w (far).
		OutVertex& o = out[v];
		o.pos[0] = syn;
		o.pos[1] = -sxn;
		o.pos[2] = -Z;
		o.pos[3] = cw;

		float vc[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		if (fmt & VF_CLR0)
			for (int i = 0; i < 4; i++)
				vc[i] = src[lay.clr[0] + i] * (1.0f / 255.0f);
		float col[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		if (numChans) {
			float N[3] = {0.0f, 0.0f, 1.0f};
			if (lit && (fmt & VF_NRM)) {
				float n[3];
				memcpy(n, src + lay.nrm, 12);
				const uint32_t nm = 0x400 + pm * 3;
				for (int r = 0; r < 3; r++)
					N[r] = xmem(nm + 3 * r) * n[0] + xmem(nm + 3 * r + 1) * n[1] + xmem(nm + 3 * r + 2) * n[2];
				float len = sqrtf(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
				if (len > 0.0f)
					for (int i = 0; i < 3; i++)
						N[i] /= len;
				else
					N[0] = N[1] = 0.0f, N[2] = 1.0f;
			}
			litChannel(cctrl, vc, amb, mat, vpos, N, lights, col, 0, 3);
			litChannel(actrl, vc, amb, mat, vpos, N, lights, col, 3, 4);
		}
		for (int i = 0; i < 4; i++)
			o.clr[i] = (uint8_t)(clamp01(col[i]) * 255.0f + 0.5f);

		o.tc[0] = o.tc[1] = 0.0f;
		if (fmt & VF_TEX0)
			memcpy(o.tc, src + lay.tex[0], 8);
	}
}

}  // namespace gx3ds
