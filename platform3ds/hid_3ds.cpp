// 3DS input, read for platform3ds/pad_3ds.cpp (which cannot include <3ds.h>: the game's types).
#include "sms3ds.h"

#include <3ds.h>

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
	if ((k & (KEY_L | KEY_R | KEY_X)) == (KEY_L | KEY_R | KEY_X) && !sms3ds_gx_dump_request)
		sms3ds_gx_dump_request = 1;  // frame dump (gx3ds/render.cpp)
	out->stickX = cp.dx;
	out->stickY = cp.dy;
	out->cstickX = cs.dx;
	out->cstickY = cs.dy;
}
