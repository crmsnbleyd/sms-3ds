// Per-batch set-up of the GPU vertex pipeline (gx.v.pica): which fields of the GX front end's packed
// vertices the PICA loads, fixed values for the absent ones, and the uniforms (XF matrix memory,
// the folded projection, lighting channel 0, texgen for the three texture units).
// The maths follows sms-pc-port's vertex shader generator (platform/gx/src/gx_shader.cpp genVS;
// MIT, see NOTICE); its approximations are listed in gx.v.pica.
#include "gx3ds.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#include "gx_internal.h"
#include "sms3ds.h"

namespace gx3ds {
namespace {

using namespace gx;

// uniform locations (looked up by name, so the shader's layout can change)
int u_xf, u_fin, u_amb, u_mat, u_lmode, u_chan, u_lcol, u_lpos, u_ldir, u_latA, u_latK, u_tgp, u_tgsrc, u_tgsel;
float s_shadow[96][4];
bool s_shadowValid[96];

void setU(int loc, float x, float y, float z, float w)
{
	float* s = s_shadow[loc];
	if (s_shadowValid[loc] && s[0] == x && s[1] == y && s[2] == z && s[3] == w)
		return;
	s[0] = x, s[1] = y, s[2] = z, s[3] = w;
	s_shadowValid[loc] = true;
	g_gxCount[SMS3DS_CNT_UNIF]++;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, x, y, z, w);
}
void setU4(int loc, const float v[4]) { setU(loc, v[0], v[1], v[2], v[3]); }

inline float xmem(uint32_t word)
{
	float f;
	memcpy(&f, &g.xfMem[word], 4);
	return f;
}
inline float xreg(uint32_t reg)
{
	float f;
	memcpy(&f, &g.xfReg[reg], 4);
	return f;
}
void rgba(uint32_t v, float o[4])
{
	o[0] = float((v >> 24) & 255) / 255.0f;
	o[1] = float((v >> 16) & 255) / 255.0f;
	o[2] = float((v >> 8) & 255) / 255.0f;
	o[3] = float(v & 255) / 255.0f;
}

// projection (XF 0x1020) + viewport (XF 0x101A) + 4:3 letterbox + top-screen tilt, as one matrix on
// view-space positions; the same arithmetic the CPU path did per vertex (git history: xform.cpp).
void setFinal()
{
	float p[6], vp[6];
	for (int i = 0; i < 6; i++) {
		p[i] = xreg(XFR_PROJ + i);
		vp[i] = xreg(XFR_VIEWPORT + i);
	}
	const bool ortho = g.xfReg[XFR_PROJ + 6] & 1;
	float P[4][4] = {};
	if (!ortho) {
		P[0][0] = p[0], P[0][2] = p[1];
		P[1][1] = p[2], P[1][2] = p[3];
		P[2][2] = p[4], P[2][3] = p[5];
		P[3][2] = -1.0f;
	} else {
		P[0][0] = p[0], P[0][3] = p[1];
		P[1][1] = p[2], P[1][3] = p[3];
		P[2][2] = p[4], P[2][3] = p[5];
		P[3][3] = 1.0f;
	}
	const float vpx = vp[3] - 342.0f, vpy = vp[4] - 342.0f, S = 16777215.0f;
	// clip (cx, cy, cz, cw) -> PICA (syn, -sxn, -Z, cw): EFB (vpx + xn vp0, vpy + yn vp1) through
	// efbClipX/Y (gx3ds.h)
	const float s = g_efbScale;
	const float M[4][4] = {
	    {0.0f, -2.0f * s * vp[1] / kScreenH, 0.0f, efbClipX(vpy)},
	    {-2.0f * s * vp[0] / kScreenW, 0.0f, 0.0f, efbClipY(vpx)},
	    {0.0f, 0.0f, -vp[2] / S, -vp[5] / S},
	    {0.0f, 0.0f, 0.0f, 1.0f},
	};
	for (int r = 0; r < 4; r++) {
		float row[4];
		for (int c = 0; c < 4; c++)
			row[c] = M[r][0] * P[0][c] + M[r][1] * P[1][c] + M[r][2] * P[2][c] + M[r][3] * P[3][c];
		setU4(u_fin + r, row);
	}
}

void setXf()
{
	for (int r = 0; r < 63; r++)
		setU(u_xf + r, xmem(4 * r), xmem(4 * r + 1), xmem(4 * r + 2), xmem(4 * r + 3));
}

bool setLighting()
{
	bool approx = false;
	const uint32_t numChans = g.xfReg[XFR_NUMCHANS] & 3;
	const uint32_t cc = g.xfReg[XFR_COLOR0CTRL] & 0x7FFF, ac = g.xfReg[XFR_ALPHA0CTRL] & 0x7FFF;
	float amb[4], mat[4];
	rgba(g.xfReg[XFR_AMB0], amb);
	rgba(g.xfReg[XFR_MAT0], mat);
	if (!numChans) {  // no rasterised colour: white, so textures show as they are
		setU(u_mat, 1, 1, 1, 1);
		setU(u_chan, 0, 0, 0, 0);
		for (int i = 0; i < 3; i++)
			setU(u_lcol + i, 0, 0, 0, 0);
		return false;
	}
	setU4(u_amb, amb);
	setU4(u_mat, mat);
	const bool lit = cc & 2, alphaLit = ac & 2;
	setU(u_chan, (cc & 1) ? 1.0f : 0.0f, (cc & 64) ? 1.0f : 0.0f, lit ? 1.0f : 0.0f, alphaLit ? 1.0f : 0.0f);
	if ((ac & 1) != (cc & 1))
		approx = true;  // alpha material source follows colour's
	const uint32_t diff = (cc >> 7) & 3;
	const bool attn = (cc >> 9) & 1, spot = (cc >> 10) & 1;
	setU(u_lmode, diff == 0 ? 1.0f : 0.0f, diff == 1 ? 1.0f : 0.0f, diff >= 2 ? 1.0f : 0.0f, attn && spot ? 1.0f : 0.0f);
	if (attn && !spot)
		approx = true;  // specular attenuation: lit without it
	uint32_t mask = lit ? (((cc >> 2) & 15) | ((cc >> 11) & 15) << 4) : 0;
	int slot = 0;
	for (int l = 0; l < 8 && mask; l++) {
		if (!(mask & (1u << l)))
			continue;
		mask &= ~(1u << l);
		if (slot == 3) {
			approx = true;
			break;
		}
		const uint32_t b = 0x600 + 16 * l;
		float col[4];
		rgba(g.xfMem[b + 3], col);
		setU4(u_lcol + slot, col);
		setU(u_latA + slot, xmem(b + 4), xmem(b + 5), xmem(b + 6), 0);
		setU(u_latK + slot, xmem(b + 7), xmem(b + 8), xmem(b + 9), 0);
		setU(u_lpos + slot, xmem(b + 10), xmem(b + 11), xmem(b + 12), 0);
		setU(u_ldir + slot, xmem(b + 13), xmem(b + 14), xmem(b + 15), 0);
		slot++;
	}
	for (; slot < 3; slot++)
		setU(u_lcol + slot, 0, 0, 0, 0);
	return approx;
}

}  // namespace

void vtxInit(shaderProgram_s* prog)
{
	shaderInstance_s* vs = prog->vertexShader;
	u_xf = shaderInstanceGetUniformLocation(vs, "xf");
	u_fin = shaderInstanceGetUniformLocation(vs, "fin");
	u_amb = shaderInstanceGetUniformLocation(vs, "amb");
	u_mat = shaderInstanceGetUniformLocation(vs, "mat");
	u_lmode = shaderInstanceGetUniformLocation(vs, "lmode");
	u_chan = shaderInstanceGetUniformLocation(vs, "chan");
	u_lcol = shaderInstanceGetUniformLocation(vs, "lcol");
	u_lpos = shaderInstanceGetUniformLocation(vs, "lpos");
	u_ldir = shaderInstanceGetUniformLocation(vs, "ldir");
	u_latA = shaderInstanceGetUniformLocation(vs, "latA");
	u_latK = shaderInstanceGetUniformLocation(vs, "latK");
	u_tgp = shaderInstanceGetUniformLocation(vs, "tgp");
	u_tgsrc = shaderInstanceGetUniformLocation(vs, "tgsrc");
	u_tgsel = shaderInstanceGetUniformLocation(vs, "tgsel");
	vtxInvalidate();
}

uint32_t g_vtxWhy;
uint32_t g_xfMemVersion;
uint32_t g_gxCount[SMS3DS_CNT_N];
static_assert(SMS3DS_CNT_N == 8, "gx3ds.h g_gxCount");

namespace {
// What the previous vtxSetup depended on: when a batch repeats it (common for display-list runs of one
// shape), only the vertex buffer's address changes.
struct SetupKey {
	uint32_t fmt, stride, units, coord[3], xfVersion;
	uint32_t reg[32];
	bool operator==(const SetupKey& o) const { return !memcmp(this, &o, sizeof *this); }
};
SetupKey s_lastKey;
bool s_lastKeyValid, s_lastOk;
uint32_t s_lastEntries;
u64 s_lastPerm;
uint32_t s_lastWhy;

void fillKey(SetupKey& k, const TevPlan& plan, uint32_t fmt, uint32_t stride)
{
	memset(&k, 0, sizeof k);
	k.fmt = fmt;
	k.stride = stride;
	k.units = (uint32_t)plan.units;
	for (int u = 0; u < 3; u++)
		k.coord[u] = u < plan.units ? (uint32_t)plan.coord[u] : ~0u;
	k.xfVersion = g_xfMemVersion;
	static const uint8_t kRegs[] = {XFR_NUMCHANS,   XFR_AMB0,         XFR_MAT0,         XFR_COLOR0CTRL,
	                                XFR_ALPHA0CTRL, XFR_MATIDX_A,     XFR_MATIDX_B,     XFR_NUMTEXGENS,
	                                XFR_VIEWPORT,   XFR_VIEWPORT + 1, XFR_VIEWPORT + 2, XFR_VIEWPORT + 3,
	                                XFR_VIEWPORT + 4, XFR_VIEWPORT + 5, XFR_PROJ,       XFR_PROJ + 1,
	                                XFR_PROJ + 2,   XFR_PROJ + 3,     XFR_PROJ + 4,     XFR_PROJ + 5,
	                                XFR_PROJ + 6,   XFR_TEXGEN,       XFR_TEXGEN + 1,   XFR_TEXGEN + 2,
	                                XFR_TEXGEN + 3, XFR_TEXGEN + 4,   XFR_TEXGEN + 5,   XFR_TEXGEN + 6,
	                                XFR_TEXGEN + 7};
	static_assert(sizeof kRegs <= 32, "SetupKey::reg");
	for (size_t i = 0; i < sizeof kRegs; i++)
		k.reg[i] = g.xfReg[kRegs[i]];
}
}  // namespace

void vtxInvalidate()
{
	memset(s_shadowValid, 0, sizeof s_shadowValid);
	s_lastKeyValid = false;
}

bool vtxSetup(const TevPlan& plan, uint32_t fmt, uint32_t stride, const void* base)
{
	SetupKey key;
	fillKey(key, plan, fmt, stride);
	if (s_lastKeyValid && key == s_lastKey) {
		if (s_lastOk) {
			C3D_BufInfo* bi = C3D_GetBufInfo();
			BufInfo_Init(bi);
			BufInfo_Add(bi, base, stride, (int)s_lastEntries, s_lastPerm);
		}
		g_vtxWhy = s_lastWhy;
		return s_lastOk;
	}
	s_lastKey = key;
	s_lastKeyValid = true;
	g_gxCount[SMS3DS_CNT_VTXMISS]++;
	s_lastOk = false;
	s_lastWhy = 0;
	bool approx = false;
	const VtxFmtLayout& lay = vtxFmtLayout(fmt);
	const uint32_t matA = g.xfReg[XFR_MATIDX_A], matB = g.xfReg[XFR_MATIDX_B];
	const uint32_t defTex[8] = {(matA >> 6) & 63, (matA >> 12) & 63, (matA >> 18) & 63, (matA >> 24) & 63,
	                            matB & 63,        (matB >> 6) & 63,  (matB >> 12) & 63, (matB >> 18) & 63};
	const uint32_t numTexGens = g.xfReg[XFR_NUMTEXGENS] & 15;

	// which texcoord fields the units need (at most two can be loaded: v3, v4)
	int field[2] = {-1, -1};
	auto slotFor = [&](int f) -> int {
		if (f < 0 || !(fmt & (VF_TEX0 << f)))
			return -1;
		for (int s = 0; s < 2; s++)
			if (field[s] == f)
				return s;
		for (int s = 0; s < 2; s++)
			if (field[s] < 0) {
				field[s] = f;
				return s;
			}
		approx = true;
		return -1;
	};

	// texgen uniforms per unit
	for (int u = 0; u < 3; u++) {
		const int c = u < plan.units ? plan.coord[u] : -1;
		float tgp[4] = {1, 0, 0, 0}, src[4] = {0, 0, 0, 0}, sel[4] = {60, 0, 0, 0};  // row 60: identity
		if (c >= 0) {
			if ((uint32_t)c >= numTexGens) {  // no texgen: the raw coordinate
				const int s = slotFor(c);
				if (s >= 0)
					src[2 + s] = 1.0f;
			} else {
				const uint32_t tg = g.xfReg[XFR_TEXGEN + c];
				const uint32_t type = (tg >> 4) & 7, row = (tg >> 7) & 31, proj = (tg >> 1) & 1, form = (tg >> 2) & 1;
				if (type == 2 || type == 3) {
					tgp[3] = 1.0f;  // SRTG
				} else {
					if (type == 1)
						approx = true;  // emboss: as a matrix texgen
					if (row == 0)
						src[0] = 1.0f;
					else if (row == 1)
						src[1] = 1.0f;
					else if (row >= 5) {
						const int s = slotFor((int)(row - 5));
						if (s >= 0)
							src[2 + s] = 1.0f;
					} else {
						approx = true;  // colour / binormal / tangent rows
						src[0] = 1.0f;
					}
					if (form == 0) {  // AB11: z forced to 1
						tgp[0] = 0.0f;
						tgp[1] = 1.0f;
					}
					tgp[2] = proj ? 1.0f : 0.0f;
					if ((fmt & VF_MTX) && c <= 2) {
						sel[0] = 0.0f;
						sel[1 + c] = 1.0f;
					} else {
						sel[0] = (float)defTex[c];
						if (fmt & VF_MTX)
							approx = true;  // per-vertex index of coordinates 3..7
					}
				}
			}
		}
		setU4(u_tgp + u, tgp);
		setU4(u_tgsrc + u, src);
		setU4(u_tgsel + u, sel);
	}

	setXf();
	setFinal();
	const bool lightApprox = setLighting();

	// attribute loaders in buffer order, with padding entries over the skipped bytes
	struct L {
		uint32_t off, size;
		int reg;
		GPU_FORMATS f;
		int n;
	} ls[6];
	int nl = 0;
	ls[nl++] = {0, 12, 0, GPU_FLOAT, 3};
	if (fmt & VF_NRM)
		ls[nl++] = {lay.nrm, 12, 1, GPU_FLOAT, 3};
	if (fmt & VF_CLR0)
		ls[nl++] = {lay.clr[0], 4, 2, GPU_UNSIGNED_BYTE, 4};
	for (int s = 0; s < 2; s++)
		if (field[s] >= 0)
			ls[nl++] = {lay.tex[field[s]], 8, 3 + s, GPU_FLOAT, 2};
	if (fmt & VF_MTX)
		ls[nl++] = {lay.mtx, 4, 5, GPU_UNSIGNED_BYTE, 4};
	std::sort(ls, ls + nl, [](const L& a, const L& b) { return a.off < b.off; });

	C3D_AttrInfo* ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	u64 perm = 0;
	int entries = 0;
	uint32_t cursor = 0;
	for (int i = 0; i < nl; i++) {
		uint32_t gap = ls[i].off - cursor;
		while (gap) {
			const uint32_t pad = gap >= 16 ? 16 : gap;  // 0xC..0xF: 4..16 bytes of padding
			perm |= (u64)(0xB + pad / 4) << (4 * entries++);
			gap -= pad;
		}
		const int id = AttrInfo_AddLoader(ai, ls[i].reg, ls[i].f, ls[i].n);
		perm |= (u64)id << (4 * entries++);
		cursor = ls[i].off + ls[i].size;
	}
	if (entries > 12)
		return false;  // too many padding entries for one buffer: not drawn
	// absent attributes: fixed values (normal +z, white, zero coordinates, default matrix rows)
	if (!(fmt & VF_NRM))
		C3D_FixedAttribSet(AttrInfo_AddFixed(ai, 1), 0.0f, 0.0f, 1.0f, 0.0f);
	if (!(fmt & VF_CLR0))
		C3D_FixedAttribSet(AttrInfo_AddFixed(ai, 2), 255.0f, 255.0f, 255.0f, 255.0f);
	for (int s = 0; s < 2; s++)
		if (field[s] < 0)
			C3D_FixedAttribSet(AttrInfo_AddFixed(ai, 3 + s), 0.0f, 0.0f, 0.0f, 1.0f);
	if (!(fmt & VF_MTX))
		C3D_FixedAttribSet(AttrInfo_AddFixed(ai, 5), (float)(matA & 63), (float)defTex[0], (float)defTex[1],
		                   (float)defTex[2]);

	C3D_BufInfo* bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, base, stride, entries, perm);
	g_vtxWhy = (approx ? AX_TEXGEN : 0) | (lightApprox ? AX_LIGHT : 0);
	s_lastOk = true;
	s_lastEntries = (uint32_t)entries;
	s_lastPerm = perm;
	s_lastWhy = g_vtxWhy;
	return true;
}

}  // namespace gx3ds
