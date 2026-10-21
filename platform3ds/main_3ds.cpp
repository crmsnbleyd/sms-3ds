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
}

static void setDefault(const char* name, const char* value)
{
	setenv(name, value, 0);
}

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

	// Phase 1: no picture, sound or input yet; skip what would only wait on them.
	setDefault("SMS_NO_AUDIO", "1");
	setDefault("SMS_SKIP_MOVIES", "1");
	setDefault("SMS_SKIP_INTRO", "1");
	setDefault("SMS_SAVE_DIR", SMS3DS_DIR "/card-a");
	setDefault("SMS_DISC_ROOT", SMS3DS_DISC);
	setDefault("SMS_SETTINGS", SMS3DS_DIR "/settings.txt");

	struct stat st;
	if (stat(getenv("SMS_DISC_ROOT"), &st) != 0)
		sms3ds_hud_fatal("No disc image: put your GMSE01 .ciso/.iso at " SMS3DS_DISC);

	char* args[] = {argc > 0 ? argv[0] : (char*)"sms-3ds", NULL};
	port_init(1, args);
	// MEM1 is a heap block, not at 0x80000000: GX physical addresses are offsets into it.
	GXPC_SetMemoryWindow(port_mem1_base, (uint32_t)port_mem1_size);
	fprintf(stderr, "[3ds] MEM1 %lu KiB at %p\n", port_mem1_size >> 10, (void*)port_mem1_base);
	fprintf(stderr, "[port] entering SMS_main\n");
	SMS_main();
	fprintf(stderr, "[port] SMS_main returned\n");
	sms3ds_hud_fatal("The game returned.");
	return 0;
}
