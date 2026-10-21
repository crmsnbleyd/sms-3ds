// stdout/stderr for the 3DS build: every line goes to the emulator's debug log (svcOutputDebugString:
// Azahar shows it in its log), to SMS3DS_LOG on the SD card, and to a ring the HUD draws.
#include "sms3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

namespace {

const int kRing = 32;
char s_ring[kRing][64];
uint32_t s_lines;          // lines completed so far (ring index = s_lines % kRing)
char s_cur[256];           // the line being assembled
size_t s_curLen;
FILE* s_file;
LightLock s_lock;
bool s_fileTried;

void endLine()
{
	s_cur[s_curLen] = 0;
	svcOutputDebugString(s_cur, (s32)s_curLen);
	if (!s_file && !s_fileTried) {
		s_fileTried = true;
		s_file = fopen(SMS3DS_LOG, "w");
	}
	if (s_file) {
		fwrite(s_cur, 1, s_curLen, s_file);
		fputc('\n', s_file);
		fflush(s_file);
	}
	char* slot = s_ring[s_lines % kRing];
	strncpy(slot, s_cur, 63);
	slot[63] = 0;
	s_lines++;
	s_curLen = 0;
}

ssize_t logWrite(struct _reent*, void*, const char* ptr, size_t len)
{
	LightLock_Lock(&s_lock);
	for (size_t i = 0; i < len; i++) {
		char c = ptr[i];
		if (c == '\n' || s_curLen == sizeof s_cur - 1)
			endLine();
		if (c != '\n' && c != '\r')
			s_cur[s_curLen++] = c;
	}
	LightLock_Unlock(&s_lock);
	return (ssize_t)len;
}

const devoptab_t s_logDevoptab = {
	.name = "smslog",
	.structSize = 0,
	.open_r = NULL,
	.close_r = NULL,
	.write_r = logWrite,
};

}  // namespace

extern "C" void sms3ds_log_init(void)
{
	LightLock_Init(&s_lock);
	devoptab_list[STD_OUT] = &s_logDevoptab;
	devoptab_list[STD_ERR] = &s_logDevoptab;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
}

extern "C" int sms3ds_log_tail(char (*out)[64], int max)
{
	LightLock_Lock(&s_lock);
	uint32_t n = s_lines < (uint32_t)kRing ? s_lines : (uint32_t)kRing;
	if (n > (uint32_t)max)
		n = (uint32_t)max;
	for (uint32_t i = 0; i < n; i++)
		memcpy(out[i], s_ring[(s_lines - n + i) % kRing], 64);
	LightLock_Unlock(&s_lock);
	return (int)n;
}

extern "C" uint32_t sms3ds_log_lines(void)
{
	return s_lines;
}
