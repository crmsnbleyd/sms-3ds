// 3DS input, read for platform3ds/pad_3ds.cpp (which cannot include <3ds.h>: the game's types).
#include "sms3ds.h"

#include <3ds.h>

// Input from outside (scripts/azahar.py writes it through Azahar's RPC memory access): buttons are
// added to the 3DS's, a non-zero stick replaces the circle pad / C-stick (3DS units, about +-156).
extern "C" volatile Sms3dsHid sms3ds_remote_pad;
volatile Sms3dsHid sms3ds_remote_pad;

extern "C" void sms3ds_hid_read(Sms3dsHid* out)
{
	// The HUD thread calls hidScanInput() a few times a second; scan here too so the game sees
	// input every frame. Only held state is used, so the two scanners cannot lose a press.
	hidScanInput();
	u32 k = hidKeysHeld();
	circlePosition cp, cs;
	hidCircleRead(&cp);
	hidCstickRead(&cs);  // New 3DS C-stick (zero on the Old 3DS)
	out->keys = 0;
	if (k & KEY_A) out->keys |= SMS3DS_A;
	if (k & KEY_B) out->keys |= SMS3DS_B;
	if (k & KEY_X) out->keys |= SMS3DS_X;
	if (k & KEY_Y) out->keys |= SMS3DS_Y;
	if (k & KEY_L) out->keys |= SMS3DS_L;
	if (k & KEY_R) out->keys |= SMS3DS_R;
	if (k & (KEY_ZL | KEY_ZR)) out->keys |= SMS3DS_Z;
	if (k & KEY_START) out->keys |= SMS3DS_START;
	if (k & KEY_DUP) out->keys |= SMS3DS_UP;
	if (k & KEY_DDOWN) out->keys |= SMS3DS_DOWN;
	if (k & KEY_DLEFT) out->keys |= SMS3DS_LEFT;
	if (k & KEY_DRIGHT) out->keys |= SMS3DS_RIGHT;
	out->stickX = cp.dx;
	out->stickY = cp.dy;
	out->cstickX = cs.dx;
	out->cstickY = cs.dy;
	const Sms3dsHid r = {sms3ds_remote_pad.keys, sms3ds_remote_pad.stickX, sms3ds_remote_pad.stickY,
	                     sms3ds_remote_pad.cstickX, sms3ds_remote_pad.cstickY};
	out->keys |= r.keys;
	if (r.stickX || r.stickY) {
		out->stickX = r.stickX;
		out->stickY = r.stickY;
	}
	if (r.cstickX || r.cstickY) {
		out->cstickX = r.cstickX;
		out->cstickY = r.cstickY;
	}
}

// A new touch on the bottom screen (it was not touched at the last call), in its pixels (0-319,
// 0-239): the movie skip button (patches/decomp/3ds-13). sms3ds_remote_touch (x, y, down) is a touch
// written from outside (scripts/azahar.py touch), as the remote pad is.
extern "C" volatile int32_t sms3ds_remote_touch[3];
volatile int32_t sms3ds_remote_touch[3];

extern "C" int sms3ds_touch_tap(int* x, int* y)
{
	static bool was;
	touchPosition t = {};
	bool down = (hidKeysHeld() & KEY_TOUCH) != 0;
	if (down)
		hidTouchRead(&t);
	if (sms3ds_remote_touch[2]) {
		down = true;
		t.px = (u16)sms3ds_remote_touch[0];
		t.py = (u16)sms3ds_remote_touch[1];
	}
	const bool tap = down && !was;
	was = down;
	if (tap) {
		*x = t.px;
		*y = t.py;
	}
	return tap;
}
