// GX textures as PICA textures (Phase 2b). GameCube texels are decoded to RGBA8 by sms-pc-port's
// decoders (copied from platform/gx/src/gx_texture.cpp, MIT, see NOTICE), resampled to a power of
// two (8..1024, so GX's normalised coordinates still cover the image), and written Morton-tiled in
// the PICA format closest to the source: RGB565 stays RGB565, intensity formats become LA8, the
// rest RGBA8. Entries are cached by address, format, size and palette, and re-checked as
// sms-pc-port does: unchanged while no write stamp touched the source, re-hashed every few frames.
#include "gx3ds.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include "gx_internal.h"

namespace gx3ds {
namespace {

using namespace gx;

// ----------------------------------------------------------------------------------- decoders
inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline void rgb565(uint16_t v, uint8_t* o)
{
	o[0] = uint8_t((v >> 11) << 3 | (v >> 13));
	o[1] = uint8_t(((v >> 5) & 63) << 2 | ((v >> 9) & 3));
	o[2] = uint8_t((v & 31) << 3 | ((v >> 2) & 7));
	o[3] = 255;
}
inline void rgb5a3(uint16_t v, uint8_t* o)
{
	if (v & 0x8000) {
		o[0] = uint8_t(((v >> 10) & 31) << 3 | ((v >> 12) & 7));
		o[1] = uint8_t(((v >> 5) & 31) << 3 | ((v >> 7) & 7));
		o[2] = uint8_t((v & 31) << 3 | ((v >> 2) & 7));
		o[3] = 255;
	} else {
		o[0] = uint8_t(((v >> 8) & 15) * 17);
		o[1] = uint8_t(((v >> 4) & 15) * 17);
		o[2] = uint8_t((v & 15) * 17);
		o[3] = uint8_t(((v >> 12) & 7) * 255 / 7);
	}
}
inline void ia8(uint16_t v, uint8_t* o)
{
	o[0] = o[1] = o[2] = uint8_t(v & 0xFF);
	o[3] = uint8_t(v >> 8);
}
const uint8_t kTileW[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
const uint8_t kTileH[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
const uint8_t kTileBytes[16] = {32, 32, 32, 32, 32, 32, 64, 0, 32, 32, 32, 0, 0, 0, 32, 0};

uint32_t levelBytes(uint32_t fmt, uint32_t w, uint32_t h)
{
	fmt &= 15;
	if (!kTileW[fmt])
		return 0;
	return ((w + kTileW[fmt] - 1) / kTileW[fmt]) * ((h + kTileH[fmt] - 1) / kTileH[fmt]) * kTileBytes[fmt];
}
void palette(const uint8_t* tlut, uint32_t tlutFmt, uint32_t idx, uint8_t* o)
{
	uint16_t v = be16(tlut + 2 * idx);
	if (tlutFmt == 0)
		ia8(v, o);
	else if (tlutFmt == 1)
		rgb565(v, o);
	else
		rgb5a3(v, o);
}

// One mip level to w*h RGBA8, top row first (sms-pc-port gx_texture.cpp decodeTexture).
void decode(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* tlut, uint32_t tlutFmt,
            uint8_t* out)
{
	fmt &= 15;
	uint32_t tw = kTileW[fmt], th = kTileH[fmt];
	if (!tw) {
		memset(out, 0xFF, size_t(w) * h * 4);
		return;
	}
	uint32_t cols = (w + tw - 1) / tw, rows = (h + th - 1) / th;
	const uint8_t* p = src;
	for (uint32_t ty = 0; ty < rows; ty++) {
		for (uint32_t tx = 0; tx < cols; tx++) {
			uint32_t x0 = tx * tw, y0 = ty * th;
			auto put = [&](uint32_t x, uint32_t y, const uint8_t* c) {
				if (x < w && y < h)
					memcpy(out + (size_t(y) * w + x) * 4, c, 4);
			};
			uint8_t c[4];
			switch (fmt) {
			case 0:  // I4
				for (uint32_t i = 0; i < 64; i++) {
					uint8_t b = p[i >> 1];
					uint8_t v = (i & 1) ? (b & 15) : (b >> 4);
					c[0] = c[1] = c[2] = c[3] = uint8_t(v * 17);
					put(x0 + (i & 7), y0 + (i >> 3), c);
				}
				break;
			case 1:  // I8
				for (uint32_t i = 0; i < 32; i++) {
					c[0] = c[1] = c[2] = c[3] = p[i];
					put(x0 + (i & 7), y0 + (i >> 3), c);
				}
				break;
			case 2:  // IA4
				for (uint32_t i = 0; i < 32; i++) {
					c[0] = c[1] = c[2] = uint8_t((p[i] & 15) * 17);
					c[3] = uint8_t((p[i] >> 4) * 17);
					put(x0 + (i & 7), y0 + (i >> 3), c);
				}
				break;
			case 3:  // IA8
				for (uint32_t i = 0; i < 16; i++) {
					ia8(be16(p + 2 * i), c);
					put(x0 + (i & 3), y0 + (i >> 2), c);
				}
				break;
			case 4:  // RGB565
				for (uint32_t i = 0; i < 16; i++) {
					rgb565(be16(p + 2 * i), c);
					put(x0 + (i & 3), y0 + (i >> 2), c);
				}
				break;
			case 5:  // RGB5A3
				for (uint32_t i = 0; i < 16; i++) {
					rgb5a3(be16(p + 2 * i), c);
					put(x0 + (i & 3), y0 + (i >> 2), c);
				}
				break;
			case 6:  // RGBA8: AR plane then GB plane
				for (uint32_t i = 0; i < 16; i++) {
					c[3] = p[2 * i];
					c[0] = p[2 * i + 1];
					c[1] = p[32 + 2 * i];
					c[2] = p[32 + 2 * i + 1];
					put(x0 + (i & 3), y0 + (i >> 2), c);
				}
				break;
			case 8:  // C4
				for (uint32_t i = 0; i < 64; i++) {
					uint8_t b = p[i >> 1];
					palette(tlut, tlutFmt, (i & 1) ? (b & 15) : (b >> 4), c);
					put(x0 + (i & 7), y0 + (i >> 3), c);
				}
				break;
			case 9:  // C8
				for (uint32_t i = 0; i < 32; i++) {
					palette(tlut, tlutFmt, p[i], c);
					put(x0 + (i & 7), y0 + (i >> 3), c);
				}
				break;
			case 10:  // C14X2
				for (uint32_t i = 0; i < 16; i++) {
					palette(tlut, tlutFmt, be16(p + 2 * i) & 0x3FFF, c);
					put(x0 + (i & 3), y0 + (i >> 2), c);
				}
				break;
			case 14:  // CMPR: 2x2 DXT1 sub-blocks
				for (uint32_t sb = 0; sb < 4; sb++) {
					const uint8_t* b = p + sb * 8;
					uint16_t c0 = be16(b), c1 = be16(b + 2);
					uint8_t pal[4][4];
					rgb565(c0, pal[0]);
					rgb565(c1, pal[1]);
					if (c0 > c1) {
						for (int k = 0; k < 3; k++) {
							pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
							pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
						}
						pal[2][3] = pal[3][3] = 255;
					} else {
						for (int k = 0; k < 3; k++) {
							pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
							pal[3][k] = pal[2][k];
						}
						pal[2][3] = 255;
						pal[3][3] = 0;
					}
					uint32_t sx = x0 + (sb & 1) * 4, sy = y0 + (sb >> 1) * 4;
					for (uint32_t r = 0; r < 4; r++) {
						uint8_t bits = b[4 + r];
						for (uint32_t q = 0; q < 4; q++)
							put(sx + q, sy + r, pal[(bits >> (6 - 2 * q)) & 3]);
					}
				}
				break;
			default:
				break;
			}
			p += kTileBytes[fmt];
		}
	}
}

// ----------------------------------------------------------------------------------- PICA layout
uint32_t pot(uint32_t v)
{
	uint32_t p = 8;
	while (p < v && p < 1024)
		p <<= 1;
	return p;
}
// Texel offset inside a PICA texture: 8x8 tiles in rows, Morton order inside a tile. PICA stores
// rows bottom-up (t = 0 samples the last stored row), so callers pass GX row y as h - 1 - y.
inline uint32_t tiled(uint32_t x, uint32_t y, uint32_t w)
{
	static const uint8_t kMorton[8][8] = {
	    {0, 1, 4, 5, 16, 17, 20, 21},     {2, 3, 6, 7, 18, 19, 22, 23},     {8, 9, 12, 13, 24, 25, 28, 29},
	    {10, 11, 14, 15, 26, 27, 30, 31}, {32, 33, 36, 37, 48, 49, 52, 53}, {34, 35, 38, 39, 50, 51, 54, 55},
	    {40, 41, 44, 45, 56, 57, 60, 61}, {42, 43, 46, 47, 58, 59, 62, 63},
	};
	return (((y >> 3) * (w >> 3) + (x >> 3)) << 6) + kMorton[y & 7][x & 7];
}

struct Key {
	const uint8_t* ptr;
	uint32_t fmt, w, h, tlutOff, tlutFmt;
	bool operator==(const Key& o) const
	{
		return ptr == o.ptr && fmt == o.fmt && w == o.w && h == o.h && tlutOff == o.tlutOff && tlutFmt == o.tlutFmt;
	}
};
struct KeyHash {
	size_t operator()(const Key& k) const
	{
		return (size_t)k.ptr * 31u ^ k.fmt * 131u ^ k.w * 7919u ^ k.h * 104729u ^ k.tlutOff * 13u ^ k.tlutFmt;
	}
};
struct Entry {
	C3D_Tex tex;
	bool valid = false;
	uint64_t hash = 0;
	uint32_t bytes = 0;      // source bytes (level 0)
	uint32_t checkedClock = 0, checkedFrame = 0, usedFrame = 0;
	uint32_t texBytes = 0;   // PICA size
};
std::unordered_map<Key, Entry, KeyHash> s_cache;
std::vector<uint8_t> s_rgba;
size_t s_cacheBytes;
const size_t kCacheBudget = 12u << 20;
C3D_Tex s_white;
bool s_whiteInit;

void evictOld()
{
	for (auto it = s_cache.begin(); it != s_cache.end() && s_cacheBytes > kCacheBudget;) {
		if (g_displayFrames - it->second.usedFrame > 30) {  // not used for half a second: not in flight
			if (it->second.valid)
				C3D_TexDelete(&it->second.tex);
			s_cacheBytes -= it->second.texBytes;
			it = s_cache.erase(it);
		} else {
			++it;
		}
	}
}

bool convert(Entry& e, const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* tlut, uint32_t tlutFmt)
{
	s_rgba.resize((size_t)w * h * 4);
	decode(src, fmt, w, h, tlut, tlutFmt, s_rgba.data());
	const uint32_t pw = pot(w), ph = pot(h);
	GPU_TEXCOLOR pf = (fmt == 4) ? GPU_RGB565 : (fmt <= 3) ? GPU_LA8 : GPU_RGBA8;
	if (e.valid && (e.tex.width != pw || e.tex.height != ph || e.tex.fmt != pf)) {
		C3D_TexDelete(&e.tex);
		s_cacheBytes -= e.texBytes;
		e.valid = false;
	}
	if (!e.valid) {
		if (!C3D_TexInit(&e.tex, (u16)pw, (u16)ph, pf))
			return false;
		e.valid = true;
		e.texBytes = e.tex.size;
		s_cacheBytes += e.texBytes;
	}
	uint8_t* dst = (uint8_t*)e.tex.data;
	for (uint32_t y = 0; y < ph; y++) {
		const uint32_t sy = y * h / ph;
		for (uint32_t x = 0; x < pw; x++) {
			const uint8_t* c = &s_rgba[((size_t)sy * w + x * w / pw) * 4];
			const uint32_t o = tiled(x, ph - 1 - y, pw);
			if (pf == GPU_RGBA8) {
				uint8_t* d = dst + o * 4;  // little-endian 0xRRGGBBAA
				d[0] = c[3];
				d[1] = c[2];
				d[2] = c[1];
				d[3] = c[0];
			} else if (pf == GPU_RGB565) {
				uint16_t v = (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | (c[2] >> 3));
				memcpy(dst + o * 2, &v, 2);
			} else {  // LA8: luminance high, alpha low
				dst[o * 2] = c[3];
				dst[o * 2 + 1] = c[0];
			}
		}
	}
	C3D_TexFlush(&e.tex);
	return true;
}

}  // namespace

// The texture for GX map `map` with its sampler state applied, or a 1x1 white texture.
C3D_Tex* textureForMap(int map)
{
	if (!s_whiteInit) {
		C3D_TexInit(&s_white, 8, 8, GPU_RGBA8);
		memset(s_white.data, 0xFF, s_white.size);
		C3D_TexFlush(&s_white);
		s_whiteInit = true;
	}
	const uint32_t mode0 = g.bp[bpTexReg(BP_TX_MODE0, map)];
	const uint32_t img0 = g.bp[bpTexReg(BP_TX_IMAGE0, map)];
	const uint32_t tlutReg = g.bp[bpTexReg(BP_TX_TLUT, map)];
	const uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
	const uint8_t* ptr = g.texImage[map];
	if (!ptr || !levelBytes(fmt, w, h))
		return &s_white;

	const bool ci = fmt == 8 || fmt == 9 || fmt == 10;
	Key key{ptr, fmt, w, h, ci ? (tlutReg & 0x3FF) : 0, ci ? ((tlutReg >> 10) & 3) : 0};
	const uint8_t* tlut = ci ? g.tlutMem + ((tlutReg & 0x3FF) << 9) : nullptr;
	const uint32_t bytes = levelBytes(fmt, w, h);
	Entry& e = s_cache[key];
	e.usedFrame = g_displayFrames;
	bool check = !e.valid || g_displayFrames - e.checkedFrame >= kRecheckFrames ||
	             !unwrittenSince(ptr, bytes, e.checkedClock);
	if (check) {
		uint64_t hsh = hashBytes(ptr, bytes, tlut ? hashBytes(tlut, fmt == 8 ? 32 : 512) : 0);
		e.checkedClock = writeClock();
		e.checkedFrame = g_displayFrames;
		if (!e.valid || hsh != e.hash || e.bytes != bytes) {
			if (!convert(e, ptr, fmt, w, h, tlut, key.tlutFmt))
				return &s_white;
			e.hash = hsh;
			e.bytes = bytes;
			if (s_cacheBytes > kCacheBudget)
				evictOld();
		}
	}
	static const GPU_TEXTURE_WRAP_PARAM kWrap[4] = {GPU_CLAMP_TO_EDGE, GPU_REPEAT, GPU_MIRRORED_REPEAT, GPU_REPEAT};
	C3D_TexSetWrap(&e.tex, kWrap[mode0 & 3], kWrap[(mode0 >> 2) & 3]);
	const GPU_TEXTURE_FILTER_PARAM mag = (mode0 >> 4) & 1 ? GPU_LINEAR : GPU_NEAREST;
	const GPU_TEXTURE_FILTER_PARAM min = ((mode0 >> 5) & 4) ? GPU_LINEAR : GPU_NEAREST;
	C3D_TexSetFilter(&e.tex, mag, min);
	return &e.tex;
}

}  // namespace gx3ds
