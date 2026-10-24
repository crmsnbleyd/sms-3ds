// Controller 1 from the 3DS buttons and sticks (a minimal PADRead; the full mapping, options and
// rumble come in Phase 3). Built with the game's types, so <3ds.h> stays in hid_3ds.cpp.
//   A B X Y START, D-pad -> the same; L / R -> full analog L / R (FLUDD); ZL / ZR -> Z;
//   circle pad -> main stick; C-stick (New 3DS) -> C-stick.
#include <dolphin/pad.h>
#include <string.h>

#include "sms3ds.h"

extern "C" __attribute__((weak)) void port_ext_pad_read(void* status);  // ext/ hooks (port_ext.h)

volatile uint32_t sms3ds_pad_reads;
volatile uint16_t sms3ds_pad_buttons;

// The SDK's controller set-up calls succeed (the weak stubs would return 0, and the game's pad
// layer treats a failed reset as no controller). As sms-pc-port's platform/pad/pad.cpp.
extern "C" BOOL PADInit() { return TRUE; }
extern "C" BOOL PADReset(u32) { return TRUE; }
extern "C" BOOL PADRecalibrate(u32) { return TRUE; }
extern "C" BOOL PADSync(void) { return TRUE; }
extern "C" void PADSetSpec(u32) {}
extern "C" void PADSetAnalogMode(u32) {}
extern "C" void PADSetSamplingRate(u32) {}
extern "C" void PADControlMotor(s32, u32) {}
extern "C" void PADControlAllMotors(const u32*) {}

namespace {

// The circle pad reports about +-156 at its rim, the C-stick about +-146; GameCube sticks report
// raw values up to about +-110 that PADClamp then limits. Scale the rim to 110.
s8 stick(int v, int rim)
{
	int s = v * 110 / rim;
	if (s > 127) s = 127;
	if (s < -128) s = -128;
	return (s8)s;
}

}  // namespace

extern "C" u32 PADRead(PADStatus* status)
{
	for (int i = 0; i < 4; i++) {
		memset(&status[i], 0, sizeof status[i]);
		status[i].err = i == 0 ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
	}
	Sms3dsHid h;
	sms3ds_hid_read(&h);
	PADStatus& s = status[0];
	u16 b = 0;
	if (h.keys & SMS3DS_A) b |= PAD_BUTTON_A;
	if (h.keys & SMS3DS_B) b |= PAD_BUTTON_B;
	if (h.keys & SMS3DS_X) b |= PAD_BUTTON_X;
	if (h.keys & SMS3DS_Y) b |= PAD_BUTTON_Y;
	if (h.keys & SMS3DS_Z) b |= PAD_TRIGGER_Z;
	if (h.keys & SMS3DS_START) b |= PAD_BUTTON_START;
	if (h.keys & SMS3DS_UP) b |= PAD_BUTTON_UP;
	if (h.keys & SMS3DS_DOWN) b |= PAD_BUTTON_DOWN;
	if (h.keys & SMS3DS_LEFT) b |= PAD_BUTTON_LEFT;
	if (h.keys & SMS3DS_RIGHT) b |= PAD_BUTTON_RIGHT;
	if (h.keys & SMS3DS_L) {
		b |= PAD_TRIGGER_L;
		s.triggerLeft = 255;
	}
	if (h.keys & SMS3DS_R) {
		b |= PAD_TRIGGER_R;
		s.triggerRight = 255;
	} else if (h.keys & SMS3DS_RSOFT) {
		// ZR: R part of the way, analog only, as sms-pc-port's R_SOFT (40%): FLUDD sprays while
		// Mario walks, where the full press (the click) stands him still
		s.triggerRight = 102;
	}
	s.button = b;
	sms3ds_pad_reads = sms3ds_pad_reads + 1;
	sms3ds_pad_buttons = b;
	s.analogA = (b & PAD_BUTTON_A) ? 255 : 0;
	s.analogB = (b & PAD_BUTTON_B) ? 255 : 0;
	s.stickX = stick(h.stickX, 156);
	s.stickY = stick(h.stickY, 156);
	s.substickX = stick(h.cstickX, 146);
	s.substickY = stick(h.cstickY, 146);
	if (port_ext_pad_read)
		port_ext_pad_read(status);
	return PAD_CHAN0_BIT;
}
