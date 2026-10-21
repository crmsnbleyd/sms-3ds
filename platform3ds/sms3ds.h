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

// Counters the null GX backend advances (read by the HUD).
extern volatile uint32_t sms3ds_gx_frames;      // display copies (GXCopyDisp)
extern volatile uint32_t sms3ds_gx_copies;      // texture copies
extern volatile uint32_t sms3ds_gx_primitives;  // primitives decoded
extern volatile uint32_t sms3ds_gx_vertices;    // vertices decoded

#ifdef __cplusplus
}
#endif

#endif
