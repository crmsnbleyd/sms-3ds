// Phase 1 stand-ins for PC-port modules not built for 3DS yet: input and camera options (pad.cpp,
// Phase 3), save states (savestate/), HD movies (thp/), presence and the window icon. Values match
// sms-pc-port's defaults (platform/pad/pad.cpp) so the game behaves as with no options set.
#include <dolphin/types.h>
#include <string.h>

#include "port_platform.h"
#include "port_presence.h"
#include "port_savestate.h"
#include "sms_hd_movies.h"

// --- pad.cpp: camera and controller options ---------------------------------------------------------
extern "C" {
int port_free_camera = 0;
int port_camera_invert_y = 0;
int port_close_camera_cstick = 0;
f32 port_camera_speed_x = 1.0f;
f32 port_camera_speed_y = 1.0f;
int port_mouse_orbit = 1;
f32 port_camera_mouse_pitch = 0.0f;
f32 port_camera_zoom = 1.0f;
int port_camera_mouse_busy = 0;
f32 port_rumble_boost = 1.0f;

int port_camera_take_mouse(f32* dx, f32* dy)
{
	*dx = *dy = 0.0f;
	return 0;
}
void port_pad_autopress_field(u32) {}
const void* port_button_glyph(int) { return NULL; }
}

// --- save states ------------------------------------------------------------------------------------
int port_savestate_scan(void) { return 0; }
const PortSaveStateInfo* port_savestate_scanned(int) { return NULL; }
const void* port_savestate_picture(int, int, int*, int*) { return NULL; }
const void* port_savestate_world_art(int, int, int*, int*, int*) { return NULL; }
int port_savestate_take(char*, int) { return 0; }
void port_savestate_set_ready(int) {}
int port_savestate_save(const PortSaveStatePortable*) { return 0; }
int port_savestate_can_load_exact(const char*) { return 0; }
int port_savestate_load_exact(const char*, PortSaveStatePortable*, int* hasPortable)
{
	if (hasPortable)
		*hasPortable = 0;
	return 0;
}
void port_savestate_notify(const char*) {}
void port_savestate_request_load(const char*) {}
int port_savestate_delete(const char*) { return 0; }
void port_savestate_hint(PortSaveStateHint* out)
{
	if (out)
		memset(out, 0, sizeof *out);
}

// --- HD movies, presence, window icon -----------------------------------------------------------------
BOOL port_draw_hd_thp(u8*, u8*, u8*, s16, s16, s16, s16, s16, s16) { return FALSE; }
const char* port_hd_cutscene_directory(void) { return NULL; }
void port_window_icon_init(void) {}
