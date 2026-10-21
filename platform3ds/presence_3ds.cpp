// The game reports its state once a frame (sms-pc-port's zzz-presence-01 patch, made for the
// launcher's Discord presence): keep it for the HUD and log every change, so a run without a
// picture still shows where the game is.
#include <stdio.h>

#include "port_platform.h"
#include "port_presence.h"
#include "sms3ds.h"

class MSound;
extern MSound* gpMSound;  // created in TApplication::initialize_bootAfter

volatile int sms3ds_state = -1, sms3ds_area = -1, sms3ds_episode = -1, sms3ds_save_file = -1;

extern "C" void port_presence_frame(const PortPresence* p)
{
	static PortPresence last = {-1, -1, -1, -1, -1, -1, -2, -1, -1};
	static MSound* lastSound = (MSound*)1;
	if (p->state != last.state || p->area != last.area || p->episode != last.episode ||
	    p->saveFile != last.saveFile || p->paused != last.paused || p->cutscene != last.cutscene)
		port_log("[game] state %d area %d episode %d file %d%s%s (frame %lu)\n", p->state, p->area, p->episode,
		         p->saveFile, p->paused ? " paused" : "", p->cutscene ? " cutscene" : "",
		         (unsigned long)sms3ds_gx_frames);
	if (gpMSound != lastSound)
		port_log("[game] gpMSound %p\n", (void*)gpMSound);
	lastSound = gpMSound;
	last = *p;
	sms3ds_state = p->state;
	sms3ds_area = p->area;
	sms3ds_episode = p->episode;
	sms3ds_save_file = p->saveFile;
}
