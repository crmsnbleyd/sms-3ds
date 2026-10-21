// Status display on the bottom screen (the top screen belongs to the renderer): live counters and
// the newest log lines. Runs on its own thread on the system core (core 1), so it keeps updating
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

PrintConsole s_bottom;
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

void drawLog()
{
	static char tail[10][64];
	int n = sms3ds_log_tail(tail, 10);
	for (int i = 0; i < 10; i++)  // 40 columns on the bottom screen; clear the rest of each row
		put("\x1b[%d;1H%-40.40s", i + 18, i < n ? tail[i] : "");
}

void drawBottom()
{
	consoleSelect(&s_bottom);
	u64 ms = osGetTime() - s_start;
	put("\x1b[1;1H\x1b[7m sms-3ds  phase 2                        \x1b[0m");
	put("\x1b[3;1Huptime     %6llu.%01llu s", ms / 1000, (ms / 100) % 10);
	put("\x1b[4;1Hframes     %10lu", (unsigned long)sms3ds_gx_frames);
	put("\x1b[5;1Hcopies     %10lu", (unsigned long)sms3ds_gx_copies);
	put("\x1b[6;1Hprimitives %10lu", (unsigned long)sms3ds_gx_primitives);
	put("\x1b[7;1Hvertices   %10lu", (unsigned long)sms3ds_gx_vertices);
	put("\x1b[8;1Hper frame  %5lu batches %7lu verts  ", (unsigned long)sms3ds_gx_batches_per_frame,
	    (unsigned long)sms3ds_gx_vertices_per_frame);
	put("\x1b[9;1Hcpu %5.1f ms  gpu %5.1f ms  drop %lu  ", (double)sms3ds_gx_cpu_ms, (double)sms3ds_gx_gpu_ms,
	    (unsigned long)sms3ds_gx_dropped);
	put("\x1b[10;1Happrox %9lu  why %05lx    ", (unsigned long)sms3ds_gx_tev_approx,
	    (unsigned long)sms3ds_gx_approx_why);
	{
		const float fr = sms3ds_prof_frame_ms, fl = sms3ds_prof_flush_ms, dc = sms3ds_prof_decode_ms,
		            wt = sms3ds_prof_wait_ms;
		float game = fr - fl - dc - wt;
		if (game < 0.0f)
			game = 0.0f;
		// ms per frame: render = backend flushes (xform = CPU transform, tex = texture work)
		put("\x1b[13;1Hframe %4.0f render %4.0f xform %4.0f  ", (double)fr, (double)fl, (double)sms3ds_prof_xform_ms);
		put("\x1b[14;1Htex %3.0f dec %4.0f wait %3.0f game %4.0f  ", (double)sms3ds_prof_tex_ms, (double)dc,
		    (double)wt, (double)game);
	}
	put("\x1b[11;1Happ mem free %6lu KiB  ", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
	put("\x1b[12;1Hheap %lu KiB, linear %lu KiB", (unsigned long)(envGetHeapSize() >> 10),
	    (unsigned long)(envGetLinearHeapSize() >> 10));
	put("\x1b[15;1Hstate %d area %d ep %d file %d  pad %04x   ", sms3ds_state, sms3ds_area, sms3ds_episode,
	    sms3ds_save_file, (unsigned)sms3ds_pad_buttons);
	put("\x1b[17;1H\x1b[7m log  (START+SELECT quit, L+R+X dump)    \x1b[0m");
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
		if ((held & KEY_L) && (held & KEY_R) && (held & KEY_X) && !sms3ds_gx_dump_request)
			sms3ds_gx_dump_request = 1;
		drawBottom();
		drawLog();
		gfxFlushBuffers();
		gfxScreenSwapBuffers(GFX_BOTTOM, false);
		svcSleepThread(250 * 1000 * 1000LL);
	}
}

}  // namespace

extern "C" void sms3ds_hud_init(void)
{
	s_start = osGetTime();
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
	put("\x1b[14;1H\x1b[31m%.40s\x1b[0m", msg);
	while (aptMainLoop()) {
		drawLog();
		gfxFlushBuffers();
		gfxScreenSwapBuffers(GFX_BOTTOM, false);
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}
	gfxExit();
	svcExitProcess();
}
