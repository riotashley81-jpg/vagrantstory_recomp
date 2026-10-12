/*
 * Vagrant Story: skip the boot logos with Enter / Circle (or Start).
 *
 * TITLE.PRG's _displayPublisherAndDeveloper() shows two logos for 364 frames
 * each (about 6 s). The second already ends early on any button, but the
 * first never reads the pad. This hooks the first loop's end-of-iteration
 * compare (slti v0, s2, 364 at 0x8006F6EC) and, while the logo is fully faded
 * in, jumps its counter to the start of the fade-out when the player presses
 * Enter, Circle or Start on the keyboard or a gamepad. The fade-out still
 * plays, so the change is a quick, clean cut.
 *
 * Backed by the "vs.enhancement.skiplogos" mod package.
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "psx_keybinds.h"
#include "psx_sdl.h"
#include "freeze_heartbeat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LOGO_LOOP_SITE    0x8006F6ECu
#define LOGO_LOOP_INSN    0x2A42016Cu   /* slti v0, s2, 364 */
#define LOGO_FADE_IN_END  32
#define LOGO_FADE_OUT     331
#define REG_S2            18

static int skip_pressed(void) {
    static int forced = -1;   /* VS_SKIPLOGOS_FORCE=1: always "pressed" (testing) */
    if (forced < 0) forced = getenv("VS_SKIPLOGOS_FORCE") != NULL;
    if (forced) return 1;
    const bool* keys = SDL_GetKeyboardState(NULL);
    if (keys) {
        uint16_t kb = psx_keybinds_pad_word((const uint8_t*)keys, 1);   /* active low */
        if (!(kb & 0x2000u) || !(kb & 0x0008u)) return 1;                /* Circle, Start */
        if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]) return 1;
    }
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    int hit = 0;
    for (int i = 0; ids && i < n && !hit; i++) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        if (g && (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST) ||
                  SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)))
            hit = 1;
    }
    SDL_free(ids);
    return hit;
}

static void logo_loop_check(struct CPUState* cpu, uint32_t address) {
    (void)address;
    uint32_t i = cpu->gpr[REG_S2];
    if (i >= LOGO_FADE_IN_END && i < LOGO_FADE_OUT && skip_pressed()) {
        cpu->gpr[REG_S2] = LOGO_FADE_OUT;
        fprintf(stdout, "vagrantstory: boot logo skipped at frame %u of 364\n", i);
        fflush(stdout);
    }
}

/* The runtime's freeze watchdog writes a diagnostic psx_freeze_dump_*.json
 * when guest code looks pinned for a few seconds while frames advance. The
 * boot sequence (BIOS hand-off and the logo loops, which only wait on VSync)
 * matches that pattern on every launch, so the watchdog stands down until
 * the title overlay has gone and the first 20 s have passed. */
static uint32_t s_vblanks = 0;
static int s_watchdog_paused = -1;

static int title_resident(void) {
    return psx_mod_read_word(0x80068900u) == 0x8006CB3Cu &&
           psx_mod_read_word(0x80069800u) == 0x3C048006u;
}

static void vs_boot_watchdog_tick(void) {
    if (s_vblanks < 0xFFFFFFFFu) s_vblanks++;
    int pause = s_vblanks < 1200u || title_resident();
    if (pause != s_watchdog_paused) {
        freeze_heartbeat_set_paused(pause);
        s_watchdog_paused = pause;
    }
}

static void vs_skiplogos_activate(void) {
    fprintf(stdout, "vagrantstory: skippable boot logos on\n");
    fflush(stdout);
}

PSX_MOD_CONSTRUCTOR(vs_register_skiplogos_plugin) {
    (void)psx_mod_register_activation_plugin("vs.skiplogos", vs_skiplogos_activate);
    (void)psx_mod_register_instruction_plugin("vs.skiplogos", LOGO_LOOP_SITE,
                                              LOGO_LOOP_INSN, logo_loop_check);
    (void)psx_mod_register_vblank_plugin("vs.skiplogos", vs_boot_watchdog_tick);
}
