/*
 * Vagrant Story widescreen.
 *
 * PSXRecomp keeps widescreen out of generic Settings: a title opts in through
 * its own mod catalog. This plugin backs the "vs.enhancement.widescreen"
 * package and applies the aspect the player picked in the launcher's Mods
 * page before the game window opens. 4:3 stays the default when the feature
 * is off.
 */
#include "mod_plugins.h"
#include "vs_prompts.h"
#include "gpu.h"
#include "gpu_gl_renderer.h"

#include <stdio.h>
#include <string.h>

static void vs_widescreen_activate(void) {
    char aspect[16] = "";
    char hud[16] = "";
    (void)psx_mod_current_option_value("aspect", aspect, sizeof aspect);
    (void)psx_mod_current_option_value("hud", hud, sizeof hud);

    if (strcmp(aspect, "fit") == 0) {
        /* Start at 16:9, then follow the window as it is resized. */
        (void)psx_mod_set_fixed_display_aspect(16, 9);
        (void)psx_mod_set_adaptive_display_aspect(0, 0);
    } else if (strcmp(aspect, "21:9") == 0) {
        (void)psx_mod_set_fixed_display_aspect(21, 9);
    } else if (strcmp(aspect, "16:10") == 0) {
        (void)psx_mod_set_fixed_display_aspect(16, 10);
    } else {
        (void)psx_mod_set_fixed_display_aspect(16, 9);
    }
    (void)psx_mod_set_widescreen_hud_size(strcmp(hud, "proportional") == 0);
    /* Widen only the 3D field. The title screen, in-game menus and the
     * ending stay 4:3 (they draw from VRAM areas the wide view would show). */
    psx_mod_set_world_scene_predicate(vs_wide_world_scene);
    /* The depth-of-field blur (BATTLE.PRG) draws the foreground characters
     * into a 320x224 layer at VRAM 704,256 and pastes it back over the screen
     * as blurred tiles. Give that layer reveal margins of its own so the blur
     * reaches the screen edges instead of stopping at the 4:3 frame. */
    gpu_ws_set_aux_target(704, 256);
    gl_renderer_set_wide_aux(704, 256, 224, 1);
    /* Cutscene cuts ghost the previous frame (the other display buffer, at
     * VRAM 0 or 320) over the new one with sprites; read its margins too. */
    gl_renderer_set_wide_aux(0, 0, 224, 0);
    gl_renderer_set_wide_aux(320, 0, 224, 0);

    fprintf(stdout, "vagrantstory: widescreen %s (HUD %s)\n",
            aspect[0] ? aspect : "16:9", hud[0] ? hud : "original");
}

PSX_MOD_CONSTRUCTOR(vs_register_widescreen_plugin) {
    (void)psx_mod_register_activation_plugin("vs.widescreen",
                                             vs_widescreen_activate);
    (void)psx_mod_register_vblank_plugin("vs.widescreen", vs_wide_world_tick);
}
