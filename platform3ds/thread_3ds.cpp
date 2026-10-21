// Thread helpers for the patched PC-port platform code (kept here so those files need not include
// <3ds.h>, whose integer typedefs would meet the game's).
#include <3ds.h>

extern "C" void sms3ds_thread_raise_priority(int steps)
{
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio -= steps;  // lower numbers run first
	if (prio < 0x18)
		prio = 0x18;
	svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
}
