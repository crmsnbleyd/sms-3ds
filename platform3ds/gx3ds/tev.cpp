// GX TEV -> PICA texture combiners (Phase 2c).
//
// A GX stage computes  out = (d +/- lerp(a, b, c) + bias) * scale  per channel (colour and alpha
// separately), with inputs from the previous stage, three colour registers, the texture, the
// rasterised colour, konst colours and constants. A PICA stage applies one of a few functions to
// three sources, with one constant colour per stage. Each GX stage becomes one PICA stage by
// recognising the shape of its lerp (zero/one operands); registers never written in the draw and
// konst colours become the stage constant. What does not fit (more than 6 stages or 3 textures,
// two different constants in a stage, compare mode, indirect texturing, bias +0.5, scale 1/2, a
// general d + lerp) gets the nearest drawable form and is counted in sms3ds_gx_tev_approx.
// Register and konst encodings follow sms-pc-port's gx_shader.cpp / gx_render.cpp (MIT, NOTICE).
#include "gx3ds.h"

#include <string.h>

#include "gx_internal.h"
#include "sms3ds.h"

volatile uint32_t sms3ds_gx_tev_approx;

namespace gx3ds {
namespace {

using namespace gx;

enum Kind : uint8_t { K_SRC, K_CONST, K_ZERO, K_ONE };
struct In {
	Kind kind;
	GPU_TEVSRC src;    // K_SRC
	int sel;           // component: 0 r, 1 g, 2 b, 3 a (alpha broadcast for colour), -1 rgb
	uint8_t rgba[4];   // K_CONST value (rgb used by colour, [3] by alpha)
};

int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
int sext11(uint32_t v) { return (v & 0x400) ? int(v) - 0x800 : int(v); }

void tevReg(int i, uint8_t out[4])  // BP 0xE0 + 2i: 0 = PREV, 1..3 = C0..C2 (11-bit signed)
{
	const uint32_t ra = g.bp[BP_TEV_REG + 2 * i], bg = g.bp[BP_TEV_REG + 2 * i + 1];
	out[0] = (uint8_t)clamp255(sext11(ra & 0x7FF));
	out[1] = (uint8_t)clamp255(sext11((bg >> 12) & 0x7FF));
	out[2] = (uint8_t)clamp255(sext11(bg & 0x7FF));
	out[3] = (uint8_t)clamp255(sext11((ra >> 12) & 0x7FF));
}
void konstReg(int i, uint8_t out[4])
{
	const uint32_t ra = g.kreg[2 * i], bg = g.kreg[2 * i + 1];
	out[0] = ra & 0xFF;
	out[1] = (bg >> 12) & 0xFF;
	out[2] = bg & 0xFF;
	out[3] = (ra >> 12) & 0xFF;
}
In konst(uint32_t sel, bool alpha)
{
	static const uint8_t k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
	In r{K_CONST, GPU_CONSTANT, -1, {0, 0, 0, 0}};
	uint8_t k[4];
	if (sel < 8) {
		memset(r.rgba, k8[sel], 4);
	} else if (!alpha && sel >= 12 && sel < 16) {
		konstReg(sel - 12, k);
		memcpy(r.rgba, k, 4);
	} else if (sel >= 16) {
		konstReg(sel & 3, k);
		memset(r.rgba, k[(sel - 16) >> 2], 4);
	} else {
		memset(r.rgba, 255, 4);
	}
	return r;
}

// Where each GX register's value is during the draw: a constant until a stage writes it, then the
// PICA previous-stage output (only PREV is tracked through writes).
struct Regs {
	bool written[4];
	uint8_t value[4][4];
};

struct StageCtx {
	Regs* regs;
	int unit;               // texture unit of this stage's map, or -1
	uint32_t swapRas, swapTex;  // swap tables (2 bits per component: r g b a)
	uint32_t kcsel, kasel;
	bool approx;
};

In regIn(StageCtx& c, int reg, bool alphaOnly)
{
	if (c.regs->written[reg])
		return In{K_SRC, GPU_PREVIOUS, alphaOnly ? 3 : -1, {0, 0, 0, 0}};
	In r{K_CONST, GPU_CONSTANT, -1, {0, 0, 0, 0}};
	memcpy(r.rgba, c.regs->value[reg], 4);
	if (alphaOnly)
		memset(r.rgba, c.regs->value[reg][3], 3);
	return r;
}

// A swap table applied to a colour (sel = -1 when identity) or alpha read.
int swapSel(uint32_t swap, bool alpha, bool& ok)
{
	const int r = swap & 3, g2 = (swap >> 2) & 3, b = (swap >> 4) & 3, a = (swap >> 6) & 3;
	if (alpha)
		return a;
	if (r == 0 && g2 == 1 && b == 2)
		return -1;
	if (r == g2 && g2 == b)
		return r;  // broadcast of one channel
	ok = false;
	return -1;
}

In colorIn(StageCtx& c, uint32_t id)
{
	bool ok = true;
	In r{K_ZERO, GPU_CONSTANT, -1, {0, 0, 0, 0}};
	switch (id) {
	case 0: return regIn(c, 0, false);
	case 1: return regIn(c, 0, true);
	case 2: return regIn(c, 1, false);
	case 3: return regIn(c, 1, true);
	case 4: return regIn(c, 2, false);
	case 5: return regIn(c, 2, true);
	case 6: return regIn(c, 3, false);
	case 7: return regIn(c, 3, true);
	case 8:
	case 9:
		if (c.unit < 0) {  // no texture on this stage: GX reads white
			r.kind = K_ONE;
			return r;
		}
		r = In{K_SRC, (GPU_TEVSRC)(GPU_TEXTURE0 + c.unit), -1, {0, 0, 0, 0}};
		r.sel = id == 9 ? swapSel(c.swapTex, true, ok) : swapSel(c.swapTex, false, ok);
		if (!ok)
			c.approx = true;
		return r;
	case 10:
	case 11:
		r = In{K_SRC, GPU_PRIMARY_COLOR, -1, {0, 0, 0, 0}};
		r.sel = id == 11 ? swapSel(c.swapRas, true, ok) : swapSel(c.swapRas, false, ok);
		if (!ok)
			c.approx = true;
		return r;
	case 12: r.kind = K_ONE; return r;
	case 13: r.kind = K_CONST; memset(r.rgba, 128, 4); return r;
	case 14: return konst(c.kcsel, false);
	default: return r;  // 15: zero
	}
}

In alphaIn(StageCtx& c, uint32_t id)
{
	bool ok = true;
	In r{K_ZERO, GPU_CONSTANT, 3, {0, 0, 0, 0}};
	switch (id) {
	case 0: return regIn(c, 0, true);
	case 1: return regIn(c, 1, true);
	case 2: return regIn(c, 2, true);
	case 3: return regIn(c, 3, true);
	case 4:
		if (c.unit < 0) {
			r.kind = K_ONE;
			return r;
		}
		r = In{K_SRC, (GPU_TEVSRC)(GPU_TEXTURE0 + c.unit), swapSel(c.swapTex, true, ok), {0, 0, 0, 0}};
		return r;
	case 5: r = In{K_SRC, GPU_PRIMARY_COLOR, swapSel(c.swapRas, true, ok), {0, 0, 0, 0}}; return r;
	case 6: return konst(c.kasel, true);
	default: return r;  // 7: zero
	}
}

bool isZero(const In& i, bool alpha)
{
	if (i.kind == K_ZERO)
		return true;
	if (i.kind == K_CONST)
		return alpha ? i.rgba[3] == 0 : (i.rgba[0] | i.rgba[1] | i.rgba[2]) == 0;
	return false;
}
bool isOne(const In& i, bool alpha)
{
	if (i.kind == K_ONE)
		return true;
	if (i.kind == K_CONST)
		return alpha ? i.rgba[3] == 255 : (i.rgba[0] & i.rgba[1] & i.rgba[2]) == 255;
	return false;
}

// One PICA channel set-up being built.
struct Chan {
	GPU_TEVSRC src[3];
	int op[3];
	GPU_COMBINEFUNC func;
	GPU_TEVSCALE scale;
};

struct ConstSlot {
	bool rgbUsed, aUsed;
	uint8_t rgba[4];
};

// Turns an input into a PICA source + operand; constants claim the stage constant (rgb or a).
void place(StageCtx& c, ConstSlot& k, const In& in, bool alpha, bool oneMinus, GPU_TEVSRC& src, int& op)
{
	In i = in;
	if (i.kind == K_ZERO || i.kind == K_ONE) {  // as constants 0 / 255
		uint8_t v = i.kind == K_ONE ? 255 : 0;
		i.kind = K_CONST;
		memset(i.rgba, v, 4);
	}
	if (i.kind == K_CONST) {
		if (alpha) {
			if (k.aUsed && k.rgba[3] != i.rgba[3])
				c.approx = true;
			k.aUsed = true;
			k.rgba[3] = i.rgba[3];
		} else {
			if (k.rgbUsed && memcmp(k.rgba, i.rgba, 3) != 0)
				c.approx = true;
			k.rgbUsed = true;
			memcpy(k.rgba, i.rgba, 3);
		}
		src = GPU_CONSTANT;
		op = alpha ? (oneMinus ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA)
		           : (oneMinus ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR);
		return;
	}
	src = i.src;
	if (alpha) {
		static const int kA[4] = {GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_SRC_ALPHA};
		static const int kA1[4] = {GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_ONE_MINUS_SRC_G,
		                           GPU_TEVOP_A_ONE_MINUS_SRC_B, GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA};
		int s = i.sel < 0 ? 3 : i.sel;
		op = oneMinus ? kA1[s] : kA[s];
	} else {
		static const int kC[5] = {GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B,
		                          GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR};
		static const int kC1[5] = {GPU_TEVOP_RGB_ONE_MINUS_SRC_R, GPU_TEVOP_RGB_ONE_MINUS_SRC_G,
		                           GPU_TEVOP_RGB_ONE_MINUS_SRC_B, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,
		                           GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR};
		int s = i.sel < 0 ? 4 : i.sel;
		op = oneMinus ? kC1[s] : kC[s];
	}
}

// Compiles one channel of a GX stage (env = colour or alpha env word).
void compileChan(StageCtx& c, ConstSlot& k, uint32_t env, bool alpha, Chan& out)
{
	In a, b, cc, d;
	if (alpha) {
		d = alphaIn(c, (env >> 4) & 7);
		cc = alphaIn(c, (env >> 7) & 7);
		b = alphaIn(c, (env >> 10) & 7);
		a = alphaIn(c, (env >> 13) & 7);
	} else {
		d = colorIn(c, env & 15);
		cc = colorIn(c, (env >> 4) & 15);
		b = colorIn(c, (env >> 8) & 15);
		a = colorIn(c, (env >> 12) & 15);
	}
	const uint32_t bias = (env >> 16) & 3, sub = (env >> 18) & 1, scale = (env >> 20) & 3;
	for (int i = 0; i < 3; i++) {
		out.src[i] = GPU_PREVIOUS;
		out.op[i] = alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
	}
	out.scale = scale == 1 ? GPU_TEVSCALE_2 : scale == 2 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_1;
	if (scale == 3)
		c.approx = true;  // x0.5
	if (bias == 3) {     // compare mode: pass d through
		c.approx = true;
		place(c, k, d, alpha, false, out.src[0], out.op[0]);
		out.func = GPU_REPLACE;
		return;
	}
	if (bias == 1)
		c.approx = true;  // +0.5

	// the lerp's value v and how to combine it with d
	const bool dz = isZero(d, alpha);
	In v1, v2;          // v = v1 (single) or v1 * v2 (product) or lerp
	enum { V_ZERO, V_ONE_SRC, V_PRODUCT, V_LERP } vk;
	bool v2Minus = false;
	if (isZero(cc, alpha)) {
		vk = isZero(a, alpha) ? V_ZERO : V_ONE_SRC;
		v1 = a;
	} else if (isOne(cc, alpha)) {
		vk = isZero(b, alpha) ? V_ZERO : V_ONE_SRC;
		v1 = b;
	} else if (isZero(a, alpha)) {
		vk = isZero(b, alpha) ? V_ZERO : V_PRODUCT;
		v1 = b;
		v2 = cc;
	} else if (isZero(b, alpha)) {
		vk = V_PRODUCT;
		v1 = a;
		v2 = cc;
		v2Minus = true;
	} else {
		vk = V_LERP;
	}

	if (vk == V_ZERO) {
		place(c, k, d, alpha, false, out.src[0], out.op[0]);
		out.func = GPU_REPLACE;
	} else if (vk == V_ONE_SRC) {
		if (dz) {
			place(c, k, v1, alpha, false, out.src[0], out.op[0]);
			out.func = GPU_REPLACE;
		} else {
			place(c, k, d, alpha, false, out.src[0], out.op[0]);
			place(c, k, v1, alpha, false, out.src[1], out.op[1]);
			out.func = sub ? GPU_SUBTRACT : (bias == 2 ? GPU_ADD_SIGNED : GPU_ADD);
			if (bias == 2 && sub)
				c.approx = true;
			return;
		}
	} else if (vk == V_PRODUCT) {
		place(c, k, v1, alpha, false, out.src[0], out.op[0]);
		place(c, k, v2, alpha, v2Minus, out.src[1], out.op[1]);
		if (dz) {
			out.func = GPU_MODULATE;
		} else {
			place(c, k, d, alpha, false, out.src[2], out.op[2]);
			out.func = GPU_MULTIPLY_ADD;
			if (sub)
				c.approx = true;
		}
	} else {  // general lerp: INTERPOLATE(b, a, c) = b*c + a*(1-c)
		place(c, k, b, alpha, false, out.src[0], out.op[0]);
		place(c, k, a, alpha, false, out.src[1], out.op[1]);
		place(c, k, cc, alpha, false, out.src[2], out.op[2]);
		out.func = GPU_INTERPOLATE;
		if (!dz)
			c.approx = true;  // d dropped
	}
	if (bias == 2)
		c.approx = true;
}

}  // namespace

void setupTev(TevPlan& plan)
{
	const uint32_t gen = g.bp[BP_GENMODE];
	const uint32_t nst = ((gen >> 10) & 15) + 1;
	const uint32_t nind = (gen >> 16) & 7;
	bool approx = nst > 6 || nind > 0;

	// texture units: distinct (map, coord) pairs in order of use
	plan.units = 0;
	int stageUnit[16];
	for (uint32_t s = 0; s < nst; s++) {
		const uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
		stageUnit[s] = -1;
		if (!((ord >> 6) & 1))
			continue;
		const int map = ord & 7, coord = (ord >> 3) & 7;
		int u = -1;
		for (int i = 0; i < plan.units; i++)
			if (plan.map[i] == map && plan.coord[i] == coord)
				u = i;
		if (u < 0 && plan.units < 3) {
			u = plan.units++;
			plan.map[u] = map;
			plan.coord[u] = coord;
		}
		if (u < 0)
			approx = true;
		stageUnit[s] = u;
	}

	Regs regs;
	memset(regs.written, 0, sizeof regs.written);
	for (int i = 0; i < 4; i++)
		tevReg(i, regs.value[i]);

	const int npica = nst > 6 ? 6 : (int)nst;
	for (int s = 0; s < npica; s++) {
		const uint32_t cenv = g.bp[BP_TEV_COLOR_ENV + 2 * s] & 0xFFFFFF;
		const uint32_t aenv = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1] & 0xFFFFFF;
		const uint32_t ks = g.bp[BP_TEV_KSEL + (s >> 1)];
		const uint32_t ksel = (s & 1) ? ((ks >> 14) & 0x3FF) : ((ks >> 4) & 0x3FF);
		const uint32_t rswap = aenv & 3, tswap = (aenv >> 2) & 3;
		auto swapTable = [](uint32_t t) {
			return (g.bp[BP_TEV_KSEL + 2 * t] & 15) | (g.bp[BP_TEV_KSEL + 2 * t + 1] & 15) << 4;
		};
		const uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
		if (((ord >> 7) & 7) == 1)
			approx = true;  // rasterised colour from channel 1: shown as channel 0
		StageCtx c{&regs, stageUnit[s], swapTable(rswap), swapTable(tswap), ksel & 31, ksel >> 5, false};
		ConstSlot k{false, false, {0, 0, 0, 0}};
		Chan col, alp;
		compileChan(c, k, cenv, false, col);
		compileChan(c, k, aenv, true, alp);
		C3D_TexEnv* env = C3D_GetTexEnv(s);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_RGB, col.src[0], col.src[1], col.src[2]);
		C3D_TexEnvSrc(env, C3D_Alpha, alp.src[0], alp.src[1], alp.src[2]);
		C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)col.op[0], (GPU_TEVOP_RGB)col.op[1], (GPU_TEVOP_RGB)col.op[2]);
		C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)alp.op[0], (GPU_TEVOP_A)alp.op[1], (GPU_TEVOP_A)alp.op[2]);
		C3D_TexEnvFunc(env, C3D_RGB, col.func);
		C3D_TexEnvFunc(env, C3D_Alpha, alp.func);
		C3D_TexEnvScale(env, C3D_RGB, col.scale);
		C3D_TexEnvScale(env, C3D_Alpha, alp.scale);
		C3D_TexEnvColor(env, (u32)k.rgba[0] | (u32)k.rgba[1] << 8 | (u32)k.rgba[2] << 16 | (u32)k.rgba[3] << 24);
		// destination: PREV becomes the PICA output; another register written is not tracked
		const uint32_t cdst = (cenv >> 22) & 3, adst = (aenv >> 22) & 3;
		if (cdst != 0 || adst != 0)
			approx = true;
		regs.written[0] = true;
		approx |= c.approx;
	}
	for (int s = npica; s < 6; s++) {  // pass the result through the unused stages
		C3D_TexEnv* env = C3D_GetTexEnv(s);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}

	// alpha compare (BP 0xF3): one PICA test; GX's two-test AND/OR keeps the meaningful one
	const uint32_t ac = g.bp[BP_ALPHACOMPARE];
	const uint32_t ref0 = ac & 255, ref1 = (ac >> 8) & 255, c0 = (ac >> 16) & 7, c1 = (ac >> 19) & 7, logic = (ac >> 22) & 3;
	static const GPU_TESTFUNC kCmp[8] = {GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
	                                     GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
	const bool always0 = c0 == 7, always1 = c1 == 7, never0 = c0 == 0, never1 = c1 == 0;
	if (logic == 0 && always0 && always1)  // AND, both pass
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
	else if (logic == 0 && always1)
		C3D_AlphaTest(true, kCmp[c0], (int)ref0);
	else if (logic == 0 && always0)
		C3D_AlphaTest(true, kCmp[c1], (int)ref1);
	else if (logic == 1 && never1)  // OR with a never
		C3D_AlphaTest(true, kCmp[c0], (int)ref0);
	else if (logic == 1 && never0)
		C3D_AlphaTest(true, kCmp[c1], (int)ref1);
	else if (logic == 1 && (always0 || always1))
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
	else {
		C3D_AlphaTest(true, kCmp[c0], (int)ref0);
		approx = true;
	}

	if ((g.bp[BP_FOG3] >> 21) & 7)
		approx = true;  // fog: later
	if (approx)
		sms3ds_gx_tev_approx = sms3ds_gx_tev_approx + 1;
}

}  // namespace gx3ds
