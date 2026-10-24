// The JAudio DSP task's voices played by the 3DS DSP (ndsp), in place of sms-pc-port's software mixer
// (platform/audio/dsp_mixer.cpp, same entry points: dsp_mixer.h).
//
// The game describes up to 64 voices in its DSPBuffer blocks and the DSP HLE (dsp_hle.cpp) asks for one
// 80-sample subframe at a time. Mixing them on the ARM11 cost 8-17 ms a frame in Delfino Plaza. Here
// the CPU only decodes what each voice consumes in the subframe (its AFC ADPCM, PCM or stream ring, at
// the source rate) and queues it on an ndsp channel playing at the voice's pitch; the DSP resamples,
// applies volume and pan, and mixes. By default (s_allSoft) the CPU resamples and mixes them instead,
// into one stereo stream on one channel: the DSP's own mixing crinkled. Sunshine's AFC ADPCM is not the DSP-ADPCM the 3DS decodes (9-byte
// frames of 16 samples, a shared 16-entry predictor table), so the wave data stays in ARAM as it is.
//
// Voice semantics (start, pause, loops with the stored ADPCM history, stop requests halving the
// volume each subframe, stream ring positions, oscillators) follow dsp_mixer.cpp (MIT, see NOTICE);
// PROTOCOL.md there has the evidence. Approximations: pitch changes once per queued buffer (up to 7
// subframes, 17.5 ms) and a pan change reaches audio already queued; the per-voice filters are not
// played (the FX lines' echo is, in the software mix: fxSub); the game hears "done" when a one-shot's last sample is decoded, a few frames before
// the DSP has played it. Only voices with volume get one of the 24 channels; when more are audible
// the quietest goes without.
#include <3ds.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp_mixer.h"

// For scripts/azahar.py: voices this subframe, channels holding a voice, buffers queued in all,
// buffers dropped (the DSP that far behind), audible voices left without a channel, ndsp frames run,
// ndsp frames dropped.
extern "C" volatile uint32_t sms3ds_audio_stats[8];
volatile uint32_t sms3ds_audio_stats[8];
// The level meter (debug builds; scripts/azahar.py audio): the loudest output sample of the last
// second and of all time (32767 is full scale), output samples and subframes past full scale (the DSP
// saturates them: crackle), voice buffers that found their channel already played out (a gap),
// subframes with an echo (FX return), subframes metered, and subframes written to SMS_AUDIO_DUMP.
extern "C" volatile uint32_t sms3ds_audio_meter[8];
volatile uint32_t sms3ds_audio_meter[8];
// CPU time the voices took (decoding, mixing, queueing), in microseconds per second of audio: the
// last second's and the most in any second so far.
extern "C" volatile uint32_t sms3ds_audio_cpu_us[2];
volatile uint32_t sms3ds_audio_cpu_us[2];

namespace {

// --- game-side blocks (host byte order), as dsp_mixer.cpp --------------------------------------
struct MixSlot {
	uint16_t bus, target, current, level;
};
struct Voice {
	uint16_t enabled;           // 0x000
	uint16_t done;              // 0x002
	uint16_t ratio;             // 0x004 Q12
	uint16_t pad006;            // 0x006
	uint16_t resetVpb;          // 0x008
	uint16_t endReached;        // 0x00A
	uint16_t pause;             // 0x00C
	uint16_t delayMax;          // 0x00E
	MixSlot slot[6];            // 0x010
	uint8_t pad040[0x10];       // 0x040
	uint16_t amPan;             // 0x050
	uint16_t amFx;              // 0x052
	uint16_t amVolCurrent;      // 0x054
	uint16_t amVolTarget;       // 0x056
	uint16_t amEnabled;         // 0x058
	uint8_t pad05A[6];          // 0x05A
	int16_t posFrac;            // 0x060
	uint16_t pad062;            // 0x062
	int16_t blockSamples;       // 0x064
	int16_t constantSample;     // 0x066
	int32_t position;           // 0x068
	uint32_t samplesBeforeLoop; // 0x06C
	uint32_t aramAddr;          // 0x070
	uint32_t remaining;         // 0x074
	uint8_t pad078[0x88];       // 0x078
	uint16_t sourceType;        // 0x100
	uint16_t looping;           // 0x102
	int16_t loopYN1;            // 0x104
	int16_t loopYN2;            // 0x106
	int16_t filterMode;         // 0x108
	uint16_t endRequested;      // 0x10A
	uint32_t cpuTicks;          // 0x10C
	uint32_t loopStart;         // 0x110
	uint32_t endPosition;       // 0x114
	uint32_t base;              // 0x118
	int32_t sampleCount;        // 0x11C
	uint8_t pad120[0x60];       // 0x120
};
static_assert(sizeof(Voice) == 0x180, "DSPBuffer is 0x180 bytes");
static_assert(offsetof(Voice, amPan) == 0x50, "dolbyVoicePosition");
static_assert(offsetof(Voice, posFrac) == 0x60, "currentPosFrac");
static_assert(offsetof(Voice, sourceType) == 0x100, "samplesSourceType");
static_assert(offsetof(Voice, base) == 0x118, "baseAddress");

const uint32_t kAdpcmDefault[16] = {
    0x0,        0x08000000, 0x00000800, 0x04000400, 0x1000f800, 0x0e00fa00, 0x0c00fc00, 0x1200f600,
    0x1068f738, 0x12c0f704, 0x1400f400, 0x0800f800, 0x0400fc00, 0xfc000400, 0xfc000000, 0xf8000000,
};
enum { kDirectPcm = 0x21 };
enum { kOscSquare = 0, kOscSaw = 1, kOscSquare25 = 3, kOscPattern1 = 4, kOscPattern0 = 7, kOscPattern0V = 10,
       kOscPattern2 = 11, kOscPattern3 = 12 };
const uint16_t kBusLeft = 0x0D00, kBusRight = 0x0D60;  // buses 1 and 2: the dry stereo pair
// Buses 3-6 feed FX lines 0-3 (echo): music's mix configurations send to 3/4, the auto mixer to 5/6.
const uint16_t kBusFx[4] = {0x0DC0, 0x0E20, 0x0E80, 0x0EE0};

// DSPInterface::FXBuffer, as dsp_mixer.cpp (PROTOCOL.md, FX lines): a delay line in main memory whose
// output returns to buses (A, B) through an 8-tap FIR, and feeds back into itself with the new sends.
struct FxLine {
	uint16_t mode;    // 0 off; bit 0 filters the return, bit 1 the feedback
	uint16_t length;  // delay in 80-sample subframes
	uint32_t buffer;  // main-memory delay buffer
	uint16_t busA;
	int16_t gainA;    // Q15
	uint16_t busB;
	int16_t gainB;
	int16_t taps[8];  // Q15
};
static_assert(sizeof(FxLine) == 0x20, "FXBuffer is 0x20 bytes");
// The output rate the voices play at. The GameCube's DAC runs at 32028.5 Hz (JASRate.cpp), but the
// port's AI clock (ai.cpp) has the game make its audio at 32000 samples a second: played at 32028.5
// a voice's queued audio shrank by 0.9 ms a second until it ran dry (a gap). 1.5 cents lower.
const float kOutRate = 32000.0f;
// A note's first buffer starts after this much silence: the game makes its audio in bursts (the
// audio thread runs when the game's thread lets it), and a voice's queue has to last between them.
const float kPrerollSeconds = 0.040f;

// --- ndsp channels ---------------------------------------------------------------------------------
enum {
	kChannels = 24,
	// The DSP has 24 channels and busy scenes have more voices (Delfino Hotel ~34): the last channel
	// plays a stereo mix, made on the CPU, of the voices that get no channel of their own.
	kVoiceChans = kChannels - 1,
	kMixChan = kChannels - 1,
	kMixSubs = 16,        // subframes of that mix being filled (a buffer reaches back kSubPerBuf)
	kSubMax = 160,        // output samples per subframe at most
	kBufs = 64,           // queued buffers per channel: room for the game's audio catching up in a burst
	                      // (a voice whose pitch moves every subframe submits a buffer per subframe)
	kSubPerBuf = 7,       // subframes per buffer at most: one JAudio frame
	kBufSamples = 2048,   // 7 x 80 up to a ratio of 3.6; higher pitches submit after fewer subframes
};
// A voice's samples carry its volume over this (the channel's mix gives it back): a voice's total gain
// reaches about 1.25 (a stream's 0x7FFF sends at the 0.625 master), so samples keep headroom.
const float kGainScale = 2.0f;
struct Chan {
	int voice = -1;  // the voice it plays, or -1 (free, or still playing out a finished voice)
	float rate = -1.0f, mixL = -1.0f, mixR = -1.0f;  // as last set (ndsp calls only on change)
	uint32_t gen = 0;  // bumped by chanReset: a buffer queued for its previous use is not added
	uint32_t queuedAt = 0;  // the render call that last queued a buffer, + 1
	// The rate and mix of each queued buffer, in queue order (s_paramLock). ndsp applies a channel's
	// rate and mix to whatever it plays from its next frame on, so they are set by the frame callback
	// (onDspFrame) as playback reaches the buffer they belong to, not when it is queued (40 ms and
	// more earlier: pitch changes were applied to the wrong audio, which crackled).
	struct Params {
		uint16_t seq;
		uint32_t nsamples;
		float rate, mixL, mixR;
	} params[kBufs];
	uint32_t pHead = 0, pTail = 0;  // entries pHead..pTail-1 (mod kBufs)
	ndspWaveBuf wb[kBufs];
	int next = 0;
};

struct Decoder {
	bool active, ended;
	uint32_t idx;      // next source sample
	int32_t h1, h2;    // ADPCM history
	uint32_t frac;     // Q16 fraction of the source position (the DSP's resampling phase)
	uint32_t frame;    // ADPCM: the frame `fp` points at, + 1 (0: none)
	const uint8_t* fp;
	int32_t c1, c2, scale;
	uint32_t phase;    // oscillators: Q22 position in the period
	uint32_t remaining, written;
	int chan;          // ndsp channel, or -1
	int quietSubs;     // subframes in a row with no volume (the channel is given up after a while)
	// A buffer plays at one rate and one channel mix, but the game changes a voice's pitch (vibrato,
	// slides) and volume (envelopes, fades) every subframe. The staged samples keep the pitch they
	// were consumed at (a change starts a new buffer), and their volume is ramped per sample from
	// subframe to subframe here, in the samples: ndsp applies a channel's mix at once, to the audio
	// it still has queued too, so a volume held there stepped (clicked) a buffer early. The mix holds
	// only the pan, at kGainScale.
	uint16_t bufRatio;      // the pitch of the staged samples
	float prevGain;         // the total gain at the end of the last buffer (the next ramps from it)
	float pan;              // left share of the total gain, as last audible
	float wet[4];           // the FX lines' sends as shares of the total gain (master-free: see fxSub)
	bool hasWet;
	uint32_t subEnd[kSubPerBuf];  // staged samples at the end of each staged subframe
	float subGain[kSubPerBuf];    // and the voice's total gain (left + right) there
	uint32_t lastSub;             // the render call (subframe count) of the last staged subframe
	uint32_t contSub;             // the subframe after the last queued buffer, + 1 (0: none)
	bool queuedAny;               // this note has queued a buffer (the first starts with the preroll)
	int16_t stage[kBufSamples];
	uint32_t staged;   // samples in stage
	uint32_t stagedSubs;
	float gainL, gainR;
};

struct State {
	Voice* voices;
	uint32_t nvoices;
	int16_t pattern[4][64];
	int16_t coef[16][2];
	port_dspmix_aram_fn aram;
	Decoder dec[64];
	Chan chan[kChannels];
	// the software mix (kMixChan): stereo sums by subframe, submitted kSubPerBuf subframes late
	int32_t mixAcc[kMixSubs][2 * kSubMax];
	bool mixUsed[kMixSubs];
	uint32_t subNow;    // render calls so far: the current subframe
	uint32_t mixNext;   // the next subframe of the mix to submit
	uint32_t subN;      // output samples per subframe (80)
	uint32_t softMixed; // voice buffers mixed on the CPU
	// echo: the voices' sends to the FX lines by subframe (as mixAcc), and each line's state
	int32_t wetAcc[kMixSubs][4][kSubMax];
	FxLine* fx;
	port_dspmix_mram_fn mram;
	uint32_t fxBuffer[4], fxLength[4], fxPos[4];
	int32_t fxHist[4][8];
	float master;       // the game's master level, for the FX return (the voices' gains carry it)
	bool ndspOk;
	int active;
	uint32_t overruns, unassigned;
	bool trace;
	uint32_t traceTick;
#if SMS3DS_DEBUG
	// the level meter: every voice buffer, as the DSP mixes them, by subframe (as the software mix)
	int32_t meterAcc[kMixSubs][2 * kSubMax];
	bool meterUsed[kMixSubs];
	uint32_t meterPeak, meterSubs;
	FILE* dump;
	uint32_t dumpFrames;
#endif
} g;

inline int32_t sat16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
const uint8_t* aram(uint32_t a) { return g.aram ? g.aram(a) : nullptr; }

inline int32_t decode_at(Decoder& d, const Voice& v)
{
	const uint32_t i = d.idx;
	switch (v.sourceType) {
	case 9:
	case 5: {
		const uint32_t fbytes = v.sourceType;
		if ((i >> 4) + 1 != d.frame) {  // a new frame: its address and header, once
			d.frame = (i >> 4) + 1;
			d.fp = aram(v.base + (i >> 4) * fbytes);
			if (d.fp) {
				d.scale = d.fp[0] >> 4;
				d.c1 = g.coef[d.fp[0] & 0xF][0];
				d.c2 = g.coef[d.fp[0] & 0xF][1];
			}
		}
		const uint8_t* p = d.fp;
		if (!p)
			return 0;
		const int k = i & 15;
		int r;
		if (fbytes == 9) {
			const uint8_t b = p[1 + (k >> 1)];
			r = (k & 1) ? (b & 0xF) : (b >> 4);
			r = (r ^ 8) - 8;
		} else {
			const uint8_t b = p[1 + (k >> 2)];
			r = (b >> (6 - 2 * (k & 3))) & 3;
			r = ((r ^ 2) - 2) * 4;
		}
		int32_t s = ((int32_t)(r << d.scale) << 11) + d.c1 * d.h1 + d.c2 * d.h2;
		s = sat16(s >> 11);
		d.h2 = d.h1;
		d.h1 = s;
		return s;
	}
	case 8: {
		const uint8_t* p = aram(v.base + i);
		return p ? (int8_t)p[0] * 256 : 0;
	}
	case 16: {
		const uint8_t* p = aram(v.base + i * 2);
		return p ? (int16_t)((p[0] << 8) | p[1]) : 0;
	}
	case kDirectPcm:
		return v.base ? ((const int16_t*)(uintptr_t)v.base)[i] : 0;
	}
	return 0;
}

// The next source sample, following loops; ends a one-shot (d.ended) and returns 0 past its end.
inline int32_t pull(Decoder& d, const Voice& v)
{
	if (d.ended)
		return 0;
	if (v.sourceType == kDirectPcm) {
		const uint32_t ring = v.endPosition >> 16;
		if (d.remaining == 0 || !ring) {
			d.ended = true;
			return 0;
		}
		if (d.remaining != 0xFFFFFFFFu)
			d.remaining--;
		if (d.idx >= ring)
			d.idx = 0;
		const int32_t s = decode_at(d, v);
		d.idx++;
		return s;
	}
	if (d.idx >= v.endPosition) {
		if (v.looping && v.loopStart < v.endPosition) {
			if (v.sourceType == 9 || v.sourceType == 5) {
				d.idx = v.loopStart & ~15u;  // the stored history belongs to the loop start's frame
				d.h1 = v.loopYN1;
				d.h2 = v.loopYN2;
				while (d.idx < v.loopStart) {
					decode_at(d, v);
					d.idx++;
				}
			} else {
				d.idx = v.loopStart;
			}
		} else {
			d.ended = true;
			return 0;
		}
	}
	const int32_t s = decode_at(d, v);
	d.idx++;
	return s;
}

// `count` source samples of an AFC ADPCM voice (4- or 2-bit) into the stage, a frame at a time:
// the same samples pull() gives, without its per-sample dispatch.
void decode_adpcm_run(Decoder& d, const Voice& v, uint32_t count)
{
	const uint32_t fbytes = v.sourceType;
	int32_t h1 = d.h1, h2 = d.h2;
	while (count && !d.ended) {
		if (d.idx >= v.endPosition) {
			d.h1 = h1;
			d.h2 = h2;
			pull(d, v);  // the loop (or the end) as pull() handles it; that sample is the loop start
			if (d.ended)
				break;
			h1 = d.h1;
			h2 = d.h2;
			if (d.staged < kBufSamples)
				d.stage[d.staged++] = (int16_t)h1;
			count--;
			continue;
		}
		const uint32_t f = d.idx >> 4, k0 = d.idx & 15;
		uint32_t run = 16 - k0;
		if (run > count)
			run = count;
		if (run > v.endPosition - d.idx)
			run = v.endPosition - d.idx;
		const uint8_t* p = aram(v.base + f * fbytes);
		if (!p) {
			d.ended = true;
			break;
		}
		const int scale = p[0] >> 4;
		const int32_t c1 = g.coef[p[0] & 0xF][0], c2 = g.coef[p[0] & 0xF][1];
		int16_t* out = d.stage + d.staged;
		const uint32_t room = kBufSamples - d.staged;
		for (uint32_t j = 0; j < run; j++) {
			const uint32_t k = k0 + j;
			int r;
			if (fbytes == 9) {
				const uint8_t b = p[1 + (k >> 1)];
				r = (int)((k & 1) ? (b & 0xF) : (b >> 4));
				r = (r ^ 8) - 8;
			} else {
				const uint8_t b = p[1 + (k >> 2)];
				r = (b >> (6 - 2 * (k & 3))) & 3;
				r = ((r ^ 2) - 2) * 4;
			}
			const int32_t y = sat16((((int32_t)(r << scale) << 11) + c1 * h1 + c2 * h2) >> 11);
			h2 = h1;
			h1 = y;
			if (j < room)
				out[j] = (int16_t)y;
		}
		d.staged += run < room ? run : room;
		d.idx += run;
		count -= run;
	}
	d.h1 = h1;
	d.h2 = h2;
	d.frame = 0;  // decode_at refetches its frame
}

float s_preroll = kPrerollSeconds;  // SMS_AUDIO_PREROLL_MS (debug builds)
// Every voice in the software mix, one stereo stream for the DSP to play. Played on the DSP's own
// channels (SMS_AUDIO_SOFTMIX=0) the voices crinkled where the same samples mixed here did not (by
// ear, 2026-10-24; still after the per-buffer rates of onDspFrame). In Delfino Plaza the mix costs
// 15-20 ms of CPU a second more than the channels (Azahar), and the frame rate is unchanged.
bool s_allSoft = true;

// cos and sin of the auto mixer's pan (0..127 -> 0..pi/2)
float s_panCos[128], s_panSin[128];

int pattern_index(uint16_t t)
{
	switch (t) {
	case kOscPattern0:
	case kOscPattern0V: return 0;
	case kOscPattern1: return 1;
	case kOscPattern2: return 2;
	case kOscPattern3: return 3;
	}
	return -1;
}
bool is_oscillator(uint16_t t) { return t == kOscSquare || t == kOscSaw || t == kOscSquare25 || pattern_index(t) >= 0; }
bool supported(uint16_t t) { return t == 9 || t == 5 || t == 8 || t == 16 || t == kDirectPcm || is_oscillator(t); }

// --- channels --------------------------------------------------------------------------------------
// The samples of every queued buffer, in one ring in linear memory: the buffers a render call fills
// lie next to each other, so the data cache is flushed once per call (a flush per buffer was 1 ms a
// frame). A channel has the preroll and a burst or two of the game's audio queued, ~100 ms, so the
// ring (~1.2 s of data at the plaza's rate) is never written where a queued buffer still waits.
const uint32_t kRingSamples = 512 * 1024;
int16_t* s_ring;
uint32_t s_ringHead, s_flushFrom;
struct Pending {
	int chan;
	uint32_t gen;
	ndspWaveBuf* wb;
	float rate, mixL, mixR;  // for a voice channel's buffer (the software mix's are fixed: rate < 0)
};
LightLock s_paramLock;
Pending s_pending[64];
int s_npending;

void flushRing(uint32_t from, uint32_t to)
{
	if (to > from)
		DSP_FlushDataCache(s_ring + from, (to - from) * sizeof(int16_t));
}

int16_t* ringTake(uint32_t n)
{
	if (s_ringHead + n > kRingSamples) {  // wrap: what this call wrote before the end goes out now
		flushRing(s_flushFrom, s_ringHead);
		s_ringHead = s_flushFrom = 0;
	}
	int16_t* p = s_ring + s_ringHead;
	s_ringHead += n;
	return p;
}

// After a render call: one flush, then the buffers it filled go to their channels.
void addPending()
{
	flushRing(s_flushFrom, s_ringHead);
	s_flushFrom = s_ringHead;
	for (int i = 0; i < s_npending; i++) {
		const Pending& p = s_pending[i];
		Chan& ch = g.chan[p.chan];
		if (ch.gen != p.gen)
			continue;
		LightLock_Lock(&s_paramLock);
		ndspChnWaveBufAdd(p.chan, p.wb);  // gives the buffer its sequence number
		if (p.rate > 0.0f) {
			if (ch.pTail - ch.pHead >= (uint32_t)kBufs)
				ch.pHead++;  // cannot happen: one entry per queued buffer
			ch.params[ch.pTail++ % kBufs] = Chan::Params{p.wb->sequence_id, p.wb->nsamples, p.rate, p.mixL, p.mixR};
		}
		LightLock_Unlock(&s_paramLock);
	}
	s_npending = 0;
}

void applyParams(int c, const Chan::Params& p)
{
	Chan& ch = g.chan[c];
	if (p.rate != ch.rate) {
		ch.rate = p.rate;
		ndspChnSetRate(c, p.rate);
	}
	if (p.mixL != ch.mixL || p.mixR != ch.mixR) {
		ch.mixL = p.mixL;
		ch.mixR = p.mixR;
		float mix[12] = {};
		mix[0] = p.mixL;
		mix[1] = p.mixR;
		ndspChnSetMix(c, mix);
	}
}

// ndsp's frame callback (its thread, after it read the channels' state and before it sends their
// updates for the next frame of 160 output samples): each voice channel gets the rate and mix of
// the buffer that plays most of that frame.
void onDspFrame(void*)
{
	const float kFrameHalf = 80.0f, kNdspRate = 32728.5f;
	LightLock_Lock(&s_paramLock);
	for (int c = 0; c < kVoiceChans; c++) {
		Chan& ch = g.chan[c];
		if (ch.pHead == ch.pTail)
			continue;
		const uint16_t cur = ndspChnGetWaveBufSeq(c);
		uint32_t i = ch.pHead;
		while (i != ch.pTail && ch.params[i % kBufs].seq != cur)
			i++;
		if (i == ch.pTail) {  // nothing of ours playing yet: the first queued buffer starts
			applyParams(c, ch.params[ch.pHead % kBufs]);
			continue;
		}
		ch.pHead = i;  // the buffers before it have played
		const Chan::Params& p = ch.params[i % kBufs];
		const uint32_t pos = ndspChnGetSamplePos(c);
		const float left = pos < p.nsamples ? (float)(p.nsamples - pos) * kNdspRate / p.rate : 0.0f;
		applyParams(c, left < kFrameHalf && i + 1 != ch.pTail ? ch.params[(i + 1) % kBufs] : p);
	}
	LightLock_Unlock(&s_paramLock);
}
bool chanIdle(int c)
{
	for (int b = 0; b < kBufs; b++)
		if (g.chan[c].wb[b].status == NDSP_WBUF_QUEUED || g.chan[c].wb[b].status == NDSP_WBUF_PLAYING)
			return false;
	return true;
}

void chanReset(int c)
{
	ndspChnReset(c);
	ndspChnSetInterp(c, NDSP_INTERP_POLYPHASE);
	ndspChnSetFormat(c, NDSP_FORMAT_MONO_PCM16);
	Chan& ch = g.chan[c];
	for (int b = 0; b < kBufs; b++)
		memset(&ch.wb[b], 0, sizeof ch.wb[b]);
	ch.next = 0;
	ch.gen++;
	LightLock_Lock(&s_paramLock);
	ch.rate = ch.mixL = ch.mixR = -1.0f;
	ch.pHead = ch.pTail = 0;
	LightLock_Unlock(&s_paramLock);
}

// Lets a voice's channel play out what it has queued (a finished one-shot) or stops it at once.
void releaseChan(Decoder& d, bool now)
{
	if (d.chan < 0)
		return;
	if (now && g.ndspOk)
		chanReset(d.chan);
	g.chan[d.chan].voice = -1;
	d.chan = -1;
}

int takeChan(int vi, float loudness)
{
	int pick = -1;
	for (int c = 0; c < kVoiceChans && pick < 0; c++)
		if (g.chan[c].voice < 0 && chanIdle(c))
			pick = c;
	for (int c = 0; c < kVoiceChans && pick < 0; c++)
		if (g.chan[c].voice < 0)  // still playing out a finished voice
			pick = c;
	if (pick < 0) {  // all taken: the quietest voice gives its channel up if quieter than this one
		float least = loudness;
		for (int c = 0; c < kVoiceChans; c++) {
			const Decoder& o = g.dec[g.chan[c].voice];
			const float l = o.gainL + o.gainR;
			if (l < least) {
				least = l;
				pick = c;
			}
		}
		if (pick < 0)
			return -1;
		g.dec[g.chan[pick].voice].chan = -1;
	}
	chanReset(pick);
	g.chan[pick].voice = vi;
	return pick;
}

// Adds a voice buffer to a stereo mix kept by subframe (the software mix, the level meter): its n
// samples (volume already ramped) are stretched linearly over the `subs` subframes they were consumed
// in, ending at d.lastSub, and added with the pan (the volume is in the samples, over kGainScale).
// With `wet`, the voice's FX sends go to wetAcc too; acc may then be null (the dry signal plays on a
// channel of its own).
void accumulate(int32_t (*acc)[2 * kSubMax], bool* used, bool wet, const Decoder& d, uint32_t n, uint32_t subs)
{
	const uint32_t sn = g.subN ? g.subN : 80, outN = subs * sn;
	const uint32_t first = d.lastSub + 1 - subs;
	if (first < g.mixNext || !n)
		return;  // already submitted (cannot happen: buffers reach back kSubPerBuf at most)
	// gains in Q14 (kGainScale x pan, up to 2.0), the source position in Q16
	const int32_t gl = (int32_t)(kGainScale * d.pan * 16384.0f), gr = (int32_t)(kGainScale * 16384.0f) - gl;
	int32_t gw[4];
	int lines[4], nl = 0;
	for (int j = 0; wet && d.hasWet && j < 4; j++)
		if ((gw[j] = (int32_t)(kGainScale * d.wet[j] * 16384.0f)) != 0)
			lines[nl++] = j;
	if (!acc && !nl)
		return;
	const uint32_t step = (uint32_t)(((uint64_t)n << 16) / outN);
	const int16_t* st = d.stage;
	const uint32_t last = n - 1;
	uint32_t p = 0;
	for (uint32_t sub = 0; sub < subs; sub++) {
		const uint32_t slot = (first + sub) % kMixSubs;
		int32_t* a = acc ? acc[slot] : nullptr;
		if (acc)
			used[slot] = true;
		for (uint32_t k = 0; k < sn; k++, p += step) {
			const uint32_t i = p >> 16;
			const int32_t s0 = st[i], s1 = st[i < last ? i + 1 : i];
			const int32_t smp = s0 + (((s1 - s0) * (int32_t)((p & 0xFFFF) >> 1)) >> 15);
			if (a) {
				a[2 * k] += (smp * gl) >> 14;
				a[2 * k + 1] += (smp * gr) >> 14;
			}
			for (int j = 0; j < nl; j++)
				g.wetAcc[slot][lines[j]][k] += (smp * gw[lines[j]]) >> 14;
		}
	}
}

// A voice buffer with no channel of its own (all taken by louder voices) goes into the software mix.
void softMix(const Decoder& d, uint32_t n, uint32_t subs)
{
	accumulate(g.mixAcc, g.mixUsed, true, d, n, subs);
	g.softMixed++;
}

#if SMS3DS_DEBUG
void meterAdd(const Decoder& d, uint32_t n, uint32_t subs)
{
	accumulate(g.meterAcc, g.meterUsed, false, d, n, subs);
}

void dumpHeader()
{
	const uint32_t rate = (uint32_t)(kOutRate + 0.5f), bytes = g.dumpFrames * 8;
	uint8_t h[44];
	memcpy(h, "RIFF", 4);
	const uint32_t riff = 36 + bytes;
	memcpy(h + 4, &riff, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	const uint32_t fmtLen = 16, byteRate = rate * 8;
	const uint16_t fmt = 3, chans = 2, align = 8, bits = 32;  // float32 stereo: the sum before saturation
	memcpy(h + 16, &fmtLen, 4);
	memcpy(h + 20, &fmt, 2);
	memcpy(h + 22, &chans, 2);
	memcpy(h + 24, &rate, 4);
	memcpy(h + 28, &byteRate, 4);
	memcpy(h + 32, &align, 2);
	memcpy(h + 34, &bits, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &bytes, 4);
	fseek(g.dump, 0, SEEK_SET);
	fwrite(h, 1, sizeof h, g.dump);
	fseek(g.dump, 0, SEEK_END);
}

// The meter's subframe `sub`, complete: its loudest sample, the samples past full scale, the dump.
void meterSub(uint32_t sub)
{
	const uint32_t slot = sub % kMixSubs, sn = g.subN;
	int32_t* acc = g.meterAcc[slot];
	if (g.meterUsed[slot]) {
		g.meterUsed[slot] = false;
		uint32_t clipped = 0, peak = g.meterPeak;
		for (uint32_t i = 0; i < 2 * sn; i++) {
			const uint32_t a = (uint32_t)(acc[i] < 0 ? -acc[i] : acc[i]);
			peak = a > peak ? a : peak;
			clipped += a > 32767;
		}
		g.meterPeak = peak;
		if (clipped) {
			sms3ds_audio_meter[2] = sms3ds_audio_meter[2] + clipped;
			sms3ds_audio_meter[3] = sms3ds_audio_meter[3] + 1;
		}
	}
	if (g.dump) {
		float out[2 * kSubMax];
		for (uint32_t i = 0; i < 2 * sn; i++)
			out[i] = (float)acc[i] / 32768.0f;
		fwrite(out, sizeof(float), 2 * sn, g.dump);
		g.dumpFrames += sn;
		sms3ds_audio_meter[7] = g.dumpFrames / sn;
		if (sub % 400 == 0)
			dumpHeader();  // the file stays readable when the game is quit
	}
	memset(acc, 0, 2 * sn * sizeof(int32_t));
	sms3ds_audio_meter[6] = sms3ds_audio_meter[6] + 1;
	if (++g.meterSubs >= 400) {  // about a second
		sms3ds_audio_meter[0] = g.meterPeak;
		if (g.meterPeak > sms3ds_audio_meter[1])
			sms3ds_audio_meter[1] = g.meterPeak;
		g.meterPeak = 0;
		g.meterSubs = 0;
	}
}
#endif

// One subframe of the FX lines (echo), as dsp_mixer.cpp prepare_fx/finish_fx, once every voice's
// sends to it are in (wetAcc[slot]): each line's delay output, filtered, returns to the stereo pair
// (into `out`, with the master level), and feeds back into the delay with the new sends. Whether
// anything came back.
bool fxSub(uint32_t slot, uint32_t n, int32_t* out)
{
	bool any = false;
	for (int i = 0; i < 4; i++) {
		int32_t* wet = g.wetAcc[slot][i];
		const FxLine* f = g.fx ? &g.fx[i] : nullptr;
		const uint32_t len = f ? (uint32_t)f->length * 80 : 0;
		int16_t* delay = f && f->mode && f->buffer && len
		                     ? (g.mram ? g.mram(f->buffer, len) : (int16_t*)(uintptr_t)f->buffer)
		                     : nullptr;
		if (!delay) {
			g.fxBuffer[i] = 0;
			memset(wet, 0, n * sizeof(int32_t));
			continue;
		}
		if (g.fxBuffer[i] != f->buffer || g.fxLength[i] != len) {
			fprintf(stderr, "[audio] echo line %d: %lu ms, mode %u, returns %04x x %d / %04x x %d\n", i,
			        (unsigned long)(len * 1000 / 32000), (unsigned)f->mode, (unsigned)f->busA, (int)f->gainA,
			        (unsigned)f->busB, (int)f->gainB);
			g.fxBuffer[i] = f->buffer;
			g.fxLength[i] = len;
			g.fxPos[i] = 0;
			memset(g.fxHist[i], 0, sizeof g.fxHist[i]);
		}
		int32_t window[kSubMax + 8];
		memcpy(window, g.fxHist[i], sizeof g.fxHist[i]);
		const uint32_t pos = g.fxPos[i];
		const uint32_t run = len - pos < n ? len - pos : n;  // the delay wraps at most once a subframe
		int32_t loud = 0;
		for (uint32_t k = 0; k < run; k++)
			loud |= window[k + 8] = delay[pos + k];
		for (uint32_t k = run; k < n; k++)
			loud |= window[k + 8] = delay[k - run];
		for (int t = 0; t < 8; t++)
			loud |= window[t];
		if (!loud) {  // silent, and nothing new sent: the delay stays as it is (zeros)
			bool sent = false;
			for (uint32_t k = 0; k < n && !sent; k++)
				sent = wet[k] != 0;
			if (!sent) {
				g.fxPos[i] = (pos + n) % len;
				continue;
			}
		}
		memcpy(g.fxHist[i], window + n, sizeof g.fxHist[i]);
		// the taps' magnitudes sum to under 1.0 (SMS's: 0.95), so the sums fit 32 bits
		int32_t tapSum = 0;
		for (int t = 0; t < 8; t++)
			tapSum += f->taps[t] < 0 ? -f->taps[t] : f->taps[t];
		const bool narrow = tapSum <= 32767;
		for (int pass = 0; pass < 2; pass++) {
			if (f->mode & (1 << pass))
				for (uint32_t k = 0; k < n; k++) {
					const int32_t* w = window + k;
					const int16_t* tp = f->taps;
					if (narrow) {
						const int32_t sum = w[0] * tp[0] + w[1] * tp[1] + w[2] * tp[2] + w[3] * tp[3] + w[4] * tp[4] +
						                    w[5] * tp[5] + w[6] * tp[6] + w[7] * tp[7];
						window[k] = sat16(sum >> 15);
					} else {
						int64_t sum = 0;
						for (int t = 0; t < 8; t++)
							sum += (int64_t)w[t] * tp[t];
						window[k] = sat16((int32_t)(sum >> 15));
					}
				}
			if (pass == 0) {
				const uint16_t buses[2] = {f->busA, f->busB};
				const int16_t gains[2] = {f->gainA, f->gainB};
				for (int r = 0; r < 2; r++) {
					const int side = buses[r] == kBusLeft ? 0 : buses[r] == kBusRight ? 1 : -1;
					if (side < 0 || !gains[r])
						continue;  // returns to other buses (surround) are not played
					const int32_t gq = (int32_t)(gains[r] * g.master);
					for (uint32_t k = 0; k < n; k++)
						out[2 * k + side] += (window[k] * gq) >> 15;
					any = true;
				}
			}
		}
		for (uint32_t k = 0; k < run; k++)
			delay[pos + k] = (int16_t)sat16(window[k] + wet[k]);
		for (uint32_t k = run; k < n; k++)
			delay[k - run] = (int16_t)sat16(window[k] + wet[k]);
		g.fxPos[i] = (pos + n) % len;
		memset(wet, 0, n * sizeof(int32_t));
	}
	return any;
}

// The software mix's subframe `sub`, with the echo, onto its channel (nothing when silent).
void submitMix(uint32_t sub)
{
	const uint32_t slot = sub % kMixSubs, sn = g.subN;
	int32_t echo[2 * kSubMax] = {};
	if (fxSub(slot, sn, echo)) {
		for (uint32_t k = 0; k < 2 * sn; k++)
			g.mixAcc[slot][k] += echo[k];
		g.mixUsed[slot] = true;
#if SMS3DS_DEBUG
		bool heard = false;
		for (uint32_t k = 0; k < 2 * sn; k++) {  // the meter hears it too (meterSub runs after this)
			g.meterAcc[slot][k] += echo[k];
			heard = heard || echo[k];
		}
		g.meterUsed[slot] = true;
		if (heard)
			sms3ds_audio_meter[5] = sms3ds_audio_meter[5] + 1;
#endif
	}
	if (!g.mixUsed[slot])
		return;
	g.mixUsed[slot] = false;
	int32_t* acc = g.mixAcc[slot];
	Chan& ch = g.chan[kMixChan];
	ndspWaveBuf& wb = ch.wb[ch.next];
	if (wb.status == NDSP_WBUF_QUEUED || wb.status == NDSP_WBUF_PLAYING || s_npending == 64) {
		g.overruns++;
		memset(acc, 0, 2 * sn * sizeof(int32_t));
		return;
	}
	// starting (again): the preroll first, so it plays in step with the voices' channels
	const uint32_t pad = ch.queuedAt != g.subNow + 1 && chanIdle(kMixChan) ? (uint32_t)(s_preroll * kOutRate) : 0;
	ch.queuedAt = g.subNow + 1;
	int16_t* dst = ringTake(2 * (pad + sn));
	memset(dst, 0, 2 * pad * sizeof(int16_t));
	for (uint32_t i = 0; i < 2 * sn; i++) {
		const int32_t v = acc[i];
		dst[2 * pad + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		acc[i] = 0;
	}
	memset(&wb, 0, sizeof wb);
	wb.data_pcm16 = dst;
	wb.nsamples = pad + sn;  // stereo frames
	wb.looping = false;
	s_pending[s_npending++] = Pending{kMixChan, ch.gen, &wb, -1.0f, 0.0f, 0.0f};
	ch.next = (ch.next + 1) % kBufs;
}

// Queues the decoded samples of voice `vi` on its channel (taking one when it has volume), their
// volume ramped from subframe to subframe (Decoder).
void submit(int vi, const Voice& v)
{
	Decoder& d = g.dec[vi];
	const uint32_t n = d.staged, subs = d.stagedSubs;
	d.staged = d.stagedSubs = 0;
	if (!n || !subs || !g.ndspOk)
		return;
	float peak = d.prevGain;
	for (uint32_t k = 0; k < subs; k++)
		peak = d.subGain[k] > peak ? d.subGain[k] : peak;
	const float endGain = d.subGain[subs - 1];
	if (peak <= 0.0f) {
		d.prevGain = endGain;
		return;
	}
	// the volume ramp, in place
	{
		const float inv = 1.0f / kGainScale;
		uint32_t at = 0;
		float g0 = d.prevGain;
		for (uint32_t k = 0; k < subs; k++) {
			const uint32_t end = d.subEnd[k] < n ? d.subEnd[k] : n;
			const float g1 = d.subGain[k];
			if (end > at) {
				const float step = (g1 - g0) * inv / float(end - at);
				float f = g0 * inv;
				for (; at < end; at++) {
					f += step;
					d.stage[at] = (int16_t)sat16((int32_t)((float)d.stage[at] * f));
				}
			}
			g0 = g1;
		}
	}
	d.prevGain = endGain;
	if (d.chan < 0) {
		d.chan = s_allSoft ? -1 : takeChan(vi, peak);
		d.contSub = 0;  // its audio so far was elsewhere
		if (d.chan < 0) {
			g.unassigned++;
			softMix(d, n, subs);
#if SMS3DS_DEBUG
			meterAdd(d, n, subs);
#endif
			return;
		}
	}
	Chan& ch = g.chan[d.chan];
	ndspWaveBuf& wb = ch.wb[ch.next];
	if (wb.status == NDSP_WBUF_QUEUED || wb.status == NDSP_WBUF_PLAYING || s_npending == 64) {
		g.overruns++;  // the DSP is that far behind: drop rather than wait
		return;
	}
	const uint32_t first = d.lastSub + 1 - subs;
	const bool following = d.contSub == first + 1;  // this voice's audio carries straight on
	d.contSub = d.lastSub + 2;
#if SMS3DS_DEBUG
	meterAdd(d, n, subs);
	if (following && ch.queuedAt != g.subNow + 1 && chanIdle(d.chan))
		sms3ds_audio_meter[4] = sms3ds_audio_meter[4] + 1;
#else
	(void)following;
#endif
	ch.queuedAt = g.subNow + 1;
	accumulate(nullptr, nullptr, true, d, n, subs);  // its echo sends: the software mix plays the echo
	const float rate = is_oscillator(v.sourceType) ? kOutRate : kOutRate * (float)d.bufRatio / 4096.0f;
	const uint32_t pad = d.queuedAny ? 0 : (uint32_t)(s_preroll * rate);
	d.queuedAny = true;
	int16_t* dst = ringTake(pad + n);
	memset(dst, 0, pad * sizeof(int16_t));
	memcpy(dst + pad, d.stage, n * sizeof(int16_t));
	memset(&wb, 0, sizeof wb);
	wb.data_pcm16 = dst;
	wb.nsamples = pad + n;
	wb.looping = false;
	// the rate and mix go with the buffer (onDspFrame sets them as it starts playing)
	s_pending[s_npending++] = Pending{d.chan, ch.gen, &wb, rate, kGainScale * d.pan, kGainScale * (1.0f - d.pan)};
	ch.next = (ch.next + 1) % kBufs;
	sms3ds_audio_stats[2] = sms3ds_audio_stats[2] + 1;
}

// --- one subframe of one voice ---------------------------------------------------------------------
void render_voice(int vi, int n, float master)
{
	Voice& v = g.voices[vi];
	Decoder& d = g.dec[vi];
	if (v.resetVpb) {  // a new note: whatever this voice still has queued stops
		v.resetVpb = 0;
		releaseChan(d, true);
		d.active = false;
	}
	const bool stopping = v.endRequested != 0;
	if (!supported(v.sourceType)) {
		if (stopping)
			v.done = 1;
		return;
	}
	if (!d.active) {
		const int chan = d.chan;
		memset(&d, 0, offsetof(Decoder, stage));
		d.chan = chan;
		d.staged = d.stagedSubs = 0;
		d.active = true;
		d.remaining = d.written = v.remaining;
	} else if (v.sourceType == kDirectPcm && v.remaining != d.written)
		d.remaining = v.remaining;
	d.written = v.remaining;
	if (v.pause) {
		if (stopping)
			v.done = 1;
		return;
	}
	g.active++;

	// volumes, as dsp_mixer.cpp: the auto mixer (Q15, sine pan law) or the six sends (Q14): the dry
	// pair (buses 1 and 2) and the FX sends (buses 3-6, the echo). Stop requests halve them each
	// subframe until silent.
	float gl = 0.0f, gr = 0.0f, wet[4] = {};
	if (v.amEnabled) {
		const int32_t cur = v.amVolCurrent, tgt = stopping ? cur / 2 : v.amVolTarget;
		if (stopping) {
			v.amVolTarget = (uint16_t)tgt;
			v.done = tgt == 0;
		}
		const int pan = (v.amPan >> 8) & 127;
		const float vol = (float)tgt / 32768.0f;
		gl = vol * s_panCos[pan];
		gr = vol * s_panSin[pan];
		// the reverb send, Q16 (PROTOCOL.md): the front pair to buses 5/6, FX lines 2/3
		const float fx = (float)v.amFx / 65536.0f;
		wet[2] = gl * fx;
		wet[3] = gr * fx;
		v.amVolCurrent = (uint16_t)tgt;
	} else {
		bool released = true;
		for (int s = 0; s < 6; s++) {
			MixSlot& m = v.slot[s];
			const int32_t tgt = stopping ? (int16_t)m.current / 2 : (int16_t)m.target;
			if (stopping) {
				m.target = (uint16_t)tgt;
				released = released && tgt == 0;
			}
			if (m.bus == kBusLeft)
				gl += (float)tgt / 16384.0f;
			else if (m.bus == kBusRight)
				gr += (float)tgt / 16384.0f;
			else
				for (int j = 0; j < 4; j++)
					if (m.bus == kBusFx[j])
						wet[j] += (float)tgt / 16384.0f;
			m.current = (uint16_t)tgt;
		}
		if (stopping)
			v.done = released;
	}
	d.gainL = gl > 0.0f ? gl * master : 0.0f;
	d.gainR = gr > 0.0f ? gr * master : 0.0f;
	{
		// the sends as shares of the total (dry, master-scaled) gain, which the staged samples carry
		const float total = d.gainL + d.gainR;
		d.hasWet = false;
		for (int j = 0; j < 4; j++) {
			d.wet[j] = total > 0.0f && wet[j] > 0.0f ? wet[j] / total : 0.0f;
			d.hasWet = d.hasWet || d.wet[j] > 0.0f;
		}
	}
	// a buffer plays at one rate: a pitch change of more than ~0.1% starts the next one
	if (!is_oscillator(v.sourceType) && d.stagedSubs) {
		const uint32_t a = v.ratio, b = d.bufRatio, diff = a > b ? a - b : b - a;
		if (diff * 1024 > b)
			submit(vi, v);
	}
	if (!d.stagedSubs)
		d.bufRatio = v.ratio;

	// the source samples this subframe consumes (the DSP steps ratio << 4 in 16.16 per output sample),
	// or n synthesised ones for an oscillator (not resampled)
	if (is_oscillator(v.sourceType)) {
		const uint32_t step = (uint32_t)v.ratio << 5, mask = (1u << 22) - 1;
		const int pi = pattern_index(v.sourceType);
		uint32_t ph = d.phase;
		for (int k = 0; k < n && d.staged < kBufSamples; k++) {
			int32_t s;
			switch (v.sourceType) {
			case kOscSquare: s = (ph >> 21) & 1 ? -0x4000 : 0x4000; break;
			case kOscSquare25: s = (ph >> 20) & 3 ? -0x4000 : 0x4000; break;
			case kOscSaw: s = (int16_t)(ph >> 6); break;
			default: s = g.pattern[pi][ph >> 16]; break;
			}
			d.stage[d.staged++] = (int16_t)s;
			ph = (ph + step) & mask;
		}
		d.phase = ph;
	} else {
		const uint32_t total = d.frac + ((uint32_t)v.ratio << 4) * (uint32_t)n;
		uint32_t count = total >> 16;
		d.frac = total & 0xFFFF;
		if (v.sourceType == 9 || v.sourceType == 5) {
			decode_adpcm_run(d, v, count);
			count = 0;
		}
		for (; count && !d.ended; count--) {
			const int32_t s = pull(d, v);
			if (d.ended)
				break;
			if (d.staged < kBufSamples)
				d.stage[d.staged++] = (int16_t)s;
		}
	}
	const float total = d.gainL + d.gainR;
	if (total > 0.0f)
		d.pan = d.gainL / total;
	d.subEnd[d.stagedSubs] = d.staged;
	d.subGain[d.stagedSubs] = total;
	d.lastSub = g.subNow;
	d.stagedSubs++;

	if (v.sourceType == kDirectPcm) {  // what the CPU's stream decoder paces itself by
		const uint32_t ring = v.endPosition >> 16;
		const uint32_t at = ring ? d.idx % ring : 0;
		v.position = (int32_t)(at << 16);
		v.samplesBeforeLoop = (ring - at) << 16;
		if (v.remaining != 0xFFFFFFFFu)
			v.remaining = d.ended ? 0 : d.remaining;
		d.written = v.remaining;
	} else {
		v.position = (int32_t)d.idx;
	}
	v.posFrac = (int16_t)(is_oscillator(v.sourceType) ? d.phase >> 6 : d.frac);
	if (d.ended) {
		v.endReached = 1;
		v.done = 1;
	}

	const bool silent = d.gainL == 0.0f && d.gainR == 0.0f;
	d.quietSubs = silent ? d.quietSubs + 1 : 0;
	if (v.done) {  // the queued tail still plays (a finished one-shot), unless it was faded out
		submit(vi, v);
		releaseChan(d, silent);
		d.active = false;
	} else if (d.stagedSubs >= kSubPerBuf || d.staged + 8 * (uint32_t)n > kBufSamples) {
		submit(vi, v);
		if (d.quietSubs > 4 * kSubPerBuf)
			releaseChan(d, true);  // silent for a while: the channel is better used elsewhere
	}
}

}  // namespace

extern "C" bool sms3ds_ndsp_init(void)
{
	if (R_FAILED(ndspInit()))
		return false;
	LightLock_Init(&s_paramLock);
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetOutputCount(1);
	ndspSetMasterVol(1.0f);
	s_ring = (int16_t*)linearAlloc(kRingSamples * sizeof(int16_t));
	if (!s_ring) {
		ndspExit();
		return false;
	}
	for (int c = 0; c < kChannels; c++)
		chanReset(c);
	// the software mix: stereo, at the DSP's own rate, unscaled
	ndspChnSetInterp(kMixChan, NDSP_INTERP_NONE);
	ndspChnSetFormat(kMixChan, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetRate(kMixChan, kOutRate);
	{
		float mix[12] = {};
		mix[0] = mix[1] = 1.0f;
		ndspChnSetMix(kMixChan, mix);
	}
	for (int i = 0; i < 64; i++)
		g.dec[i].chan = -1;
	ndspSetCallback(onDspFrame, nullptr);
	g.ndspOk = true;
	return true;
}

extern "C" void port_dspmix_setup(uint32_t nvoices, void* voices, const uint32_t* res_filter,
                                  const uint32_t* adpcm_filter, void* fx_lines)
{
	const char* fxEnv = getenv("SMS_AUDIO_FX");  // 0: no echo (as sms-pc-port)
	g.fx = fxEnv && strcmp(fxEnv, "0") == 0 ? nullptr : (FxLine*)fx_lines;
	memset(g.fxBuffer, 0, sizeof g.fxBuffer);
	memset(g.fxHist, 0, sizeof g.fxHist);
	memset(g.wetAcc, 0, sizeof g.wetAcc);
	g.voices = (Voice*)voices;
	g.nvoices = nvoices > 64 ? 64 : nvoices;
	for (int i = 0; i < 128; i++) {
		s_panCos[i] = cosf((float)i / 127.0f * (float)M_PI / 2);
		s_panSin[i] = sinf((float)i / 127.0f * (float)M_PI / 2);
	}
	memset(g.pattern, 0, sizeof g.pattern);
	if (res_filter)
		for (int i = 0; i < 128; i++) {
			int16_t* p = &g.pattern[0][0] + i * 2;
			p[0] = (int16_t)(res_filter[128 + i] >> 16);
			p[1] = (int16_t)(res_filter[128 + i] & 0xFFFF);
		}
	const uint32_t* t = adpcm_filter ? adpcm_filter : kAdpcmDefault;
	for (int i = 0; i < 16; i++) {
		g.coef[i][0] = (int16_t)(t[i] >> 16);
		g.coef[i][1] = (int16_t)(t[i] & 0xFFFF);
	}
	static bool first = true;
	for (int i = 0; i < 64; i++) {
		if (first)
			g.dec[i].chan = -1;
		releaseChan(g.dec[i], true);
		g.dec[i].active = false;
	}
	first = false;
	const char* e = getenv("SMS_AUDIO_TRACE");
	g.trace = e && *e && strcmp(e, "0") != 0;
	if ((e = getenv("SMS_AUDIO_SOFTMIX")) && *e)
		s_allSoft = strcmp(e, "0") != 0;
#if SMS3DS_DEBUG
	if ((e = getenv("SMS_AUDIO_PREROLL_MS")) && *e)
		s_preroll = (float)atoi(e) / 1000.0f;
	// SMS_AUDIO_DUMP=path: the meter's mix as a WAV (float, before saturation), for scripts/audio-levels.py
	const char* dump = getenv("SMS_AUDIO_DUMP");
	if (dump && *dump && !g.dump) {
		g.dump = fopen(dump, "wb");
		if (g.dump) {
			static char buf[64 * 1024];
			setvbuf(g.dump, buf, _IOFBF, sizeof buf);
			dumpHeader();
		}
		fprintf(stderr, "[audio] dumping the mix to %s%s\n", dump, g.dump ? "" : ": cannot open it");
	}
#endif
	if (s_allSoft)
		fprintf(stderr, "[audio] voices mixed on the CPU, played by the 3DS DSP (ndsp)%s\n",
		        g.ndspOk ? "" : ": ndsp is not running, silent");
	else
		fprintf(stderr, "[audio] voices played by the 3DS DSP (ndsp, %d channels and a software mix)%s\n",
		        kVoiceChans, g.ndspOk ? "" : ": ndsp is not running, silent");
}

extern "C" void port_dspmix_set_aram(port_dspmix_aram_fn fn) { g.aram = fn; }
extern "C" void port_dspmix_set_mram(port_dspmix_mram_fn fn) { g.mram = fn; }

extern "C" void port_dspmix_render(int16_t* outA, int16_t* outB, int n, uint16_t master)
{
	const u64 t0 = svcGetSystemTick();
	memset(outA, 0, n * sizeof(int16_t));  // the DSP plays the voices; the AI DMA carries silence
	memset(outB, 0, n * sizeof(int16_t));
	g.active = 0;
	if (n > 0 && n <= kSubMax)
		g.subN = (uint32_t)n;
	const float m = (float)master / 32768.0f;  // Q15 (PROTOCOL.md, Levels)
	g.master = m;
	if (g.voices)
		for (uint32_t i = 0; i < g.nvoices; i++) {
			Voice& v = g.voices[i];
			if (!v.enabled || v.done) {
				Decoder& d = g.dec[i];
				if (d.active || d.chan >= 0) {
					submit((int)i, v);
					releaseChan(d, !v.enabled);
					d.active = false;
				}
				continue;
			}
			render_voice((int)i, n, m);
		}
	// the software mix's subframes no voice buffer can reach any more
	while (g.ndspOk && g.mixNext + kSubPerBuf <= g.subNow) {
		submitMix(g.mixNext);
#if SMS3DS_DEBUG
		meterSub(g.mixNext);
#endif
		g.mixNext++;
	}
	g.subNow++;
	if (g.ndspOk)
		addPending();
	{
		int used = 0;
		for (int c = 0; c < kVoiceChans; c++)
			used += g.chan[c].voice >= 0;
		sms3ds_audio_stats[0] = (uint32_t)g.active;
		sms3ds_audio_stats[1] = (uint32_t)used;
		sms3ds_audio_stats[3] = g.overruns;
		sms3ds_audio_stats[4] = g.unassigned;
		sms3ds_audio_stats[7] = g.softMixed;
		if (g.ndspOk) {
			sms3ds_audio_stats[5] = ndspGetFrameCount();
			sms3ds_audio_stats[6] = ndspGetDroppedFrames();
		}
	}
	{
		static u64 ticks;
		static uint32_t calls;
		ticks += svcGetSystemTick() - t0;
		if (++calls >= 400) {  // about a second of audio
			const uint32_t us = (uint32_t)(ticks / (SYSCLOCK_ARM11 / 1000000));
			sms3ds_audio_cpu_us[0] = us;
			if (us > sms3ds_audio_cpu_us[1])
				sms3ds_audio_cpu_us[1] = us;
			ticks = 0;
			calls = 0;
		}
	}
	if (g.trace && ++g.traceTick % 2000 == 0) {
		int used = 0;
		for (int c = 0; c < kVoiceChans; c++)
			used += g.chan[c].voice >= 0;
		fprintf(stderr, "[audio] %u subframes: %d voices, %d channels, %lu without a channel, %lu overruns\n",
		        g.traceTick, g.active, used, (unsigned long)g.unassigned, (unsigned long)g.overruns);
	}
}

extern "C" int port_dspmix_active_voices(void) { return g.active; }

extern "C" void port_dspmix_decode_wave(int format, uint32_t base, int count, int16_t* out)
{
	static const uint16_t types[4] = {9, 5, 8, 16};
	Voice v;
	memset(&v, 0, sizeof v);
	v.sourceType = types[format & 3];
	v.base = base;
	v.endPosition = (uint32_t)count;
	Decoder* d = (Decoder*)calloc(1, sizeof(Decoder));
	if (!d)
		return;
	for (int i = 0; i < count; i++)
		out[i] = (int16_t)pull(*d, v);
	free(d);
}
