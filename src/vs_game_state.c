/*
 * Vagrant Story game-state probes for host features (button prompts).
 * Reads guest RAM only; never writes it. Addresses and layouts come from the
 * rood-reverse decompilation (BATTLE.PRG symbol_addrs.txt, 146C.h, 5BF94.h).
 */
#include "mod_plugins.h"
#include "vs_prompts.h"

#include <stdint.h>

/* BATTLE.PRG is the field/battle engine, loaded at 0x80068800. Two code
 * words from the stock image prove it is the resident overlay. */
#define BATTLE_SIG0_ADDR 0x8009723Cu   /* map renderer entry */
#define BATTLE_SIG0      0x3C071F80u
#define BATTLE_SIG1_ADDR 0x800C8C50u   /* vs_battle_loadMenuPrg */
#define BATTLE_SIG1      0x27BDFFD8u

/* TITLE.PRG (title screen, New Game / Continue, memory-card menus) loads at
 * the same address; two words from its stock image identify it. */
#define TITLE_SIG0_ADDR 0x80068900u
#define TITLE_SIG0      0x8006CB3Cu
#define TITLE_SIG1_ADDR 0x80069800u
#define TITLE_SIG1      0x3C048006u

#define VS_MENU_STATE       0x800F51C0u   /* vs_battle_menuState.currentMenu */
#define VS_MENU_CLOSING     0x1F
#define VS_MENU_CLOSED      0x3F
#define VS_CHARACTER_STATE  0x800F19FCu   /* vs_battle_actor* (Ashley) */
#define VS_ACTOR_WEAPON_OFS 0x20u         /* vs_battle_actor.weaponDrawn */

static int ram_ptr(uint32_t p) {
    return p >= 0x80000000u && p < 0x80200000u && (p & 3u) == 0;
}

int vs_battle_resident(void) {
    return psx_mod_read_word(BATTLE_SIG0_ADDR) == BATTLE_SIG0 &&
           psx_mod_read_word(BATTLE_SIG1_ADDR) == BATTLE_SIG1;
}

static int title_resident(void) {
    return psx_mod_read_word(TITLE_SIG0_ADDR) == TITLE_SIG0 &&
           psx_mod_read_word(TITLE_SIG1_ADDR) == TITLE_SIG1;
}

int vs_prompts_context(void) {
    if (!vs_battle_resident())   /* movies are filtered by the caller */
        return title_resident() ? VS_CTX_TITLE : VS_CTX_HIDDEN;
    uint8_t menu = psx_mod_read_byte(VS_MENU_STATE);
    if (menu != VS_MENU_CLOSED && menu != VS_MENU_CLOSING) return VS_CTX_MENU;
    uint32_t ashley = psx_mod_read_word(VS_CHARACTER_STATE);
    if (ram_ptr(ashley) &&
        (psx_mod_read_word(ashley + VS_ACTOR_WEAPON_OFS) & 1u))
        return VS_CTX_BATTLE;
    return VS_CTX_FIELD;
}

/* Widescreen world-scene predicate: the 3D field, including the Triangle
 * menu ring drawn over it (currentMenu 0, or 0x1F while it closes). The
 * full-screen submenus (Items, Equip, Status...; currentMenu 0x40 | item,
 * MAINMENU.PRG) park their sliding panels just outside the 320-pixel screen
 * and leave the rest of VRAM uncleared, so a widened view shows those panels
 * and garbage at the sides: they present 4:3 instead. */
/* MAINMENU.PRG (the in-game menu pages) loads at 0x800F9800. Pages with the
 * full-screen backdrop fade it in via vs_mainMenu_backgroundFadeStep
 * (0x801022D8, 0..16; nonzero while the backdrop is drawn at all). */
#define MAINMENU_SIG0_ADDR 0x800FFB68u
#define MAINMENU_SIG0      0x10800006u
#define MAINMENU_SIG1_ADDR 0x800FFB8Cu
#define MAINMENU_SIG1      0xA04422DCu
#define VS_MENU_BACKDROP   0x801022D8u

static int menu_backdrop_on(void) {
    return psx_mod_read_word(MAINMENU_SIG0_ADDR) == MAINMENU_SIG0 &&
           psx_mod_read_word(MAINMENU_SIG1_ADDR) == MAINMENU_SIG1 &&
           psx_mod_read_word(VS_MENU_BACKDROP) != 0;
}

static int full_screen_menu(uint8_t menu) {
    /* Only the ring over the field (0) and the closed/closing markers
     * (0x3F, 0x1F, plus 0x7F/0x5F with the "from main menu" bit) leave the
     * field visible. Every other value is a full-screen menu page: 0x40|item
     * from the ring, and the nested pages (Items > Setup, ...) that use
     * other values. */
    uint8_t item = menu & 0x3F;
    if (item == VS_MENU_CLOSED || item == VS_MENU_CLOSING || menu == 0) return 0;
    /* Pages drawn over the field (Options, ...) keep the field visible; only
     * pages that draw the full-screen backdrop (Items, Equip, Status, Setup,
     * ...) are full screen. */
    return menu_backdrop_on();
}

/* Debounced once per guest VBlank (vs_wide_world_tick). Asymmetric: a
 * full-screen menu switches to 4:3 after 2 frames, so its sliding panels are
 * hidden almost at once, while going back to wide waits 10 frames, so the
 * menu's closing animation and transient state values never flash the
 * picture between the two. */
static int s_shown_wide = 1, s_streak = 0;

void vs_wide_world_tick(void) {
    int want = vs_battle_resident() &&
               !full_screen_menu(psx_mod_read_byte(VS_MENU_STATE));
    if (want == s_shown_wide) {
        s_streak = 0;
    } else if (++s_streak >= (want ? 10 : 2)) {
        s_shown_wide = want;
        s_streak = 0;
    }
}

int vs_wide_world_scene(void) {
    return s_shown_wide && vs_battle_resident();
}
