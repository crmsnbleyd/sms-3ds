// 3DS entry point: sets up the screens, logging and defaults, then runs the PC port's platform
// initialisation and the game's main() on this thread (it becomes the game's default OSThread).
#include "sms3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "port_platform.h"

extern void SMS_main(void);

// From the PC port's platform layer (port_os.h, gx_pc.h), declared here so this file need not mix
// <3ds.h>'s integer typedefs with the game's.
extern unsigned char* port_mem1_base;
extern unsigned long port_mem1_size;
extern "C" void GXPC_SetMemoryWindow(void* base, uint32_t size);

// The boot thread runs the game; the PC build gives it 8 MiB on 64-bit hosts. The game's own
// threads are host threads with their own stacks (os_thread.cpp).
extern "C" {
u32 __stacksize__ = 1024 * 1024;
// Linear (GPU-readable) memory: 40 MiB instead of libctru's 32, for the renderer's display-list
// arena (gx3ds/render.cpp) beside textures and copies; the regular heap gets the rest.
u32 __ctru_linear_heap_size = 40u << 20;
}

static void setDefault(const char* name, const char* value)
{
	setenv(name, value, 0);
}

extern "C" bool sms3ds_ndsp_init(void);  // dsp_ndsp.cpp

int main(int argc, char** argv)
{
	gfxInitDefault();
	sms3ds_hud_init();
	sms3ds_log_init();
	mkdir(SMS3DS_DIR, 0777);

	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	if (n3ds)
		osSetSpeedupEnable(true);
	fprintf(stderr, "[3ds] %s 3DS, app memory %lu KiB (free %lu KiB), heap %lu KiB, linear %lu KiB\n",
	        n3ds ? "New" : "Old", (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10),
	        (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10),
	        (unsigned long)(envGetHeapSize() >> 10), (unsigned long)(envGetLinearHeapSize() >> 10));
	u32 fpscr;
	__asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
	fprintf(stderr, "[3ds] FPSCR %08lx (flush-to-zero %s, default NaN %s)\n", (unsigned long)fpscr,
	        (fpscr >> 24) & 1 ? "on" : "off", (fpscr >> 25) & 1 ? "on" : "off");
	sms3ds_hud_start();
	{
		// Effective CPU clock: 30M iterations of a 3-instruction loop (subs, bne + nop: ~3 cycles
		// each on the ARM11) against the 268 MHz system tick. ~804 means the New 3DS clock is in
		// effect; ~268 means the emulator (or the system) runs the old clock.
		const u64 t0 = svcGetSystemTick();
		u32 n = 30000000;
		__asm__ volatile("1: nop\n subs %0, %0, #1\n bne 1b" : "+r"(n) : : "cc");
		const u64 dt = svcGetSystemTick() - t0;
		fprintf(stderr, "[3ds] CPU: about %lu MHz (3-cycle loop timed against the 268 MHz tick)\n",
		        (unsigned long)(90000000ull * (u64)SYSCLOCK_ARM11 / (dt ? dt : 1) / 1000000ull));
	}

	// Sound: the game's audio runs and the 3DS DSP plays its voices (dsp_ndsp.cpp); ai.cpp only paces
	// JAudio's frames (SMS_AUDIO=0: no output device of its own). ndsp needs the DSP firmware dumped
	// from the console (sdmc:/3ds/dspfirm.cdc, e.g. with DSP1); without it the game runs silent
	// (SMS_NO_AUDIO=1, which also skips the game's audio work).
	const bool sound = sms3ds_ndsp_init();
	fprintf(stderr, "[3ds] sound: %s\n", sound ? "ndsp" : "none (no sdmc:/3ds/dspfirm.cdc?): silent");
	setDefault("SMS_NO_AUDIO", sound ? "0" : "1");
	setDefault("SMS_AUDIO", "0");
	setDefault("SMS_SKIP_MOVIES", "0");
	// The logos and the opening movie play (SMS_SKIP_INTRO is left unset, so settings.txt can set it);
	// A, or a tap on the bottom screen's Skip button, skips them.
	// The game's own 30 fps logic (sms-pc-port defaults to 60, twice the work per second).
	setDefault("SMS_FRAME_RATE", "30");
	setDefault("SMS_SAVE_DIR", SMS3DS_DIR "/card-a");
	setDefault("SMS_SETTINGS", SMS3DS_DIR "/settings.txt");

	// The disc image: game.ciso, else game.iso (the reader tells CISO from ISO by its header).
	struct stat st;
	setDefault("SMS_DISC_ROOT", stat(SMS3DS_DISC, &st) != 0 && stat(SMS3DS_DISC_ISO, &st) == 0 ? SMS3DS_DISC_ISO : SMS3DS_DISC);
	if (stat(getenv("SMS_DISC_ROOT"), &st) != 0)
		sms3ds_hud_fatal("No disc image: put your GMSE01 disc at " SMS3DS_DISC " or " SMS3DS_DISC_ISO);
	fprintf(stderr, "[3ds] disc: %s\n", getenv("SMS_DISC_ROOT"));

	char* args[] = {argc > 0 ? argv[0] : (char*)"sms-3ds", NULL};
	port_init(1, args);
	// MEM1 is a heap block, not at 0x80000000. Game code turns pointers into GX "physical"
	// addresses with the SDK macro OSCachedToPhysical (p - 0x80000000), which the front end masks
	// to 30 bits: every 3DS user address is below 0x40000000, so a window based at 0 makes both
	// directions the identity (gx_fifo's write stamps measure from MEM1: patches/port/3ds-05).
	GXPC_SetMemoryWindow(NULL, 0x40000000u);
	fprintf(stderr, "[3ds] MEM1 %lu KiB at %p\n", port_mem1_size >> 10, (void*)port_mem1_base);
	fprintf(stderr, "[port] entering SMS_main\n");
	SMS_main();
	fprintf(stderr, "[port] SMS_main returned\n");
	sms3ds_hud_fatal("The game returned.");
	return 0;
}
