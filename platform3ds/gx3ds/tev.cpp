// GX TEV -> PICA texture combiners (Phase 2c).
//
// A GX stage computes  out = (d +/- lerp(a, b, c) + bias) * scale  per channel (colour and alpha
// separately), with inputs from the previous stage, three colour registers, the texture, the
// rasterised colour, konst colours and constants, and writes PREV or one of C0..C2. A PICA stage
// applies one of a few functions to three sources, with one constant colour per stage, and always
// writes its output (read by the next stage as "previous", and kept in the combiner buffer for
// later stages when its update bit is set).
//
// Each GX stage becomes one or two PICA stages, by the shape of its lerp (zero/one operands): a
// single source, a product, or a general lerp, then combined with d and the bias. Where every GX
// register value lives is tracked: a constant while nothing in the draw wrote it, else the PICA
// stage that produced it, read as "previous" by the next stage or through the combiner buffer by a
// later one. A stage expands to two only while every remaining GX stage still gets one of the six.
// What does not fit (more than 6 PICA stages or 3 textures, two constants in one PICA stage,
// compare mode, indirect texturing, scale 1/2, a register value the buffer cannot carry, colour
// channel 1) gets the nearest drawable form, noted in TevPlan::why (AX_*).
// Register and konst encodings follow sms-pc-port's gx_shader.cpp / gx_render.cpp (MIT, NOTICE).
#include "gx3ds.h"

#include <string.h>

#include "gx_internal.h"
#include "sms3ds.h"

namespace gx3ds {
namespace {

using namespace gx;

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

// --- operands ----------------------------------------------------------------------------------------
enum Kind : uint8_t { K_SRC, K_CONST, K_REG, K_TEMP };
struct In {
	Kind kind;
	GPU_TEVSRC src;   // K_SRC: texture or rasterised colour
	int sel;          // K_SRC: component 0 r, 1 g, 2 b, 3 a (broadcast), -1 rgb
	uint8_t rgba[4];  // K_CONST: the value (a colour operand uses rgb, an alpha operand [3])
	int reg;          // K_REG: GX register 0 PREV, 1..3 C0..C2
	bool alphaOf;     // K_REG: its alpha
};
In cst(uint8_t v) { return In{K_CONST, GPU_CONSTANT, -1, {v, v, v, v}, 0, false}; }
In cstRgba(const uint8_t* v, bool alphaBroadcast)
{
	In r{K_CONST, GPU_CONSTANT, -1, {v[0], v[1], v[2], v[3]}, 0, false};
	if (alphaBroadcast)
		memset(r.rgba, v[3], 3);
	return r;
}
In src(GPU_TEVSRC s, int sel) { return In{K_SRC, s, sel, {0, 0, 0, 0}, 0, false}; }
In temp() { return In{K_TEMP, GPU_PREVIOUS, -1, {0, 0, 0, 0}, 0, false}; }
bool isZero(const In& i, bool alpha)
{
	return i.kind == K_CONST && (alpha ? i.rgba[3] == 0 : (i.rgba[0] | i.rgba[1] | i.rgba[2]) == 0);
}
bool isOne(const In& i, bool alpha)
{
	return i.kind == K_CONST && (alpha ? i.rgba[3] == 255 : (i.rgba[0] & i.rgba[1] & i.rgba[2]) == 255);
}

// Where a GX register's colour or alpha is: known (a constant), a source read again where it is used
// (a texture or the rasterised colour, when the stage that wrote it only copied it), or the output of
// a PICA stage.
struct Loc {
	bool known;
	uint8_t rgba[4];
	int stage;
	bool isSrc;
	In in;  // isSrc: the source as an operand (alpha locations: its alpha component)
};
Loc atStage(int s) { return Loc{false, {0, 0, 0, 0}, s, false, In{}}; }

// GX register `reg` (its alpha with alphaOf) as an operand of a colour (alpha = false) or alpha input.
In regIn(const Loc* col, const Loc* alp, int reg, bool alphaOf, bool alpha)
{
	const bool a = alphaOf || alpha;
	const Loc& L = a ? alp[reg] : col[reg];
	if (L.known)
		return cstRgba(L.rgba, alphaOf && !alpha);
	if (L.isSrc)
		return L.in;
	return In{K_REG, GPU_PREVIOUS, -1, {0, 0, 0, 0}, reg, a};
}

// --- one GX stage's inputs ------------------------------------------------------------------------------
struct StageCtx {
	const Loc* col;  // [4] register colour locations
	const Loc* alp;  // [4] register alpha locations
	int unit;        // texture unit of this stage's map, or -1
	int ras;         // colour channel: 0, 1, 7 none, 5/6 bump
	uint32_t swapRas, swapTex;  // swap tables (2 bits per component: r g b a)
	uint32_t kcsel, kasel;
	uint32_t why;
	TexCmp* cmp;   // the compare baked into this stage's texture unit (null without a texture)
	bool cmpTrue;  // this stage's compare texture was left out for want of units: taken as true
	bool texIsCopy;  // this stage's texture is an EFB copy (a compare cannot be baked into it)
};

In konst(uint32_t sel, bool alpha)
{
	static const uint8_t k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
	uint8_t k[4];
	if (sel < 8)
		return cst(k8[sel]);
	if (!alpha && sel >= 12 && sel < 16) {
		konstReg(sel - 12, k);
		return cstRgba(k, false);
	}
	if (sel >= 16) {
		konstReg(sel & 3, k);
		return cst(k[(sel - 16) >> 2]);
	}
	return cst(255);
}

// A swap table applied to a colour (sel = -1 when identity) or alpha read.
int swapSel(uint32_t swap, bool alpha, uint32_t& why)
{
	const int r = swap & 3, g2 = (swap >> 2) & 3, b = (swap >> 4) & 3, a = (swap >> 6) & 3;
	if (alpha)
		return a;
	if (r == 0 && g2 == 1 && b == 2)
		return -1;
	if (r == g2 && g2 == b)
		return r;  // broadcast of one channel
	why |= AX_SWAP;
	return -1;
}

In rasIn(StageCtx& c, bool alphaRead)
{
	if (c.ras == 0)
		return src(GPU_PRIMARY_COLOR, swapSel(c.swapRas, alphaRead, c.why));
	if (c.ras != 1)
		return cst(0);
	// Colour channel 1 is not computed by the vertex shader. Unlit, it is its material register
	// (or vertex colour 1, read as white); lit (specular in SMS), zero: no highlight.
	const uint32_t ctrl = g.xfReg[alphaRead ? XFR_ALPHA0CTRL + 1 : XFR_COLOR0CTRL + 1];
	if ((g.xfReg[XFR_NUMCHANS] & 3) < 2)
		return cst(0);
	if (ctrl & 2) {
		c.why |= AX_CHAN1;
		return cst(0);
	}
	if (ctrl & 1) {
		c.why |= AX_CHAN1;
		return cst(255);
	}
	const uint32_t m = g.xfReg[XFR_MAT0 + 1];
	const uint8_t v[4] = {uint8_t(m >> 24), uint8_t(m >> 16), uint8_t(m >> 8), uint8_t(m)};
	return cstRgba(v, alphaRead);
}

In texIn(StageCtx& c, bool alphaRead)
{
	if (c.unit < 0)
		return cst(255);  // no texture on this stage: GX reads white
	return src((GPU_TEVSRC)(GPU_TEXTURE0 + c.unit), swapSel(c.swapTex, alphaRead, c.why));
}

In colorIn(StageCtx& c, uint32_t id)
{
	In r;
	switch (id) {
	case 0: return regIn(c.col, c.alp, 0, false, false);
	case 1: return regIn(c.col, c.alp, 0, true, false);
	case 2: return regIn(c.col, c.alp, 1, false, false);
	case 3: return regIn(c.col, c.alp, 1, true, false);
	case 4: return regIn(c.col, c.alp, 2, false, false);
	case 5: return regIn(c.col, c.alp, 2, true, false);
	case 6: return regIn(c.col, c.alp, 3, false, false);
	case 7: return regIn(c.col, c.alp, 3, true, false);
	case 8: return texIn(c, false);
	case 9:
		r = texIn(c, true);
		if (r.kind == K_SRC && r.sel < 0)
			r.sel = 3;
		return r;
	case 10: return rasIn(c, false);
	case 11:
		r = rasIn(c, true);
		if (r.kind == K_SRC && r.sel < 0)
			r.sel = 3;
		return r;
	case 12: return cst(255);
	case 13: return cst(128);
	case 14: return konst(c.kcsel, false);
	default: return cst(0);
	}
}

In alphaIn(StageCtx& c, uint32_t id)
{
	switch (id) {
	case 0: return regIn(c.col, c.alp, 0, true, true);
	case 1: return regIn(c.col, c.alp, 1, true, true);
	case 2: return regIn(c.col, c.alp, 2, true, true);
	case 3: return regIn(c.col, c.alp, 3, true, true);
	case 4: return texIn(c, true);
	case 5: return rasIn(c, true);
	case 6: return konst(c.kasel, true);
	default: return cst(0);
	}
}

// --- a GX stage channel as PICA operations ------------------------------------------------------------------
struct Op {
	GPU_COMBINEFUNC func;
	int n;  // operands used
	In arg[3];
	bool inv[3];  // one minus
	GPU_TEVSCALE scale;
};
struct Prog {
	int nops;
	Op op[6];
	GPU_TEVSCALE scale;  // of the last operation (GX's stage scale)
};

Op mkOp(GPU_COMBINEFUNC f, int n, const In& a, const In& b, const In& c)
{
	return Op{f, n, {a, b, c}, {false, false, false}, GPU_TEVSCALE_1};
}
Op op1(GPU_COMBINEFUNC f, const In& a) { return mkOp(f, 1, a, a, a); }
Op op2(GPU_COMBINEFUNC f, const In& a, const In& b) { return mkOp(f, 2, a, b, b); }

// Appends d +/- x + bias, where x is an operand or (temp) the previous operation's result.
// False when it did not fit in maxOps and something was dropped.
bool combineD(Prog& p, const In& d, const In& x, bool dz, bool sub, uint32_t bias, int maxOps)
{
	const In biasK = cst(bias == 1 ? 255 : 0);  // ADD_SIGNED(t, 1) = t + 0.5, ADD_SIGNED(t, 0) = t - 0.5
	if (dz && sub) {  // 0 - v clamps to 0
		p.op[p.nops++] = op1(GPU_REPLACE, cst(0));
		return true;
	}
	if (dz) {
		if (bias == 0) {
			if (x.kind != K_TEMP)
				p.op[p.nops++] = op1(GPU_REPLACE, x);
			return true;
		}
		if (p.nops + 1 > maxOps)
			return false;
		p.op[p.nops++] = op2(GPU_ADD_SIGNED, x, biasK);
		return true;
	}
	if (p.nops + 1 > maxOps)
		return false;
	if (!sub && bias == 0) {
		p.op[p.nops++] = op2(GPU_ADD, d, x);
		return true;
	}
	if (!sub && bias == 2) {
		p.op[p.nops++] = op2(GPU_ADD_SIGNED, d, x);
		return true;
	}
	p.op[p.nops++] = op2(sub ? GPU_SUBTRACT : GPU_ADD, d, x);
	if (bias == 0)
		return true;
	if (p.nops + 1 > maxOps)  // d + v + 0.5, d - v -/+ 0.5: the bias needs one more
		return false;
	p.op[p.nops++] = op2(GPU_ADD_SIGNED, temp(), biasK);
	return true;
}

// One channel of a GX stage (env = colour or alpha env word) as at most `maxOps` PICA operations.
Prog compileChan(StageCtx& c, uint32_t env, bool alpha, int maxOps)
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
	Prog p{};
	p.scale = scale == 1 ? GPU_TEVSCALE_2 : scale == 2 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_1;
	if (bias == 3) {
		// compare mode, d + (a OP b ? c : 0): a texture against a constant is baked into the texture
		// (TexCmp), which then holds the 0/1 result; anything else passes d through
		const uint8_t op = (uint8_t)((scale << 1) | sub);
		p.scale = GPU_TEVSCALE_1;  // the scale bits are the compare op here
		auto isTex = [](const In& i) { return i.kind == K_SRC && i.src >= GPU_TEXTURE0 && i.src <= GPU_TEXTURE2; };
		const In* t = nullptr;
		const In* k = nullptr;
		if (isTex(a) && b.kind == K_CONST)
			t = &a, k = &b;
		else if (isTex(b) && a.kind == K_CONST)
			t = &b, k = &a;
		if (c.cmpTrue) {  // d + c
			c.why |= AX_COMPARE;
			if (isZero(cc, alpha))
				p.op[p.nops++] = op1(GPU_REPLACE, d);
			else
				combineD(p, d, cc, isZero(d, alpha), false, 0, maxOps);
			return p;
		}
		TexCmp m{true, op, (int8_t)(t ? t->sel : -1), t == &b, {0, 0, 0, 0}};
		if (k)
			memcpy(m.ref, k->rgba, 4);
		const bool fits = t && c.cmp && !c.texIsCopy && (!alpha || op >= 6) && (!c.cmp->on || *c.cmp == m);
		if (!fits) {
			// "greater than" by saturating arithmetic: (a - b) * 4^4 is >= 1 wherever a > b on 8-bit
			// values (the GR16 / BGR24 forms compare their most significant byte only)
			const int extra = (isZero(cc, alpha) ? 0 : isOne(cc, alpha) ? 0 : 1) + (isZero(d, alpha) ? 0 : 1);
			if (!(op & 1) && (!alpha || op >= 6) && maxOps >= 4 + extra && !isZero(cc, alpha)) {
				const int comp = alpha ? 3 : op == 0 ? 0 : op == 2 ? 1 : op == 4 ? 2 : -1;
				auto pick = [&](In i) {
					if (comp < 0)
						return i;
					if (i.kind == K_SRC && i.sel < 0)
						i.sel = comp;
					else if (i.kind == K_CONST)
						memset(i.rgba, i.rgba[comp], 4);
					return i;
				};
				Op o = op2(GPU_SUBTRACT, pick(a), pick(b));
				o.scale = GPU_TEVSCALE_4;
				p.op[p.nops++] = o;
				for (int i = 0; i < 3; i++) {
					Op r = op1(GPU_REPLACE, temp());
					r.scale = GPU_TEVSCALE_4;
					p.op[p.nops++] = r;
				}
				if (!isOne(cc, alpha))
					p.op[p.nops++] = op2(GPU_MODULATE, temp(), cc);
				if (!isZero(d, alpha))
					p.op[p.nops++] = op2(GPU_ADD, d, temp());
				return p;
			}
			c.why |= AX_COMPARE;
			p.op[p.nops++] = op1(GPU_REPLACE, d);
			return p;
		}
		*c.cmp = m;
		const In mask = src(t->src, alpha ? 3 : -1);
		if (isZero(cc, alpha))
			p.op[p.nops++] = op1(GPU_REPLACE, d);
		else if (isOne(cc, alpha))
			combineD(p, d, mask, isZero(d, alpha), false, 0, maxOps);
		else if (isZero(d, alpha))
			p.op[p.nops++] = op2(GPU_MODULATE, cc, mask);
		else
			p.op[p.nops++] = mkOp(GPU_MULTIPLY_ADD, 3, cc, mask, d);
		return p;
	}
	const bool dz = isZero(d, alpha);

	// v = lerp(a, b, c) = a (1 - c) + b c, by shape: zero, one operand, a product, or a lerp
	enum { V_ZERO, V_ONE, V_PRODUCT, V_LERP } vk;
	In x{}, y{};
	bool yInv = false;
	if (isZero(cc, alpha)) {
		vk = isZero(a, alpha) ? V_ZERO : V_ONE;
		x = a;
	} else if (isOne(cc, alpha)) {
		vk = isZero(b, alpha) ? V_ZERO : V_ONE;
		x = b;
	} else if (isZero(a, alpha)) {
		vk = isZero(b, alpha) ? V_ZERO : V_PRODUCT;
		x = b;
		y = cc;
	} else if (isZero(b, alpha)) {
		vk = V_PRODUCT;
		x = a;
		y = cc;
		yInv = true;
	} else {
		vk = V_LERP;
	}

	bool ok = true;
	if (vk == V_ZERO) {
		p.op[p.nops++] = bias == 0 ? op1(GPU_REPLACE, d) : op2(GPU_ADD_SIGNED, d, cst(bias == 1 ? 255 : 0));
	} else if (vk == V_ONE) {
		ok = combineD(p, d, x, dz, sub, bias, maxOps);
	} else if (vk == V_PRODUCT && !dz && !sub && bias == 0) {
		Op o = mkOp(GPU_MULTIPLY_ADD, 3, x, y, d);  // x y + d
		o.inv[1] = yInv;
		p.op[p.nops++] = o;
	} else if (vk == V_LERP && a.kind == K_CONST && b.kind == K_CONST && dz && !sub && bias == 0 && maxOps >= 2) {
		// two constants blended (gradient text): a (1 - c), then b c + that, one constant per stage
		Op o = op2(GPU_MODULATE, a, cc);
		o.inv[1] = true;
		p.op[p.nops++] = o;
		p.op[p.nops++] = mkOp(GPU_MULTIPLY_ADD, 3, b, cc, temp());
	} else if (!dz && maxOps < 2) {
		// one stage for d +/- v: keep d (usually the colour so far) and the lerp's main operand
		const In& main = vk == V_PRODUCT ? x : (cc.kind == K_CONST && cc.rgba[alpha ? 3 : 0] < 128) ? a : b;
		combineD(p, d, main, false, sub, bias, 1);
		c.why |= AX_LERP_D;
	} else {
		Op o;
		if (vk == V_PRODUCT) {
			o = op2(GPU_MODULATE, x, y);
			o.inv[1] = yInv;
		} else if (isOne(a, alpha) && cc.kind == K_CONST) {
			o = mkOp(GPU_MULTIPLY_ADD, 3, b, cc, cc);  // b c + (1 - c): one constant
			o.inv[2] = true;
		} else if (isOne(b, alpha) && cc.kind == K_CONST) {
			o = mkOp(GPU_MULTIPLY_ADD, 3, a, cc, cc);  // a (1 - c) + c
			o.inv[1] = true;
		} else {
			o = mkOp(GPU_INTERPOLATE, 3, b, a, cc);  // b c + a (1 - c)
		}
		p.op[p.nops++] = o;
		if (!dz || bias || sub)
			ok = combineD(p, d, temp(), dz, sub, bias, maxOps);
	}
	if (!ok)
		c.why |= sub ? AX_SUB : !dz ? AX_LERP_D : bias == 1 ? AX_BIAS_HALF : AX_SIGNED;
	if (scale == 3) {  // x 0.5: one more stage multiplying by 128/255
		if (p.nops < maxOps)
			p.op[p.nops++] = op2(GPU_MODULATE, temp(), cst(128));
		else
			c.why |= AX_SCALE_HALF;
	}
	return p;
}

// --- PICA stages being emitted ----------------------------------------------------------------------------
struct PStage {
	GPU_TEVSRC src[2][3];  // [colour, alpha][operand]
	int op[2][3];
	GPU_COMBINEFUNC func[2];
	GPU_TEVSCALE scale[2];
	bool kUsed[2];
	uint8_t k[4];
};

struct Emit {
	PStage st[6];
	int n;
	bool bufUpd[2][4];  // [colour, alpha][stage 0..3]: the buffer takes that stage's output
	int useStage[2][12], useSrc[2][12], nuse[2];  // buffer reads: at which stage, of which stage's output
	uint32_t why;
};

// The combiner buffer at stage n holds the output of the latest stage k < n (k <= 3) with its update
// bit set. True when stage j's output can reach stage n that way (setting j's bit if needed).
bool bufferRequest(Emit& e, int comp, int j, int n)
{
	if (j < 0 || j > 3)
		return false;
	for (int k = j + 1; k < n && k <= 3; k++)
		if (e.bufUpd[comp][k])
			return false;
	if (!e.bufUpd[comp][j])
		for (int u = 0; u < e.nuse[comp]; u++)
			if (e.useStage[comp][u] > j && e.useSrc[comp][u] != j)
				return false;
	e.bufUpd[comp][j] = true;
	if (e.nuse[comp] < 12) {
		e.useStage[comp][e.nuse[comp]] = n;
		e.useSrc[comp][e.nuse[comp]] = j;
		e.nuse[comp]++;
	}
	return true;
}

// Operand `in` of channel g (0 colour, 1 alpha) at PICA stage s.
void resolve(Emit& e, const Loc* col, const Loc* alp, int s, int g, const In& in, bool inv, GPU_TEVSRC& srcOut,
             int& opOut)
{
	static const int kC[5] = {GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_ALPHA,
	                          GPU_TEVOP_RGB_SRC_COLOR};
	static const int kC1[5] = {GPU_TEVOP_RGB_ONE_MINUS_SRC_R, GPU_TEVOP_RGB_ONE_MINUS_SRC_G,
	                           GPU_TEVOP_RGB_ONE_MINUS_SRC_B, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA,
	                           GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR};
	static const int kA[4] = {GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_SRC_ALPHA};
	static const int kA1[4] = {GPU_TEVOP_A_ONE_MINUS_SRC_R, GPU_TEVOP_A_ONE_MINUS_SRC_G, GPU_TEVOP_A_ONE_MINUS_SRC_B,
	                           GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA};
	PStage& st = e.st[s];
	int sel = -1;  // component read: -1 colour, 3 alpha, 0..2 one channel
	switch (in.kind) {
	case K_CONST:
		if (g == 1) {
			if (st.kUsed[1] && st.k[3] != in.rgba[3])
				e.why |= AX_CONST2;
			st.k[3] = in.rgba[3];
		} else {
			if (st.kUsed[0] && memcmp(st.k, in.rgba, 3) != 0)
				e.why |= AX_CONST2;
			memcpy(st.k, in.rgba, 3);
		}
		st.kUsed[g] = true;
		srcOut = GPU_CONSTANT;
		break;
	case K_SRC:
		srcOut = in.src;
		sel = in.sel;
		break;
	case K_TEMP:
		srcOut = GPU_PREVIOUS;
		break;
	case K_REG: {
		const Loc& L = in.alphaOf ? alp[in.reg] : col[in.reg];
		if (L.stage == s - 1)
			srcOut = GPU_PREVIOUS;
		else if (bufferRequest(e, in.alphaOf ? 1 : 0, L.stage, s))
			srcOut = GPU_PREVIOUS_BUFFER;
		else {
			e.why |= AX_DEST;  // out of the buffer's reach: the previous stage instead
			srcOut = GPU_PREVIOUS;
		}
		sel = in.alphaOf ? 3 : -1;
		break;
	}
	}
	if (g == 1)
		opOut = inv ? kA1[sel < 0 ? 3 : sel] : kA[sel < 0 ? 3 : sel];
	else
		opOut = inv ? kC1[sel < 0 ? 4 : sel] : kC[sel < 0 ? 4 : sel];
}

void emitOp(Emit& e, const Loc* col, const Loc* alp, int s, int g, const Op& o, GPU_TEVSCALE scale)
{
	PStage& st = e.st[s];
	st.func[g] = o.func;
	st.scale[g] = scale;
	for (int i = 0; i < 3; i++) {
		if (i < o.n) {
			resolve(e, col, alp, s, g, o.arg[i], o.inv[i], st.src[g][i], st.op[g][i]);
		} else {
			st.src[g][i] = GPU_PREVIOUS;
			st.op[g][i] = g == 1 ? (int)GPU_TEVOP_A_SRC_ALPHA : (int)GPU_TEVOP_RGB_SRC_COLOR;
		}
	}
}

void passThrough(PStage& st, int g)
{
	st.func[g] = GPU_REPLACE;
	st.scale[g] = GPU_TEVSCALE_1;
	for (int i = 0; i < 3; i++) {
		st.src[g][i] = GPU_PREVIOUS;
		st.op[g][i] = g == 1 ? (int)GPU_TEVOP_A_SRC_ALPHA : (int)GPU_TEVOP_RGB_SRC_COLOR;
	}
}

// When a channel's program only copies a source, a constant or a register, the location it leaves
// in its destination register (no PICA stage needed); false otherwise.
bool copyLoc(const Prog& p, bool alpha, const Loc* col, const Loc* alp, Loc& out)
{
	if (p.nops != 1 || p.op[0].func != GPU_REPLACE || p.op[0].inv[0] || p.scale != GPU_TEVSCALE_1)
		return false;
	const In& in = p.op[0].arg[0];
	switch (in.kind) {
	case K_CONST:
		out = Loc{true, {in.rgba[0], in.rgba[1], in.rgba[2], in.rgba[3]}, -1, false, In{}};
		if (alpha)
			memset(out.rgba, in.rgba[3], 3);
		return true;
	case K_SRC:
		out = Loc{false, {0, 0, 0, 0}, -1, true, in};
		if (alpha && out.in.sel < 0)
			out.in.sel = 3;
		return true;
	case K_REG:
		if (!alpha && in.alphaOf)
			return false;  // colour = a register's alpha broadcast: not a location
		out = in.alphaOf ? alp[in.reg] : col[in.reg];
		return true;
	default:
		return false;
	}
}

}  // namespace

void compileTev(TevPlan& plan, uint32_t copyMaps)
{
	const uint32_t gen = g.bp[BP_GENMODE];
	const uint32_t nst = ((gen >> 10) & 15) + 1;
	const uint32_t nind = (gen >> 16) & 7;
	uint32_t approx = (nst > 6 ? AX_STAGES : 0) | (nind > 0 ? AX_INDIRECT : 0);

	// texture units: distinct (map, coord) pairs in order of use; a compare stage's texture gets its
	// own unit (its texels become the comparison's result)
	// With more than three, compare textures go first: SMS's characters compare a dirt mask to pick
	// their body texture or the goop texture, and without it they are drawn clean (compare true).
	struct Pair {
		int map, coord;
		int cmp;  // the compare stage this unit is baked for (each its own: their masks differ), or -1
	} pairs[16];
	int npairs = 0, stagePair[16];
	for (uint32_t s = 0; s < nst; s++) {
		const uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
		stagePair[s] = -1;
		if (!((ord >> 6) & 1))
			continue;
		const int map = ord & 7, coord = (ord >> 3) & 7;
		const int cmpStage = ((((g.bp[BP_TEV_COLOR_ENV + 2 * s] >> 16) & 3) == 3) ||
		                      (((g.bp[BP_TEV_COLOR_ENV + 2 * s + 1] >> 16) & 3) == 3))
		                         ? (int)s
		                         : -1;
		int i = 0;
		while (i < npairs && !(pairs[i].map == map && pairs[i].coord == coord && pairs[i].cmp == cmpStage))
			i++;
		if (i == npairs)
			pairs[npairs++] = Pair{map, coord, cmpStage};
		stagePair[s] = i;
	}
	const bool dropCmp = npairs > 3;
	int pairUnit[16];
	plan.units = 0;
	for (int u = 0; u < 3; u++)
		plan.cmp[u].on = false;
	for (int i = 0; i < npairs; i++) {
		pairUnit[i] = -1;
		if (dropCmp && pairs[i].cmp >= 0)
			continue;
		if (plan.units < 3) {
			pairUnit[i] = plan.units;
			plan.map[plan.units] = pairs[i].map;
			plan.coord[plan.units] = pairs[i].coord;
			plan.units++;
		} else {
			approx |= AX_UNITS;
		}
	}
	int stageUnit[16];
	bool stageCmpTrue[16];
	for (uint32_t s = 0; s < nst; s++) {
		stageUnit[s] = stagePair[s] < 0 ? -1 : pairUnit[stagePair[s]];
		stageCmpTrue[s] = stagePair[s] >= 0 && dropCmp && pairs[stagePair[s]].cmp >= 0;
	}

	Loc col[4], alp[4];
	for (int i = 0; i < 4; i++) {
		uint8_t v[4];
		tevReg(i, v);
		col[i] = Loc{true, {v[0], v[1], v[2], v[3]}, -1, false, In{}};
		alp[i] = col[i];
	}

	Emit e;
	memset(&e, 0, sizeof e);
	const int ngx = nst > 6 ? 6 : (int)nst;
	for (int s = 0; s < ngx; s++) {
		const uint32_t cenv = g.bp[BP_TEV_COLOR_ENV + 2 * s] & 0xFFFFFF;
		const uint32_t aenv = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1] & 0xFFFFFF;
		const uint32_t ks = g.bp[BP_TEV_KSEL + (s >> 1)];
		const uint32_t ksel = (s & 1) ? ((ks >> 14) & 0x3FF) : ((ks >> 4) & 0x3FF);
		const uint32_t rswap = aenv & 3, tswap = (aenv >> 2) & 3;
		auto swapTable = [](uint32_t t) {
			return (g.bp[BP_TEV_KSEL + 2 * t] & 15) | (g.bp[BP_TEV_KSEL + 2 * t + 1] & 15) << 4;
		};
		const uint32_t ord = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
		StageCtx c{col, alp, stageUnit[s], (int)((ord >> 7) & 7), swapTable(rswap), swapTable(tswap), ksel & 31,
		           ksel >> 5, 0, stageUnit[s] >= 0 ? &plan.cmp[stageUnit[s]] : nullptr, stageCmpTrue[s],
		           stageUnit[s] >= 0 && ((copyMaps >> plan.map[stageUnit[s]]) & 1)};
		// two PICA stages for this GX stage only while every later GX stage keeps at least one
		const int room = 6 - e.n - (ngx - s - 1);
		const int maxOps = room >= 1 ? room : 1;
		Prog pc = compileChan(c, cenv, false, maxOps);
		Prog pa = compileChan(c, aenv, true, maxOps);
		approx |= c.why;
		const uint32_t cdst = (cenv >> 22) & 3, adst = (aenv >> 22) & 3;
		// a stage that only copies a source or constant takes no PICA stage: its readers use that
		Loc nc, na;
		if (copyLoc(pc, false, col, alp, nc) && copyLoc(pa, true, col, alp, na)) {
			col[cdst] = nc;
			alp[adst] = na;
			continue;
		}
		const int k = pc.nops > pa.nops ? pc.nops : pa.nops;
		if (e.n + k > 6) {
			approx |= AX_STAGES;
			break;
		}
		for (int i = 0; i < k; i++) {
			const int ps = e.n + i;
			// a channel with fewer operations does them first and passes its result on
			auto opScale = [](const Prog& p, int i) {
				return i == p.nops - 1 && p.scale != GPU_TEVSCALE_1 ? p.scale : p.op[i].scale;
			};
			if (i < pc.nops)
				emitOp(e, col, alp, ps, 0, pc.op[i], opScale(pc, i));
			else
				passThrough(e.st[ps], 0);
			if (i < pa.nops)
				emitOp(e, col, alp, ps, 1, pa.op[i], opScale(pa, i));
			else
				passThrough(e.st[ps], 1);
		}
		e.n += k;
		col[cdst] = atStage(e.n - 1);
		alp[adst] = atStage(e.n - 1);
	}
	// the result is PREV: when the last stage did not write it, one more stage fetches it
	auto isLast = [&](const Loc& L) { return !L.known && !L.isSrc && L.stage >= 0 && L.stage == e.n - 1; };
	if (!isLast(col[0]) || !isLast(alp[0])) {
		if (e.n < 6) {
			emitOp(e, col, alp, e.n, 0, op1(GPU_REPLACE, regIn(col, alp, 0, false, false)), GPU_TEVSCALE_1);
			emitOp(e, col, alp, e.n, 1, op1(GPU_REPLACE, regIn(col, alp, 0, true, true)), GPU_TEVSCALE_1);
			e.n++;
		} else {
			approx |= AX_DEST;
		}
	}
	// constant alpha (BP CMODE1) replaces only what the EFB stores; the alpha test and blending still
	// see the TEV's alpha. 0 is done by the blend unit (applyState). Another value becomes the last
	// stage's alpha when nothing reads that alpha (no alpha test, no blending by source alpha).
	const uint32_t cm1 = g.bp[BP_CMODE1];
	const uint32_t acTest = g.bp[BP_ALPHACOMPARE];
	const bool alphaTested = !((((acTest >> 22) & 3) == 0 && ((acTest >> 16) & 7) == 7 && ((acTest >> 19) & 7) == 7) ||
	                           (((acTest >> 22) & 3) == 1 && (((acTest >> 16) & 7) == 7 || ((acTest >> 19) & 7) == 7)));
	const uint32_t cm0 = g.bp[BP_CMODE0], sf = (cm0 >> 8) & 7, df = (cm0 >> 5) & 7;
	const bool srcAlphaBlend = (cm0 & 1) && (sf == 4 || sf == 5 || df == 4 || df == 5);
	if (((cm1 >> 8) & 1) && (cm1 & 0xFF) != 0 && (alphaTested || srcAlphaBlend)) {
		approx |= AX_ALPHA_TEST;
	} else if (((cm1 >> 8) & 1) && (cm1 & 0xFF) != 0) {
		int last = e.n < 6 ? e.n : 5;
		if (last == e.n) {
			passThrough(e.st[last], 0);
			e.n++;
		}
		PStage& st = e.st[last];
		st.func[1] = GPU_REPLACE;
		st.scale[1] = GPU_TEVSCALE_1;
		st.src[1][0] = st.src[1][1] = st.src[1][2] = GPU_CONSTANT;
		st.op[1][0] = st.op[1][1] = st.op[1][2] = GPU_TEVOP_A_SRC_ALPHA;
		st.k[3] = cm1 & 0xFF;
	}
	approx |= e.why;

	for (int s = 0; s < 6; s++) {
		C3D_TexEnv* env = &plan.env[s];
		C3D_TexEnvInit(env);
		if (s >= e.n) {  // pass the result through the unused stages
			C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			continue;
		}
		const PStage& st = e.st[s];
		C3D_TexEnvSrc(env, C3D_RGB, st.src[0][0], st.src[0][1], st.src[0][2]);
		C3D_TexEnvSrc(env, C3D_Alpha, st.src[1][0], st.src[1][1], st.src[1][2]);
		C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)st.op[0][0], (GPU_TEVOP_RGB)st.op[0][1], (GPU_TEVOP_RGB)st.op[0][2]);
		C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)st.op[1][0], (GPU_TEVOP_A)st.op[1][1], (GPU_TEVOP_A)st.op[1][2]);
		C3D_TexEnvFunc(env, C3D_RGB, st.func[0]);
		C3D_TexEnvFunc(env, C3D_Alpha, st.func[1]);
		C3D_TexEnvScale(env, C3D_RGB, st.scale[0]);
		C3D_TexEnvScale(env, C3D_Alpha, st.scale[1]);
		C3D_TexEnvColor(env, (u32)st.k[0] | (u32)st.k[1] << 8 | (u32)st.k[2] << 16 | (u32)st.k[3] << 24);
	}
	int mc = 0, ma = 0;
	for (int s = 0; s < 4; s++) {
		mc |= e.bufUpd[0][s] ? 1 << s : 0;
		ma |= e.bufUpd[1][s] ? 1 << s : 0;
	}
	plan.bufRgb = (uint8_t)mc;
	plan.bufAlpha = (uint8_t)ma;
	auto alphaTest = [&](bool on, GPU_TESTFUNC f, int ref) {
		plan.alphaTest = on;
		plan.alphaFunc = f;
		plan.alphaRef = ref;
	};

	// alpha compare (BP 0xF3): one PICA test; GX's two-test AND/OR keeps the meaningful one
	const uint32_t ac = g.bp[BP_ALPHACOMPARE];
	const uint32_t ref0 = ac & 255, ref1 = (ac >> 8) & 255, c0 = (ac >> 16) & 7, c1 = (ac >> 19) & 7, logic = (ac >> 22) & 3;
	static const GPU_TESTFUNC kCmp[8] = {GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
	                                     GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
	// tests that cannot fail or pass on 8 bits count as always / never; two equal tests are one
	auto always = [](uint32_t c, uint32_t ref) { return c == 7 || (c == 3 && ref == 255) || (c == 6 && ref == 0); };
	auto never = [](uint32_t c, uint32_t ref) { return c == 0 || (c == 1 && ref == 0) || (c == 4 && ref == 255); };
	const bool same = c0 == c1 && ref0 == ref1 && logic <= 1;
	const bool always0 = always(c0, ref0), always1 = always(c1, ref1) || same;
	const bool never0 = never(c0, ref0), never1 = never(c1, ref1) || same;
	if (logic == 0 && always0 && always1)  // AND, both pass
		alphaTest(false, GPU_ALWAYS, 0);
	else if (logic == 0 && always1)
		alphaTest(true, kCmp[c0], (int)ref0);
	else if (logic == 0 && always0)
		alphaTest(true, kCmp[c1], (int)ref1);
	else if (logic == 1 && never1)  // OR with a never
		alphaTest(true, kCmp[c0], (int)ref0);
	else if (logic == 1 && never0)
		alphaTest(true, kCmp[c1], (int)ref1);
	else if (logic == 1 && (always0 || always1))
		alphaTest(false, GPU_ALWAYS, 0);
	else {
		alphaTest(true, kCmp[c0], (int)ref0);
		approx |= AX_ALPHA_TEST;
	}

	if ((g.bp[BP_FOG3] >> 21) & 7)
		approx |= AX_FOG;  // fog: later
	plan.why = approx;
}

void applyTev(const TevPlan& plan)
{
	for (int s = 0; s < 6; s++)
		*C3D_GetTexEnv(s) = plan.env[s];  // marks the stage dirty
	C3D_TexEnvBufUpdate(C3D_RGB, plan.bufRgb);
	C3D_TexEnvBufUpdate(C3D_Alpha, plan.bufAlpha);
	C3D_AlphaTest(plan.alphaTest, plan.alphaFunc, plan.alphaRef);
}

}  // namespace gx3ds
