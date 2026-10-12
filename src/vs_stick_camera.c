/*
 * Vagrant Story: rotate the camera with the right analog stick.
 *
 * The game turns the camera in 90-degree steps while L1 or R1 is held
 * (BATTLE.PRG func_8006E158). This pad transform presses R1/L1 for the
 * player while the right stick is pushed left/right (the "invert" option
 * swaps them), so the stick and the shoulder buttons both work. It is
 * inactive in menus and outside the field/battle engine; the right-stick
 * click (R3) and every other input pass through untouched.
 *
 * In first-person view the game looks around with the D-pad, but any
 * right-stick push in analog mode reads as the look button, which leaves the
 * view again. So there the right stick is turned into D-pad presses (look
 * around), and pushing the left stick leaves first-person view:
 * the transform taps Start, one of the buttons _checkFirstPersonViewExit
 * accepts. The stick has to return to centre after the view is entered
 * before it can do that, so walking into the view never bounces back out.
 *
 * Backed by the "vs.enhancement.stickcam" mod package.
 */
#include "mod_plugins.h"
#include "vs_prompts.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAD_L1    0x0400u   /* active-low PSX pad bits */
#define PAD_R1    0x0800u
#define PAD_START 0x0008u
#define PAD_UP    0x0010u
#define PAD_RIGHT 0x0020u
#define PAD_DOWN  0x0040u
#define PAD_LEFT  0x0080u
#define LOOK_DEADZONE 0x30

#define VS_FIRST_PERSON 0x800F186Cu   /* _firstPersonViewEnabled (int) */
#define VS_MENU_STATE   0x800F51C0u   /* vs_battle_menuState.currentMenu */
#define VS_MENU_CLOSED  0x3F

#define STICK_PRESS   0x50   /* deflection from centre that starts a turn */
#define STICK_RELEASE 0x30   /* ...and the smaller one that ends it */

#define EXIT_TAP_FRAMES 4

static int s_invert = 0;
static int s_held = 0;   /* -1 left, 0 none, +1 right (with hysteresis) */
static int s_fp_armed = 0;   /* left stick centred since first person began */
static int s_fp_tap = 0;     /* frames of Start still to press */

static int field_active(void) {
    return vs_battle_resident() &&
           psx_mod_read_byte(VS_MENU_STATE) == VS_MENU_CLOSED;
}

static int stick_deflected(uint32_t x, uint32_t y, int threshold) {
    int dx = (int)x - 0x80, dy = (int)y - 0x80;
    return dx * dx + dy * dy >= threshold * threshold;
}

/* First-person view: the left stick leaves it (taps Start). */
static int first_person(const PSXModPadFrame* frame, PSXModPadOutput* out) {
    if (s_fp_tap > 0) {
        s_fp_tap--;
        out->buttons &= ~PAD_START;
    } else if (!stick_deflected(frame->lx, frame->ly, STICK_RELEASE)) {
        s_fp_armed = 1;
    } else if (s_fp_armed && stick_deflected(frame->lx, frame->ly, STICK_PRESS)) {
        s_fp_armed = 0;
        s_fp_tap = EXIT_TAP_FRAMES - 1;
        out->buttons &= ~PAD_START;
    }
    /* Right stick: look around through the D-pad. */
    int rx = (int)frame->rx - 0x80, ry = (int)frame->ry - 0x80;
    if (rx <= -LOOK_DEADZONE) out->buttons &= ~PAD_LEFT;
    if (rx >= LOOK_DEADZONE)  out->buttons &= ~PAD_RIGHT;
    /* Up looks up: the game's D-pad pitch is flight-style (Up looks down),
     * so the vertical axis is crossed over, for the D-pad as well. */
    int up = !(frame->buttons & PAD_UP) || ry <= -LOOK_DEADZONE;
    int down = !(frame->buttons & PAD_DOWN) || ry >= LOOK_DEADZONE;
    out->buttons |= PAD_UP | PAD_DOWN;
    if (up)   out->buttons &= ~PAD_DOWN;
    if (down) out->buttons &= ~PAD_UP;
    out->lx = 0x80;
    out->ly = 0x80;
    out->rx = 0x80;
    out->ry = 0x80;
    return 1;
}

static int stick_camera(const PSXModPadFrame* frame, PSXModPadOutput* out) {
    int dx = (int)frame->rx - 0x80;
    if (s_held == 0) {
        if (dx <= -STICK_PRESS) s_held = -1;
        else if (dx >= STICK_PRESS) s_held = 1;
    } else if ((s_held < 0 && dx > -STICK_RELEASE) ||
               (s_held > 0 && dx < STICK_RELEASE)) {
        s_held = 0;
    }
    if (!field_active()) {   /* stock pad (menus, outside the field) */
        s_fp_armed = 0;
        s_fp_tap = 0;
        return 0;
    }
    if (psx_mod_read_word(VS_FIRST_PERSON) != 0) return first_person(frame, out);
    if (s_fp_tap > 0) {   /* finish a tap that left first person */
        s_fp_tap--;
        out->buttons &= ~PAD_START;
    }
    s_fp_armed = 0;
    /* In analog mode the game treats any right-stick push as "enter free
     * look". Hide the stick from it here so only R3 enters first person;
     * inside first person the right stick passes through to look around. */
    out->rx = 0x80;
    out->ry = 0x80;
    if (s_held != 0) {
        int right = s_invert ? (s_held > 0) : (s_held < 0);
        out->buttons &= ~(right ? PAD_R1 : PAD_L1);
    }
    return 1;
}

static void vs_stickcam_activate(void) {
    char v[16] = "";
    (void)psx_mod_current_option_value("invert", v, sizeof v);
    s_invert = strcmp(v, "true") == 0;
    s_held = 0;
    static const PSXModPadTransform t = {
        sizeof(PSXModPadTransform),
        PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_DIGITAL) | PSX_MOD_PAD_TYPE_BIT(PSX_MOD_PAD_DUALSHOCK),
        PSX_MOD_PAD_DIGITAL,
        stick_camera,
    };
    int ok = psx_mod_set_pad_transform(0, &t);
    fprintf(stdout, "vagrantstory: right-stick camera %s%s\n",
            ok ? "on" : "FAILED to register", s_invert ? " (inverted)" : "");
}

PSX_MOD_CONSTRUCTOR(vs_register_stickcam_plugin) {
    (void)psx_mod_register_activation_plugin("vs.stickcam", vs_stickcam_activate);
}
