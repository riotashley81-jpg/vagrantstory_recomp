/*
 * Vagrant Story host performance settings that do not change what the game
 * does (always on, not a mod feature).
 *
 * VSync(0) waits in PsyQ v_wait (SLUS_010.40 0x8001F83C): it counts a timeout
 * down in a stack slot and re-reads the VBlank counter until it changes. The
 * block at 0x8001F864 is the top of that loop; let the runtime advance guest
 * time to the next device event there instead of emulating every iteration
 * (psx_mod_add_wait_loop).
 */
#include "mod_plugins.h"

#define VS_VWAIT_LOOP 0x8001F864u

PSX_MOD_CONSTRUCTOR(vs_register_perf) {
    (void)psx_mod_add_wait_loop(VS_VWAIT_LOOP);
}
