// Status display while there is no renderer: the top screen shows the newest log lines, the bottom
// screen live counters. Runs on its own thread on the system core (core 1), so it keeps updating
// while the game's threads hold the application core.
#include "sms3ds.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

volatile uint32_t sms3ds_gx_frames;
volatile uint32_t sms3ds_gx_copies;
volatile uint32_t sms3ds_gx_primitives;
volatile uint32_t sms3ds_gx_vertices;

namespace {

PrintConsole s_top, s_bottom;
const devoptab_t* s_con;  // the console's stdout devoptab, saved before stdout goes to the log
Thread s_thread;
u64 s_start;

void put(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 1)
		n = sizeof buf - 1;
	if (n > 0 && s_con)
		s_con->write_r(_REENT, NULL, buf, (size_t)n);
}

void drawTop()
{
	consoleSelect(&s_top);
	put("\x1b[1;1H\x1b[7m sms-3ds  phase 1 (no picture yet)          \x1b[0m");
	static char tail[28][64];
	int n = sms3ds_log_tail(tail, 28);
	for (int i = 0; i < 28; i++) {
		// 50 columns on the top screen; clear the rest of each row
		put("\x1b[%d;1H%-50.50s", i + 2, i < n ? tail[i] : "");
	}
}

void drawBottom()
{
	consoleSelect(&s_bottom);
	u64 ms = osGetTime() - s_start;
	put("\x1b[1;1H\x1b[7m status                                  \x1b[0m");
	put("\x1b[3;1Huptime     %6llu.%01llu s", ms / 1000, (ms / 100) % 10);
	put("\x1b[4;1Hframes     %10lu", (unsigned long)sms3ds_gx_frames);
	put("\x1b[5;1Hcopies     %10lu", (unsigned long)sms3ds_gx_copies);
	put("\x1b[6;1Hprimitives %10lu", (unsigned long)sms3ds_gx_primitives);
	put("\x1b[7;1Hvertices   %10lu", (unsigned long)sms3ds_gx_vertices);
	put("\x1b[8;1Hlog lines  %10lu", (unsigned long)sms3ds_log_lines());
	put("\x1b[10;1Happ mem free %6lu KiB  ", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
	put("\x1b[11;1Hheap %lu KiB, linear %lu KiB", (unsigned long)(envGetHeapSize() >> 10),
	    (unsigned long)(envGetLinearHeapSize() >> 10));
	put("\x1b[28;1HSTART+SELECT: quit");
}

void hudThread(void*)
{
	for (;;) {
		hidScanInput();
		u32 held = hidKeysHeld();
		if (!aptMainLoop() || ((held & KEY_START) && (held & KEY_SELECT))) {
			fflush(stderr);
			svcExitProcess();
		}
		drawTop();
		drawBottom();
		gfxFlushBuffers();
		gfxSwapBuffers();
		svcSleepThread(250 * 1000 * 1000LL);
	}
}

}  // namespace

extern "C" void sms3ds_hud_init(void)
{
	s_start = osGetTime();
	consoleInit(GFX_TOP, &s_top);
	consoleInit(GFX_BOTTOM, &s_bottom);
	s_con = devoptab_list[STD_OUT];
}

extern "C" void sms3ds_hud_start(void)
{
	// The system core gives applications a share of its time only when asked.
	APT_SetAppCpuTimeLimit(30);
	s_thread = threadCreate(hudThread, NULL, 32 * 1024, 0x18, 1, true);
	if (!s_thread)  // no system core time: run on the application core instead
		s_thread = threadCreate(hudThread, NULL, 32 * 1024, 0x18, 0, true);
}

extern "C" void sms3ds_hud_fatal(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	consoleSelect(&s_bottom);
	put("\x1b[14;1H\x1b[31m%s\x1b[0m", msg);
	while (aptMainLoop()) {
		drawTop();
		gfxFlushBuffers();
		gfxSwapBuffers();
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}
	gfxExit();
	svcExitProcess();
}
