/*
 * Vagrant Story: exit options for the PC port.
 *
 * In-game menu (Triangle): two rows under the game's own list, "EXIT TO MAIN
 * MENU" and "EXIT GAME". The list is BATTLE.PRG's func_800C930C (rood-reverse
 * 5BF94.c), which only knows ten rows (Magic ... Quick Manual, shown when
 * their vs_main_settings.menuFlags bit is set). A function filter replaces it
 * with the same logic over twelve rows, calling the game's own helpers for
 * the row boxes, cursor, sounds and help line, so the new rows look and
 * animate like the others. Picking one asks for a second press (the help
 * line says so), then either tail-calls vs_main_resetGame, the game's soft
 * reset (back to the title screen, like Start+Select+L1+L2+R1+R2), or closes
 * the window as the X button does.
 *
 * Title screen: the scrolling New Game / Continue / Vibration / Sound wheel
 * (TITLE.PRG vs_title_exec) gets a fifth entry, "Exit Game", after Sound.
 * The wheel wraps with "& 3", so instruction hooks redirect its index maths
 * to five entries. The new entry uses menu slot 8, whose tables point at an
 * unused "Easy" label and glow in VRAM; once the title has uploaded its
 * images, the label is repainted as "Exit Game" from letters of the game's
 * own labels (no game data ships here) and the glow copied from New Game's.
 *
 * Backed by the "vs.enhancement.exitmenu" mod package.
 */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "gpu.h"
#include "psx_sdl.h"
#include "vs_prompts.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REG_V0 2
#define REG_A0 4
#define REG_A1 5
#define REG_A2 6
#define REG_A3 7
#define REG_S0 16
#define REG_S2 18
#define REG_SP 29
#define REG_RA 31

/* ---- shared -------------------------------------------------------------- */

static void request_quit(void) {
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_QUIT;
    fprintf(stdout, "vagrantstory: Exit Game chosen, closing\n");
    fflush(stdout);
    SDL_PushEvent(&ev);
}

/* ---- in-game menu (BATTLE.PRG) ------------------------------------------- */

#define RING_FUNC           0x800C930Cu   /* func_800C930C(mode) */
#define RING_SIG0           0x27BDFFC0u   /* addiu sp, sp, -0x40 */
#define RING_SIG1           0xAFB00018u   /* sw s0, 0x18(sp) */

#define VS_MAIN_RESET_GAME  0x80042364u   /* vs_main_resetGame */
#define VS_SETTINGS_CURSOR  0x80060021u   /* vs_main_settings.cursorMemory */
#define VS_SETTINGS_MENUS   0x80060022u   /* vs_main_settings.menuFlags (u16) */
#define VS_BUTTONS_PRESSED  0x8005E1D0u
#define VS_BUTTON_REPEAT    0x8005DFDCu
#define VS_BUTTONS_STATE    0x8005E238u

#define VS_GET_SCENE_ID     0x8008D400u   /* vs_battle_getCurrentSceneId */
#define VS_SFX_LEAVE        0x800C02E8u   /* vs_battle_playMenuLeaveSfx */
#define VS_SFX_SELECT       0x800C02F0u   /* vs_battle_playMenuSelectSfx */
#define VS_SFX_CHANGE       0x800C02F8u   /* vs_battle_playMenuChangeSfx */
#define VS_SET_TEXT_BOX     0x800C6BF0u   /* vs_battle_setTextBox */
#define VS_INIT_INFO_BOX    0x800C8E04u   /* vs_battle_initInformationTextBox */
#define VS_SET_MENU_ITEM    0x800C8E5Cu   /* vs_battle_setMenuItem */
#define VS_RENDER_MENU_ITEM 0x800C9078u   /* vs_battle_renderMenuItem */
#define VS_RMEMZERO         0x800CCC1Cu   /* vs_battle_rMemzero */
#define VS_DRAW_CURSOR      0x800CCD40u   /* vs_battle_drawCursor */

#define VS_MENU_ITEMS_PTR   0x800EB9C0u   /* vs_battle_menuItems (0x40 each) */
#define VS_MENU_STRINGS     0x800EB9DCu   /* vs_battle_menuStrings */
#define VS_CANT_USE_MSG     0x800EBA78u   /* "This command cannot be used ..." */
#define VS_ROW_STEPS        0x800EBBC8u   /* vs_battle_rowAnimationSteps */
#define VS_CURSOR_ANIM      0x800F4CC0u   /* D_800F4CC0 */
#define VS_SUBMENU_STATE0   0x800F4E70u   /* vs_battle_submenuStates[0] */
#define VS_MENU_LOCKS       0x800F4EA0u   /* D_800F4EA0 */
#define VS_INFO_BOX_STATE   0x800F514Cu   /* vs_battle_textBoxes[7].state (u16) */
#define VS_STRING_ARG0      0x800F5190u   /* vs_battle_stringContext.strings[0] */

#define ITEM_SIZE           0x40u
#define ITEM_WIDTH_FLAG     2u
#define ITEM_ANIM_STEP      3u
#define ITEM_SELECTED       6u
#define ITEM_UNSELECTABLE   7u
#define ITEM_Y              0x16u

#define PAD_TRIANGLE        0x0010u
#define PAD_CIRCLE          0x0020u
#define PAD_CROSS           0x0040u
#define PAD_UP              0x1000u
#define PAD_DOWN            0x4000u

#define GAME_ROWS           10
#define ROW_TO_TITLE        10
#define ROW_QUIT            11
#define ALL_ROWS            12

static int s_armed = -1;      /* row waiting for its confirming press */

enum { STR_ROW_TITLE, STR_ROW_QUIT, STR_HELP_TITLE, STR_HELP_QUIT, STR_CONFIRM, STR_N };
static const char* const k_text[STR_N] = {
    "EXIT TO MAIN MENU",
    "EXIT GAME",
    "Return to the title screen.\nUnsaved progress is lost.",
    "Close Vagrant Story.\nUnsaved progress is lost.",
    "Press @ again to confirm.",
};

/* The game's text encoding (rood-reverse tools/etc/vsString.py). '@' stands
 * for the Circle glyph, '\n' for a line break. */
static uint8_t vs_char(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x0A + c - 'A');
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x24 + c - 'a');
    switch (c) {
    case '.': return 0xA0;
    case ',': return 0x9F;
    case '!': return 0x90;
    case '?': return 0xA5;
    case '@': return 0xB0;
    case '\n': return 0xE8;
    default: return 0x8F;   /* space */
    }
}

/* Writes an encoded string at `at` (help lines get the "|!" prefix the
 * game's own help strings start with) and returns `at`. */
static uint32_t put_string(uint32_t at, int id) {
    uint32_t p = at;
    if (id >= STR_HELP_TITLE) {
        psx_mod_write_byte(p++, 0xF8);
        psx_mod_write_byte(p++, 0x00);
    }
    for (const char* s = k_text[id]; *s; s++) psx_mod_write_byte(p++, vs_char(*s));
    psx_mod_write_byte(p, 0xE7);
    return at;
}

static int ring_code_ok(void) {
    return vs_battle_resident() &&
           psx_mod_read_word(RING_FUNC) == RING_SIG0 &&
           psx_mod_read_word(RING_FUNC + 4) == RING_SIG1;
}

static int row_enabled(int row) {
    if (row >= GAME_ROWS) return 1;
    return (psx_mod_read_half(VS_SETTINGS_MENUS) >> row) & 1;
}

static uint32_t item_addr(int row) {
    return psx_mod_read_word(VS_MENU_ITEMS_PTR) + (uint32_t)row * ITEM_SIZE;
}

/* Guest calls run on a scratch frame below the caller's stack, so the two
 * stack arguments of vs_battle_setMenuItem never touch the caller's frame.
 * Our row labels go in the same frame (setMenuItem copies them). */
static uint32_t s_ret;

static uint32_t row_text(struct CPUState* cpu, int row) {
    if (row == ROW_TO_TITLE) return put_string(cpu->gpr[REG_SP] + 24, STR_ROW_TITLE);
    if (row == ROW_QUIT) return put_string(cpu->gpr[REG_SP] + 24, STR_ROW_QUIT);
    return VS_MENU_STRINGS + 2u * psx_mod_read_half(VS_MENU_STRINGS + 2u * (uint32_t)row);
}

/* The help box keeps a pointer to its text and draws it over the next frames,
 * so help lines live in the last menu items, which nothing uses while the
 * list is up: bytes 1..63 of items 37..39 (byte 0, the item state, stays 0
 * so no renderer picks them up). Rewritten every frame they are shown. */
static uint32_t help_text(int id) {
    return put_string(item_addr(37 + id - STR_HELP_TITLE) + 1, id);
}
static uint32_t call(struct CPUState* cpu, uint32_t fn, uint32_t a0, uint32_t a1,
                     uint32_t a2, uint32_t a3) {
    return psx_mod_call_guest(cpu, fn, s_ret, a0, a1, a2, a3);
}

static void set_menu_item(struct CPUState* cpu, int row, int x, int y, uint32_t text) {
    const uint32_t sp = cpu->gpr[REG_SP];
    psx_mod_write_word(sp + 16, 0);
    psx_mod_write_word(sp + 20, text);
    call(cpu, VS_SET_MENU_ITEM, (uint32_t)row, (uint32_t)x, (uint32_t)y, 126);
}

static void show_help(struct CPUState* cpu, uint32_t text) {
    call(cpu, VS_INIT_INFO_BOX, 1, 0, 0, 0);
    psx_mod_write_half(VS_INFO_BOX_STATE, 11);
    call(cpu, VS_SET_TEXT_BOX, 7, text, 0, 0);
}

/* mode 1/2: (re)open the list; the rows slide in one after another. */
static int ring_open(struct CPUState* cpu, int mode) {
    int sel = psx_mod_read_byte(VS_SUBMENU_STATE0);
    if (mode == 2 && !psx_mod_read_byte(VS_SETTINGS_CURSOR)) sel = 0;
    if (sel >= ALL_ROWS) sel = 0;
    while (!row_enabled(sel)) sel = (sel + 1) % ALL_ROWS;
    psx_mod_write_byte(VS_SUBMENU_STATE0, (uint8_t)sel);
    call(cpu, VS_RMEMZERO, psx_mod_read_word(VS_MENU_ITEMS_PTR), 0xB24, 0, 0);
    int step = 9;
    for (int row = 0; row < ALL_ROWS; row++)
        if (row_enabled(row))
            psx_mod_write_byte(item_addr(row) + ITEM_ANIM_STEP, (uint8_t)step++);
    psx_mod_write_word(VS_CURSOR_ANIM, (uint32_t)-step);
    s_armed = -1;
    return 0;
}

static void ring_slide_in(struct CPUState* cpu) {
    psx_mod_write_word(VS_CURSOR_ANIM, psx_mod_read_word(VS_CURSOR_ANIM) + 1);
    int shown = 0;
    for (int row = 0; row < ALL_ROWS; row++) {
        if (!row_enabled(row)) continue;
        const uint32_t item = item_addr(row);
        uint8_t step = psx_mod_read_byte(item + ITEM_ANIM_STEP);
        if (step) psx_mod_write_byte(item + ITEM_ANIM_STEP, --step);
        /* The steps table is 16 offsets plus four "off screen" (0xFF) bytes;
         * twelve rows can start one entry past them. */
        if (step > 19) step = 19;
        set_menu_item(cpu, row, psx_mod_read_byte(VS_ROW_STEPS + step) + 194,
                      shown * 16 + 18, row_text(cpu, row));
        shown++;
    }
    const uint32_t locks = psx_mod_read_word(VS_MENU_LOCKS);
    psx_mod_write_byte(item_addr(0) + ITEM_UNSELECTABLE, (locks & 0xB7) != 0);
    psx_mod_write_byte(item_addr(1) + ITEM_UNSELECTABLE, (locks & 0x15F) != 0);
    psx_mod_write_byte(item_addr(3) + ITEM_UNSELECTABLE, (locks & 7) != 0);
    psx_mod_write_byte(item_addr(5) + ITEM_UNSELECTABLE,
                       call(cpu, VS_GET_SCENE_ID, 0, 0, 0, 0) == 0);
}

/* One of our rows was confirmed. Returns 1 when the game should go to the
 * title screen (the caller turns the return into vs_main_resetGame). */
static int ring_exit(int row) {
    if (row == ROW_QUIT) {
        request_quit();
        return 0;
    }
    fprintf(stdout, "vagrantstory: Exit to main menu chosen\n");
    fflush(stdout);
    return 1;
}

static int ring_input(struct CPUState* cpu, int* to_title) {
    const uint32_t pressed = psx_mod_read_word(VS_BUTTONS_PRESSED);
    int sel = psx_mod_read_byte(VS_SUBMENU_STATE0);
    if (sel >= ALL_ROWS) sel = 0;
    uint32_t item = item_addr(sel);

    if (pressed & PAD_CIRCLE) {
        if (sel < GAME_ROWS) {
            call(cpu, VS_SFX_SELECT, 0, 0, 0, 0);
            psx_mod_write_byte(item + ITEM_SELECTED, 1);
            psx_mod_write_byte(item + ITEM_WIDTH_FLAG, 1);
            psx_mod_write_half(VS_INFO_BOX_STATE, 0);
            return sel + 1;
        }
        call(cpu, VS_SFX_SELECT, 0, 0, 0, 0);
        if (s_armed == sel) {
            s_armed = -1;
            *to_title = ring_exit(sel);
            if (*to_title) return 0;
        } else {
            s_armed = sel;
        }
    }
    psx_mod_write_byte(item + ITEM_SELECTED, 0);

    if (pressed & (PAD_TRIANGLE | PAD_CROSS)) {
        call(cpu, VS_SFX_LEAVE, 0, 0, 0, 0);
        psx_mod_write_half(VS_INFO_BOX_STATE, 0);
        s_armed = -1;
        return -1;
    }

    const uint32_t repeat = psx_mod_read_word(VS_BUTTON_REPEAT);
    if (repeat & PAD_UP) {
        do sel = (sel + ALL_ROWS - 1) % ALL_ROWS;
        while (!row_enabled(sel));
    }
    if (repeat & PAD_DOWN) sel = (sel + 1) % ALL_ROWS;
    while (!row_enabled(sel)) sel = (sel + 1) % ALL_ROWS;
    if (sel != psx_mod_read_byte(VS_SUBMENU_STATE0)) {
        call(cpu, VS_SFX_CHANGE, 0, 0, 0, 0);
        psx_mod_write_byte(VS_SUBMENU_STATE0, (uint8_t)sel);
        s_armed = -1;
    }

    item = item_addr(sel);
    psx_mod_write_byte(item + ITEM_SELECTED, 1);
    const int16_t y = (int16_t)psx_mod_read_half(item + ITEM_Y);
    psx_mod_write_word(VS_CURSOR_ANIM,
        call(cpu, VS_DRAW_CURSOR, psx_mod_read_word(VS_CURSOR_ANIM),
             (uint32_t)((y >> 4) - 1), 0, 0));

    if (psx_mod_read_byte(item + ITEM_UNSELECTABLE)) {
        if (psx_mod_read_word(VS_BUTTONS_STATE) & (PAD_UP | PAD_DOWN)) return 0;
        show_help(cpu, VS_CANT_USE_MSG);
        uint32_t mask = sel == 0 ? 0xB7 : sel == 1 ? 0x15F : sel == 3 ? 7 : 0;
        uint32_t why = psx_mod_read_word(VS_MENU_LOCKS) & mask;
        int reason = 0;
        while (reason < 9 && !(why & (1u << reason))) reason++;
        psx_mod_write_word(VS_STRING_ARG0, VS_MENU_STRINGS +
            2u * psx_mod_read_half(VS_MENU_STRINGS + 2u * (uint32_t)(reason + 12)));
    } else if (sel >= GAME_ROWS) {
        if (psx_mod_read_word(VS_BUTTONS_STATE) & (PAD_UP | PAD_DOWN)) return 0;
        show_help(cpu, help_text(s_armed == sel ? STR_CONFIRM
                                 : sel == ROW_QUIT ? STR_HELP_QUIT : STR_HELP_TITLE));
    } else {
        psx_mod_write_half(VS_INFO_BOX_STATE, 0);
    }
    return 0;
}

static int ring_filter(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!ring_code_ok()) return 0;
    const uint32_t caller_sp = cpu->gpr[REG_SP];
    const int mode = (int)cpu->gpr[REG_A0];
    s_ret = cpu->gpr[REG_RA];
    cpu->gpr[REG_SP] = caller_sp - 0x40;

    int result = 0, to_title = 0;
    if (mode) {
        result = ring_open(cpu, mode);
    } else if ((int32_t)psx_mod_read_word(VS_CURSOR_ANIM) < 0) {
        ring_slide_in(cpu);
    } else {
        result = ring_input(cpu, &to_title);
    }
    /* While MAINMENU.PRG is loading, BATTLE.PRG's func_800C97BC draws the
     * list itself and stops at row 9; MAINMENU's renderer covers all rows. */
    if (!mode && s_ret >= 0x80068800u && s_ret < 0x800F9800u)
        for (int row = GAME_ROWS; row < ALL_ROWS; row++)
            call(cpu, VS_RENDER_MENU_ITEM, item_addr(row), 0, 0, 0);

    cpu->gpr[REG_SP] = caller_sp;
    cpu->gpr[REG_V0] = (uint32_t)result;
    if (to_title) cpu->gpr[REG_RA] = VS_MAIN_RESET_GAME;   /* never returns */
    return 1;
}

/* ---- title screen (TITLE.PRG) -------------------------------------------- */

#define TITLE_SIG0_ADDR 0x80068900u
#define TITLE_SIG0      0x8006CB3Cu
#define TITLE_SIG1_ADDR 0x80069800u
#define TITLE_SIG1      0x3C048006u

#define TITLE_ITEMS     0x800EFDF8u   /* _menuItemStates[10], 8 bytes each */
#define TI_ENABLED      0
#define TI_STATE        1
#define TI_TEXT_POS     4
#define TI_TARGET_POS   5
#define TI_UP           1             /* menuItemStateUpper: moves up 4/frame */
#define TI_DOWN         2             /* menuItemStateLower */

#define TITLE_EXIT_SLOT 8             /* unused "Easy" slot */
#define TITLE_ENTRIES   5
static const int k_title_slot[TITLE_ENTRIES] = { 0, 1, 2, 3, TITLE_EXIT_SLOT };

/* Instruction hooks (all in TITLE.PRG; listed in game.toml). */
#define T_FADE_IN       0x8006FC3Cu   /* _setMenuItemFadeIn: sll a0, a0, 3 */
#define T_FADE_IN_INSN  0x000420C0u
#define T_SELECT        0x8007156Cu   /* vs_title_exec: switch (menuItem) */
#define T_SELECT_INSN   0x12570011u
#define T_WRAP          0x8007163Cu   /* andi s2, v0, 3 (menuItem wrap) */
#define T_WRAP_INSN     0x30520003u
#define T_WRAPPED       0x80071640u   /* bne s0, s7 (first use after it) */
#define T_WRAPPED_INSN  0x16170012u
#define T_MOVED_DOWN    0x80071684u   /* after the "scroll up" loop */
#define T_MOVED_DOWN_INSN 0x0801C5B2u
#define T_MOVED_UP      0x800716C4u   /* after the "scroll down" loop */
#define T_MOVED_UP_INSN 0x00008021u
#define T_SOUND_OPEN    0x80070EBCu   /* _menuSoundSettings, after it parks New Game */
#define T_SOUND_OPEN_INSN 0x3C028006u
#define T_SOUND_CLOSE   0x80071144u   /* ... after it brings New Game back */
#define T_SOUND_CLOSE_INSN 0x24040003u

static int title_entry(int slot) {
    for (int i = 0; i < TITLE_ENTRIES; i++)
        if (k_title_slot[i] == slot) return i;
    return 0;
}

static int title_slot_step(int slot, int delta) {
    return k_title_slot[(title_entry(slot) + delta + TITLE_ENTRIES) % TITLE_ENTRIES];
}

static uint32_t title_item(int slot) { return TITLE_ITEMS + (uint32_t)slot * 8u; }

/* _setMenuItemFadeIn(item, pos): the callers that pick the wheel neighbour
 * of the current entry, told apart by return address. */
static void title_fade_in(struct CPUState* cpu, uint32_t address) {
    (void)address;
    switch (cpu->gpr[REG_RA]) {
    case 0x8006FF20u: cpu->gpr[REG_A0] = (uint32_t)title_slot_step((int)cpu->gpr[REG_S0], 1); break;
    case 0x8006FF30u: cpu->gpr[REG_A0] = (uint32_t)title_slot_step((int)cpu->gpr[REG_S0], -1); break;
    case 0x80071658u: cpu->gpr[REG_A0] = (uint32_t)title_slot_step((int)cpu->gpr[REG_S2], 1); break;
    case 0x80071698u: cpu->gpr[REG_A0] = (uint32_t)title_slot_step((int)cpu->gpr[REG_S2], -1); break;
    default: break;
    }
}

static int s_title_next;

static void title_wrap(struct CPUState* cpu, uint32_t address) {
    (void)address;
    s_title_next = title_slot_step((int)cpu->gpr[REG_S2], (int32_t)cpu->gpr[REG_S0]);
}

static void title_wrapped(struct CPUState* cpu, uint32_t address) {
    (void)address;
    cpu->gpr[REG_S2] = (uint32_t)s_title_next;
}

/* The game's loop moved slots 0..3. Move the Exit slot the same way when it
 * is in view, and switch off the one entry that is neither shown nor fading
 * (four of five are), so it cannot wrap round into view. */
static void title_moved(int sel, int dir) {
    const int hidden = title_slot_step(sel, dir > 0 ? 2 : -2);
    for (int i = 0; i < TITLE_ENTRIES; i++) {
        const uint32_t item = title_item(k_title_slot[i]);
        if (k_title_slot[i] == hidden) {
            psx_mod_write_byte(item + TI_ENABLED, 0);
        } else if (k_title_slot[i] == TITLE_EXIT_SLOT) {
            const uint8_t pos = psx_mod_read_byte(item + TI_TEXT_POS);
            psx_mod_write_byte(item + TI_STATE, dir > 0 ? TI_UP : TI_DOWN);
            psx_mod_write_byte(item + TI_TARGET_POS, (uint8_t)(dir > 0 ? pos - 32 : pos + 32));
        }
    }
}

static void title_moved_down(struct CPUState* cpu, uint32_t address) {
    (void)address;
    title_moved((int)cpu->gpr[REG_S2], 1);
}

static void title_moved_up(struct CPUState* cpu, uint32_t address) {
    (void)address;
    title_moved((int)cpu->gpr[REG_S2], -1);
}

static void title_select(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[REG_S2] == TITLE_EXIT_SLOT) request_quit();
}

/* Sound's neighbour below is now Exit Game, not New Game: the submenu moves
 * Exit out of the way and brings it back instead. */
static void title_sound_open(struct CPUState* cpu, uint32_t address) {
    (void)cpu;
    (void)address;
    psx_mod_write_byte(title_item(0) + TI_ENABLED, 0);
    const uint32_t exit_item = title_item(TITLE_EXIT_SLOT);
    psx_mod_write_byte(exit_item + TI_STATE, TI_DOWN);
    psx_mod_write_byte(exit_item + TI_TARGET_POS, 128);
}

static void title_sound_close(struct CPUState* cpu, uint32_t address) {
    (void)cpu;
    (void)address;
    for (uint32_t i = 0; i < 8; i++)
        psx_mod_write_byte(title_item(TITLE_EXIT_SLOT) + i,
                           psx_mod_read_byte(title_item(0) + i));
    psx_mod_write_byte(title_item(0) + TI_ENABLED, 0);
}

/* -- the "Exit Game" label -- */

/* VRAM areas (16-bit words; labels are 4-bit, four pixels per word). The
 * sums are of the stock images, so only those are read or replaced. */
typedef struct { int x, y, w, h; uint32_t sum; } VArea;
static const VArea k_new_game  = { 544, 480, 32, 32, 0xDEF619DBu };
static const VArea k_vibration = { 608, 480, 32, 32, 0xA03F1616u };
static const VArea k_easy      = { 800, 480, 32, 32, 0xFC0034B6u };   /* slot 8 label */
static const VArea k_new_glow  = {   0, 448, 36, 64, 0xD81E68B4u };   /* 144 px wide */
static const VArea k_easy_glow = { 480,   0, 32, 64, 0x4E429816u };   /* slot 8 glow */

static uint32_t fnv(const uint16_t* w, int n) {
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < n; i++) {
        h = (h ^ (w[i] & 0xFFu)) * 0x01000193u;
        h = (h ^ (w[i] >> 8)) * 0x01000193u;
    }
    return h;
}

static int read_area(const VArea* a, uint16_t* out) {
    return gpu_host_vram_read(a->x, a->y, a->w, a->h, out) &&
           fnv(out, a->w * a->h) == a->sum;
}

typedef uint8_t Label[32][128];   /* palette index per pixel */

static void unpack(const uint16_t* w, int words, int rows, uint8_t* px) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < words * 4; x++)
            px[y * words * 4 + x] = (uint8_t)((w[y * words + x / 4] >> ((x & 3) * 4)) & 15);
}

static void pack(const uint8_t* px, int words, int rows, uint16_t* w) {
    memset(w, 0, (size_t)words * rows * 2u);
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < words * 4; x++)
            w[y * words + x / 4] |= (uint16_t)(px[y * words * 4 + x] << ((x & 3) * 4));
}

/* Copies columns sx0..sx1 of rows y0..y1, keeping the brighter pixel. */
static void blit(Label dst, int dx, const Label src, int sx0, int sx1, int y0, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = sx0; x <= sx1; x++)
            if (src[y][x] > dst[y][dx + x - sx0]) dst[y][dx + x - sx0] = src[y][x];
}

/* The labels' drop shadow: two dim pixels down-right of the letter body. */
static void add_shadow(Label g, int w) {
    Label s;
    memcpy(s, g, sizeof s);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < w; x++) {
            if (g[y][x]) continue;
            if (y >= 1 && x >= 1 && g[y - 1][x - 1] >= 4) s[y][x] = 2;
            else if (y >= 2 && x >= 2 && g[y - 2][x - 2] >= 4) s[y][x] = 1;
        }
    memcpy(g, s, sizeof s);
}

/* "Exit Game": a capital E stretched from the "Easy" label, a hand-drawn
 * small-capital X in the labels' palette, I and T from "Vibration" and
 * "Game" from "New Game". */
static void build_label(const Label ng, const Label vb, const Label ez, Label out) {
    static const int e_rows[22] = { 9, 10, 10, 11, 11, 11, 12, 12, 13, 14, 15,
                                    16, 17, 18, 19, 20, 20, 21, 21, 22, 22, 23 };
    static const int e_cols[15] = { 36, 37, 38, 39, 39, 40, 41, 42, 43, 43, 44, 45, 46, 47, 48 };
    static const char* const x_rows[12] = {
        "6CBB8...6BB7", ".7CB9...7A5.", "..8EC5.7A5..", "...AFD9A5...",
        "....DFF7....", "....7FFC....", "...6A7FF8...", "..6A5.8FE6..",
        ".6A5...AEC6.", "6A5....6CC9.", "8B5....5CC8.", "BBB9..7CBBB7",
    };
    Label g;
    memset(out, 0, sizeof(Label));
    int x = 16;

    memset(g, 0, sizeof g);
    for (int r = 0; r < 22; r++)
        for (int c = 0; c < 15; c++) {
            const uint8_t p = ez[e_rows[r]][e_cols[c]];
            g[2 + r][c] = p > 3 ? p : 0;
        }
    add_shadow(g, 20);
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 20; c++)
            if (g[r][c]) out[r][x + c] = g[r][c];
    x += 15;

    memset(g, 0, sizeof g);
    for (int r = 0; r < 12; r++)
        for (int c = 0; c < 12; c++) {
            const char ch = x_rows[r][c];
            g[12 + r][c] = (uint8_t)(ch == '.' ? 0 : ch <= '9' ? ch - '0' : ch - 'A' + 10);
        }
    add_shadow(g, 16);
    blit(out, x, (const uint8_t(*)[128])g, 0, 15, 0, 31);
    x += 13;

    blit(out, x, vb, 36, 42, 11, 26);   /* I */
    x += 7;
    blit(out, x, vb, 71, 78, 11, 26);   /* T */
    x += 14;
    blit(out, x, ng, 61, 107, 0, 31);   /* Game */
}

static int s_label_ready;
static uint16_t s_label_words[32 * 32];
static uint16_t s_glow_words[32 * 64];

static int title_resident(void) {
    return psx_mod_read_word(TITLE_SIG0_ADDR) == TITLE_SIG0 &&
           psx_mod_read_word(TITLE_SIG1_ADDR) == TITLE_SIG1;
}

static void prepare_label(void) {
    static uint16_t w_ng[32 * 32], w_vb[32 * 32], w_ez[32 * 32], w_glow[36 * 64];
    if (!read_area(&k_new_game, w_ng) || !read_area(&k_vibration, w_vb) ||
        !read_area(&k_easy, w_ez) || !read_area(&k_new_glow, w_glow))
        return;
    static Label ng, vb, ez, out;
    unpack(w_ng, 32, 32, &ng[0][0]);
    unpack(w_vb, 32, 32, &vb[0][0]);
    unpack(w_ez, 32, 32, &ez[0][0]);
    build_label((const uint8_t(*)[128])ng, (const uint8_t(*)[128])vb,
                (const uint8_t(*)[128])ez, out);
    pack(&out[0][0], 32, 32, s_label_words);

    /* Slot 8's glow is drawn 8 px right of New Game's (outlineXys), so its
     * column c is New Game's c + 8. */
    static uint8_t glow[64][144], glow8[64][128];
    unpack(w_glow, 36, 64, &glow[0][0]);
    for (int y = 0; y < 64; y++)
        memcpy(glow8[y], &glow[y][8], 128);
    pack(&glow8[0][0], 32, 64, s_glow_words);
    s_label_ready = 1;
}

/* The title uploads its images each time it starts; repaint slot 8 then. */
static void title_tick(void) {
    static int phase;
    if (++phase < 4) return;
    phase = 0;
    if (!title_resident()) return;
    static uint16_t cur[32 * 64];
    if (gpu_host_vram_read(k_easy.x, k_easy.y, k_easy.w, k_easy.h, cur) &&
        fnv(cur, k_easy.w * k_easy.h) == k_easy.sum) {
        if (!s_label_ready) prepare_label();
        if (s_label_ready)
            gpu_host_vram_write(k_easy.x, k_easy.y, k_easy.w, k_easy.h, s_label_words);
    }
    if (s_label_ready &&
        gpu_host_vram_read(k_easy_glow.x, k_easy_glow.y, k_easy_glow.w, k_easy_glow.h, cur) &&
        fnv(cur, k_easy_glow.w * k_easy_glow.h) == k_easy_glow.sum)
        gpu_host_vram_write(k_easy_glow.x, k_easy_glow.y, k_easy_glow.w, k_easy_glow.h,
                            s_glow_words);
}

/* ---- registration -------------------------------------------------------- */

static void exitmenu_activate(void) {
    s_armed = -1;
    fprintf(stdout, "vagrantstory: exit options in the menus on\n");
    fflush(stdout);
}

static void exitmenu_restored(void) {
    s_armed = -1;
}

PSX_MOD_CONSTRUCTOR(vs_register_exitmenu_plugin) {
    static const char id[] = "vs.exitmenu";
    (void)psx_mod_register_activation_plugin(id, exitmenu_activate);
    (void)psx_mod_register_savestate_plugin(id, exitmenu_restored);
    (void)psx_mod_register_function_filter_plugin(id, RING_FUNC, ring_filter);
    (void)psx_mod_register_vblank_plugin(id, title_tick);
    (void)psx_mod_register_instruction_plugin(id, T_FADE_IN, T_FADE_IN_INSN, title_fade_in);
    (void)psx_mod_register_instruction_plugin(id, T_SELECT, T_SELECT_INSN, title_select);
    (void)psx_mod_register_instruction_plugin(id, T_WRAP, T_WRAP_INSN, title_wrap);
    (void)psx_mod_register_instruction_plugin(id, T_WRAPPED, T_WRAPPED_INSN, title_wrapped);
    (void)psx_mod_register_instruction_plugin(id, T_MOVED_DOWN, T_MOVED_DOWN_INSN,
                                              title_moved_down);
    (void)psx_mod_register_instruction_plugin(id, T_MOVED_UP, T_MOVED_UP_INSN, title_moved_up);
    (void)psx_mod_register_instruction_plugin(id, T_SOUND_OPEN, T_SOUND_OPEN_INSN,
                                              title_sound_open);
    (void)psx_mod_register_instruction_plugin(id, T_SOUND_CLOSE, T_SOUND_CLOSE_INSN,
                                              title_sound_close);
}
