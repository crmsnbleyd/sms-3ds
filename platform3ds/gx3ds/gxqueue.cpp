// citro3d keeps the GPU's transfer and command-list submissions of a frame in a 32-entry queue that
// only runs at C3D_FrameEnd, and libctru panics (svcBreak in gxCmdQueueAdd) when it is full. Each
// EFB copy adds a few entries (a command-list split before the snapshot and the copy, the
// write-back's split and copy), so an area with many copies a frame (Noki Bay's goop maps) filled
// it. The linker wraps the GX calls citro3d makes (CMakeLists.txt, -Wl,--wrap): when the queue is
// about to be full, what it holds is run and waited for first, as citro3d itself does at
// C3D_FrameBegin. The commands keep their order, so this only does the frame's queued work earlier.
#include <3ds.h>

extern "C" {
void __real_GX_BindQueue(gxCmdQueue_s* queue);
Result __real_GX_ProcessCommandList(u32* buf0a, u32 buf0s, u8 flags);
Result __real_GX_MemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e,
                            u16 control1);
Result __real_GX_DisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags);
Result __real_GX_TextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags);
}

volatile uint32_t sms3ds_gx_queue_drains;  // times the queue was run early (azahar.py can read it)

namespace {

gxCmdQueue_s* s_queue;  // the queue citro3d bound

void ensureRoom()
{
	gxCmdQueue_s* q = s_queue;
	if (!q || q->numEntries + 1 < q->maxEntries)
		return;
	gxCmdQueueRun(q);
	gxCmdQueueWait(q, -1);
	gxCmdQueueStop(q);
	gxCmdQueueClear(q);
	sms3ds_gx_queue_drains = sms3ds_gx_queue_drains + 1;
}

}  // namespace

extern "C" {

void __wrap_GX_BindQueue(gxCmdQueue_s* queue)
{
	s_queue = queue;
	__real_GX_BindQueue(queue);
}

Result __wrap_GX_ProcessCommandList(u32* buf0a, u32 buf0s, u8 flags)
{
	ensureRoom();
	return __real_GX_ProcessCommandList(buf0a, buf0s, flags);
}

Result __wrap_GX_MemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e,
                            u16 control1)
{
	ensureRoom();
	return __real_GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
}

Result __wrap_GX_DisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags)
{
	ensureRoom();
	return __real_GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
}

Result __wrap_GX_TextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags)
{
	ensureRoom();
	return __real_GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
}

}  // extern "C"
