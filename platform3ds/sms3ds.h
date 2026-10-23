// sms-3ds: shared declarations for the 3DS layer (platform3ds/).
#ifndef SMS3DS_H
#define SMS3DS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where the user's files live on the SD card (never in the repo).
#define SMS3DS_DIR "sdmc:/3ds/sms-3ds"
#define SMS3DS_DISC SMS3DS_DIR "/game.ciso"
#define SMS3DS_DISC_ISO SMS3DS_DIR "/game.iso"  // tried when game.ciso is missing
#define SMS3DS_LOG SMS3DS_DIR "/log.txt"

// log_3ds.cpp: stdout/stderr go to the emulator's debug log (svcOutputDebugString), to SMS3DS_LOG,
// and to a ring of recent lines the HUD shows.
void sms3ds_log_init(void);
// Copies up to `max` most recent lines (oldest first) into `out`, each NUL-terminated and at most
// `width` bytes; returns how many.
int sms3ds_log_tail(char (*out)[64], int max);
uint32_t sms3ds_log_lines(void);

// hud.cpp: a thread on the system core redraws both screens' consoles.
void sms3ds_hud_init(void);
void sms3ds_hud_start(void);
void sms3ds_hud_fatal(const char* msg);  // shows msg and waits for the user to quit

// hid_3ds.cpp: 3DS input in a form pad_3ds.cpp (built with the game's types) can use.
enum {
	SMS3DS_A = 1 << 0, SMS3DS_B = 1 << 1, SMS3DS_X = 1 << 2, SMS3DS_Y = 1 << 3,
	SMS3DS_L = 1 << 4, SMS3DS_R = 1 << 5, SMS3DS_Z = 1 << 6, SMS3DS_START = 1 << 7,
	SMS3DS_UP = 1 << 8, SMS3DS_DOWN = 1 << 9, SMS3DS_LEFT = 1 << 10, SMS3DS_RIGHT = 1 << 11,
};
typedef struct {
	uint32_t keys;  // SMS3DS_* bits
	int16_t stickX, stickY, cstickX, cstickY;  // raw circle pad / C-stick
} Sms3dsHid;
void sms3ds_hid_read(Sms3dsHid* out);

// presence_3ds.cpp: the game's state as of the last frame (-1 before the first).
extern volatile int sms3ds_state, sms3ds_area, sms3ds_episode, sms3ds_save_file;
// pad_3ds.cpp: how often the game read the controller, and the buttons it saw last.
extern volatile uint32_t sms3ds_pad_reads;
extern volatile uint16_t sms3ds_pad_buttons;

// Counters the null GX backend advances (read by the HUD).
extern volatile uint32_t sms3ds_gx_frames;      // display copies (GXCopyDisp)
extern volatile uint32_t sms3ds_gx_copies;      // texture copies
extern volatile uint32_t sms3ds_gx_primitives;  // primitives decoded
extern volatile uint32_t sms3ds_gx_vertices;    // vertices decoded
// gx3ds/render.cpp: the last finished frame
extern volatile uint32_t sms3ds_gx_batches_per_frame, sms3ds_gx_vertices_per_frame, sms3ds_gx_dropped;
extern volatile float sms3ds_gx_cpu_ms, sms3ds_gx_gpu_ms;  // citro3d processing / drawing time
extern volatile uint32_t sms3ds_gx_tev_approx;   // batches drawn with an approximation
extern volatile uint32_t sms3ds_gx_approx_why;   // gx3ds.h AX_* bits seen in the last frame
extern volatile uint32_t sms3ds_gx_dump_request; // set by the HUD (L+R+X): dump the next frame's GX state
// gx3ds/render.cpp: where the last frame's time went (ms): the whole frame (display copy to display
// copy), the backend's batch flushes (of which CPU transform and texture checks/conversion), waiting
// for the GPU at frame start, and the GX front end's vertex decoding. The rest is game logic.
extern volatile float sms3ds_prof_frame_ms, sms3ds_prof_flush_ms, sms3ds_prof_xform_ms, sms3ds_prof_tex_ms,
    sms3ds_prof_wait_ms, sms3ds_prof_decode_ms;
// Phases timed by sms3ds_prof_begin/end (patches 3ds-03-director-profile, 3ds-07-dsp-profile), ms in
// the last frame: actor hit checks, movement, animation (incl. TMarDirector::movement), drawing (the
// game's draw lists, which includes GX decoding and the backend's flushes), audio mixing (DSP HLE).
// A phase includes any other game thread that ran inside it.
enum { SMS3DS_PROF_HIT, SMS3DS_PROF_MOVE, SMS3DS_PROF_ANIM, SMS3DS_PROF_DRAW, SMS3DS_PROF_MIX, SMS3DS_PROF_PHASES };
extern volatile float sms3ds_prof_phase_ms[SMS3DS_PROF_PHASES];
// gx3ds/render.cpp, last frame: cached display-list runs copied into batches (ms, outside flushes);
// of the flushes: frame start (incl. EFB-copy write-backs), pixel state, index conversion, draw calls;
// vertices decoded by the vertex loader vs replayed from the display-list cache.
extern volatile float sms3ds_prof_replay_ms, sms3ds_prof_begin_ms, sms3ds_prof_state_ms, sms3ds_prof_idx_ms,
    sms3ds_prof_draw_ms, sms3ds_prof_copy_ms;  // copy: EFB texture copies (outside the flushes)
extern volatile uint32_t sms3ds_gx_fresh_verts, sms3ds_gx_replay_verts;
// gx3ds/gxthread.cpp, per game frame while the GX thread runs (else 0): the game thread's frame time,
// its waits for the GX thread, and the GX thread's busy time (ms).
extern volatile float sms3ds_prof_cpu_ms, sms3ds_prof_gxwait_ms, sms3ds_prof_gxbusy_ms;
// GX thread, per game frame: display-list calls and their own time (cache checks, replays), stream size
extern volatile float sms3ds_prof_dl_ms;
extern volatile uint32_t sms3ds_gx_dl_calls, sms3ds_gx_stream_kb;
// gx3ds, last frame (read by scripts/azahar.py): pixel-state applications, vertex set-up misses,
// uniform vec4s changed, texture binds, display-list arena KiB uploaded, EFB-copy tiles re-encoded for
// write-backs, the frame start's write-backs and GPU wait (us), TEV compiles (cache misses), vertex
// set-ups that only reloaded XF matrices
enum { SMS3DS_CNT_PIXEL, SMS3DS_CNT_VTXMISS, SMS3DS_CNT_UNIF, SMS3DS_CNT_TEXBIND, SMS3DS_CNT_ARENAKB,
       SMS3DS_CNT_WBTILES, SMS3DS_CNT_WBUS, SMS3DS_CNT_BEGINWAITUS, SMS3DS_CNT_TEVCOMPILE, SMS3DS_CNT_XFONLY,
       SMS3DS_CNT_N };
extern volatile uint32_t sms3ds_gx_count[SMS3DS_CNT_N];
void sms3ds_prof_begin(int phase);
void sms3ds_prof_end(int phase);

#ifdef __cplusplus
}
#endif

#endif
