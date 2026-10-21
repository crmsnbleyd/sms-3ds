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
extern volatile uint32_t sms3ds_gx_tev_approx;  // gx3ds/tev.cpp: batches drawn with an approximated TEV

#ifdef __cplusplus
}
#endif

#endif
