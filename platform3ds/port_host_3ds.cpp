// Host functions newlib on 3DS lacks (declared by the patched src/port_host.h).
#include <3ds.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
LightLock s_preadLock = 1;  // LightLock_Init state: unlocked
}

// pread: seek and read under one lock, so concurrent readers of the disc image (the game's DVD
// reads run on several OSThreads) cannot interleave between the seek and the read.
extern "C++" ssize_t port_pread(int fd, void* buffer, size_t count, unsigned long long offset)
{
	LightLock_Lock(&s_preadLock);
	ssize_t n = -1;
	if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset)
		n = read(fd, buffer, count);
	LightLock_Unlock(&s_preadLock);
	return n;
}
