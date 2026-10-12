/*
 * Vagrant Story button prompts.
 *
 * A small translucent bar at the bottom of the window that lists what each
 * face/shoulder/system button does in the current situation (field, battle
 * mode, menus), and a matching bar at the top for the runtime's F-key
 * shortcuts (save states, fast forward, scanlines). The symbols follow the device the player last touched:
 * keyboard keycaps (from keybinds.ini), PlayStation shapes for a PlayStation
 * pad, or A/B/X/Y for any other gamepad. Host-only: nothing here touches
 * guest RAM, VRAM or savestates.
 *
 * Backed by the "vs.enhancement.prompts" mod package (launcher Mods page).
 */
#include "mod_plugins.h"
#include "host_osd.h"
#include "host_keymap.h"
#include "psx_keybinds.h"
#include "psx_sdl.h"
#include "gpu.h"
#include "mdec.h"
#include "vs_prompts.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* font8x8_basic (public domain), the same table the runtime's OSD uses. */
static const uint8_t FONT8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x7F,0x00}, /* E */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x06,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x06,0x06,0x06,0x06,0x06,0x06,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x06,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0f,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

enum { DEV_KEYBOARD = 0, DEV_PAD_XBOX, DEV_PAD_PS };

typedef struct { int button; const char* label; } Prompt;

/* Controls per the game manual (normal mode / battle mode / menu screen).
 * An empty label pairs a button with the next one ("L1 R1 Camera"). */
static const Prompt k_field[] = {
    { PSX_KB_CIRCLE, "Act/Draw" }, { PSX_KB_CROSS, "Interact/Cancel" },
    { PSX_KB_SQUARE, "Jump" },     { PSX_KB_TRIANGLE, "Menu" },
    { PSX_KB_L1, "" },             { PSX_KB_R1, "Camera" },
    { PSX_KB_L2, "Quick cmd" },    { PSX_KB_R2, "Walk" },
    { PSX_KB_SELECT, "Zoom" },     { PSX_KB_START, "Look" },
};
static const Prompt k_battle[] = {
    { PSX_KB_CIRCLE, "Target/Attack" }, { PSX_KB_CROSS, "Sheathe" },
    { PSX_KB_SQUARE, "Jump" },  { PSX_KB_TRIANGLE, "Menu" },
    { PSX_KB_L1, "" },                  { PSX_KB_R1, "Camera" },
    { PSX_KB_R2, "Walk" },
    { PSX_KB_SELECT, "Zoom" },          { PSX_KB_START, "Look" },
};
static const Prompt k_menu[] = {
    { PSX_KB_CIRCLE, "OK" },  { PSX_KB_CROSS, "Back" },
    { PSX_KB_TRIANGLE, "Close" },
    { PSX_KB_L1, "" },        { PSX_KB_R1, "Switch menu" },
};

/* Title screen menus (TITLE.PRG): Circle or Start confirms, Cross backs out. */
static const Prompt k_title[] = {
    { PSX_KB_CIRCLE, "" }, { PSX_KB_START, "OK" },
    { PSX_KB_CROSS, "Back" },
};

static int      s_enabled = 0;
static int      s_show_controls = 1, s_show_hotkeys = 1;   /* mod options */
static int      s_device = DEV_KEYBOARD;
static uint16_t s_prev_kb = 0xFFFFu;
static int      s_key_sig = -1, s_built_dev = -1, s_built_ctx = -1;
static uint32_t s_px[VS_PROMPT_MAX_W * VS_PROMPT_H];
static uint32_t s_tight[VS_PROMPT_MAX_W * VS_PROMPT_H];
static int      s_w = 0;

/* ---- device detection ---------------------------------------------------- */

static const char* short_key(SDL_Scancode sc);
static int pad_kind(SDL_Gamepad* g) {
    switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return DEV_PAD_PS;
    default:
        return DEV_PAD_XBOX;
    }
}

/* Latch whichever device produced a new input most recently.
 * VS_PROMPTS_DEVICE=keyboard|xbox|ps pins the symbols (screenshots, testing). */
static void update_device(void) {
    static int forced = -2;
    if (forced == -2) {
        const char* e = getenv("VS_PROMPTS_DEVICE");
        forced = !e ? -1 : !strcmp(e, "ps") ? DEV_PAD_PS
               : !strcmp(e, "xbox") ? DEV_PAD_XBOX : !strcmp(e, "keyboard") ? DEV_KEYBOARD : -1;
    }
    if (forced >= 0) { s_device = forced; return; }
    const bool* keys = SDL_GetKeyboardState(NULL);
    uint16_t kb = keys ? psx_keybinds_pad_word((const uint8_t*)keys, 1) : 0xFFFFu;
    if (kb != 0xFFFFu && kb != s_prev_kb) s_device = DEV_KEYBOARD;
    s_prev_kb = kb;

    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        int active = 0;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !active; b++)
            active = SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && !active; a++) {
            int v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a);
            active = v > 16000 || v < -16000;
        }
        if (active) { s_device = pad_kind(g); break; }
    }
    SDL_free(ids);
}

/* For vs_glyphs.c: the device last used (0 keyboard, 1 Xbox-style pad,
 * 2 PlayStation pad), polled now, and one 8x8 font row set. */
int vs_prompts_device(void) {
    update_device();
    return s_device == DEV_PAD_PS ? 2 : s_device == DEV_PAD_XBOX ? 1 : 0;
}
const uint8_t* vs_prompts_font8x8(char c) {
    unsigned ch = (unsigned char)c;
    if (ch < 32 || ch > 126) ch = '?';
    return FONT8X8[ch - 32];
}
const char* vs_prompts_key_name(int psx_button) {
    return short_key(psx_keybinds_get_button(1, psx_button));
}

/* ---- drawing (ARGB8888, top-down) ----------------------------------------- */

#define COL_BG     0xA0101418u
#define COL_TEXT   0xFFE8E4D8u
#define COL_KEY    0xFFD8D4CCu
#define COL_KEYTX  0xFF202020u
#define COL_PILL   0xFF4A4E58u
#define COL_FACE   0xFF2A2D33u
#define GLYPH_H    12

static void put(int x, int y, uint32_t c) {
    if (x >= 0 && x < s_w && y >= 0 && y < VS_PROMPT_H)
        s_px[y * VS_PROMPT_MAX_W + x] = c;
}
static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put(x + i, y + j, c);
}
/* 8px glyph cell drawn with a 6px advance (the font's glyphs are <= 7 wide). */
static int text_w(const char* s) { return (int)strlen(s) * 6; }
static void draw_text(int x, int y, const char* s, uint32_t c) {
    for (; *s; s++, x += 6) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t* g = FONT8X8[ch - 32];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 7; b++)
                if (g[r] & (1u << b)) put(x + b, y + r, c);
    }
}
static void rbox(int x, int y, int w, int h, uint32_t c) {   /* 1px corner cut */
    fill(x + 1, y, w - 2, h, c);
    fill(x, y + 1, 1, h - 2, c);
    fill(x + w - 1, y + 1, 1, h - 2, c);
}
static void disc(int cx, int cy, int r, uint32_t c, int ring) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            int d = i * i + j * j;
            if (d <= r * r + r && (!ring || d >= (r - 1) * (r - 1) - 1)) put(cx + i, cy + j, c);
        }
}

static const char* short_key(SDL_Scancode sc) {
    static char buf[16];
    switch (sc) {
    case SDL_SCANCODE_RETURN:    return "Enter";
    case SDL_SCANCODE_RSHIFT:    return "RShift";
    case SDL_SCANCODE_LSHIFT:    return "LShift";
    case SDL_SCANCODE_RCTRL:     return "RCtrl";
    case SDL_SCANCODE_LCTRL:     return "LCtrl";
    case SDL_SCANCODE_SPACE:     return "Space";
    case SDL_SCANCODE_ESCAPE:    return "Esc";
    case SDL_SCANCODE_BACKSPACE: return "Bksp";
    case SDL_SCANCODE_UNKNOWN:   return "-";
    default: break;
    }
    const char* n = SDL_GetScancodeName(sc);
    snprintf(buf, sizeof buf, "%s", (n && *n) ? n : "?");
    return buf;
}

static const char* ps_name(int b) {
    switch (b) {
    case PSX_KB_L1: return "L1";
    case PSX_KB_R1: return "R1";
    case PSX_KB_L2: return "L2";
    case PSX_KB_R2: return "R2";
    case PSX_KB_SELECT: return "SELECT";
    case PSX_KB_START: return "START";
    default: return "?";
    }
}
/* Default input.ini mapping: Cross=A, Circle=B, Square=X, Triangle=Y. */
static const char* xb_name(int b) {
    switch (b) {
    case PSX_KB_CROSS: return "A";
    case PSX_KB_CIRCLE: return "B";
    case PSX_KB_SQUARE: return "X";
    case PSX_KB_TRIANGLE: return "Y";
    case PSX_KB_L1: return "LB";
    case PSX_KB_R1: return "RB";
    case PSX_KB_L2: return "LT";
    case PSX_KB_R2: return "RT";
    case PSX_KB_SELECT: return "View";
    case PSX_KB_START: return "Menu";
    default: return "?";
    }
}

static void ps_shape(int cx, int cy, int b) {
    switch (b) {
    case PSX_KB_CIRCLE:
        disc(cx, cy, 3, 0xFFFF6A6Au, 1);
        break;
    case PSX_KB_CROSS:
        for (int i = -3; i <= 3; i++) {
            put(cx + i, cy + i, 0xFF7FB4FFu);
            put(cx + i, cy - i, 0xFF7FB4FFu);
        }
        break;
    case PSX_KB_SQUARE:
        fill(cx - 3, cy - 3, 7, 1, 0xFFF59BD2u);
        fill(cx - 3, cy + 3, 7, 1, 0xFFF59BD2u);
        fill(cx - 3, cy - 3, 1, 7, 0xFFF59BD2u);
        fill(cx + 3, cy - 3, 1, 7, 0xFFF59BD2u);
        break;
    default:   /* triangle */
        for (int r = 0; r < 6; r++) {
            put(cx - r / 2, cy - 3 + r, 0xFF4FD6B0u);
            put(cx + r / 2, cy - 3 + r, 0xFF4FD6B0u);
        }
        fill(cx - 3, cy + 2, 7, 1, 0xFF4FD6B0u);
        break;
    }
}

/* Draw (or with dry=1 only measure) one button glyph; returns its width. */
static int glyph(int x, int y, int b, int dev, int dry) {
    if (dev == DEV_KEYBOARD) {
        const char* k = short_key(psx_keybinds_get_button(1, b));
        int w = text_w(k) + 5;
        if (!dry) {
            rbox(x, y, w, GLYPH_H, COL_KEY);
            draw_text(x + 3, y + 2, k, COL_KEYTX);
        }
        return w;
    }
    int face = b == PSX_KB_CROSS || b == PSX_KB_CIRCLE ||
               b == PSX_KB_SQUARE || b == PSX_KB_TRIANGLE;
    if (face) {
        if (dry) return GLYPH_H;
        if (dev == DEV_PAD_PS) {
            disc(x + 6, y + 6, 6, COL_FACE, 0);
            ps_shape(x + 6, y + 6, b);
        } else {
            static const uint32_t col[4] = { 0xFF4CB848u, 0xFFE0473Cu, 0xFF3B7FE0u, 0xFFF0C030u };
            int idx = b == PSX_KB_CROSS ? 0 : b == PSX_KB_CIRCLE ? 1 : b == PSX_KB_SQUARE ? 2 : 3;
            disc(x + 6, y + 6, 6, col[idx], 0);
            draw_text(x + 3, y + 2, xb_name(b), 0xFF101010u);
        }
        return GLYPH_H;
    }
    const char* t = dev == DEV_PAD_PS ? ps_name(b) : xb_name(b);
    int w = text_w(t) + 5;
    if (!dry) {
        rbox(x, y, w, GLYPH_H, COL_PILL);
        draw_text(x + 3, y + 2, t, COL_TEXT);
    }
    return w;
}

static int layout(const Prompt* p, int n, int dev, int dry) {
    int x = 6;
    for (int i = 0; i < n; i++) {
        x += glyph(x, 3, p[i].button, dev, dry);
        if (p[i].label[0]) {
            x += 3;
            if (!dry) draw_text(x, 5, p[i].label, COL_TEXT);
            x += text_w(p[i].label) + 9;
        } else {
            x += 2;
        }
    }
    return x - 3;
}

static void build(int ctx) {
    const Prompt* p = k_field;
    int n = (int)(sizeof k_field / sizeof *k_field);
    if (ctx == VS_CTX_BATTLE) { p = k_battle; n = (int)(sizeof k_battle / sizeof *k_battle); }
    if (ctx == VS_CTX_MENU)   { p = k_menu;   n = (int)(sizeof k_menu / sizeof *k_menu); }
    if (ctx == VS_CTX_TITLE)  { p = k_title;  n = (int)(sizeof k_title / sizeof *k_title); }
    s_w = VS_PROMPT_MAX_W;
    int w = layout(p, n, s_device, 1);
    s_w = w < VS_PROMPT_MAX_W ? w : VS_PROMPT_MAX_W;
    memset(s_px, 0, sizeof s_px);
    rbox(0, 0, s_w, VS_PROMPT_H, COL_BG);
    layout(p, n, s_device, 0);
    /* The presenter reads rows of exactly w pixels. */
    for (int y = 0; y < VS_PROMPT_H; y++)
        memcpy(&s_tight[y * s_w], &s_px[y * VS_PROMPT_MAX_W], (size_t)s_w * 4u);
}

static int key_signature(void) {
    int sig = 0;
    for (int b = PSX_KB_CROSS; b <= PSX_KB_SELECT; b++)
        sig = sig * 31 + (int)psx_keybinds_get_button(1, b);
    return sig;
}

static int s_bottom_shown = 0;   /* the bottom bar's last answer */
static int prompt_image_impl(const uint32_t** pixels, int* w, int* h);
static int prompt_image(const uint32_t** pixels, int* w, int* h) {
    s_bottom_shown = prompt_image_impl(pixels, w, h);   /* also gates the top bar */
    return s_show_controls ? s_bottom_shown : 0;
}
static int prompt_image_impl(const uint32_t** pixels, int* w, int* h) {
    if (!s_enabled) return 0;
    if (gpu_display_is_depth24() || mdec_recently_active(30)) return 0;   /* movies */
    /* Debounce: a state must hold for a few presents before the bar follows
     * it, so one-frame values during room loads never flash the wrong set. */
    static int shown_ctx = VS_CTX_HIDDEN, pending_ctx = VS_CTX_HIDDEN, pending_n = 0;
    int raw = vs_prompts_context();
    if (raw == pending_ctx) {
        if (pending_n < 8) pending_n++;
    } else {
        pending_ctx = raw;
        pending_n = 1;
    }
    if (pending_n >= 8) shown_ctx = pending_ctx;
    int ctx = shown_ctx;
    if (ctx == VS_CTX_HIDDEN) return 0;
    update_device();
    int sig = key_signature();
    if (ctx != s_built_ctx || s_device != s_built_dev || sig != s_key_sig) {
        build(ctx);
        s_built_ctx = ctx;
        s_built_dev = s_device;
        s_key_sig = sig;
    }
    *pixels = s_tight;
    *w = s_w;
    *h = VS_PROMPT_H;
    return 1;
}

/* ---- top bar: runtime F-key shortcuts ------------------------------------- */

typedef struct { HostKeymapAction action; const char* label; } HotkeyPrompt;
static const HotkeyPrompt k_hotkeys[] = {
    { HOST_KEYMAP_SAVE_STATE_MENU, "Save states" },
    { HOST_KEYMAP_TURBO_TOGGLE,    "Fast forward" },
    { HOST_KEYMAP_SCANLINES,       "Scanlines" },
};
static uint32_t s_top[VS_PROMPT_MAX_W * VS_PROMPT_H];
static int      s_top_w = 0;
static char     s_top_sig[96] = "";

static int hotkey_layout(char keys[][16], int dry) {
    int x = 6;
    for (int i = 0; i < (int)(sizeof k_hotkeys / sizeof *k_hotkeys); i++) {
        if (!keys[i][0]) continue;   /* unbound */
        int w = text_w(keys[i]) + 5;
        if (!dry) {
            rbox(x, 3, w, GLYPH_H, COL_KEY);
            draw_text(x + 3, 5, keys[i], COL_KEYTX);
        }
        x += w + 3;
        if (!dry) draw_text(x, 5, k_hotkeys[i].label, COL_TEXT);
        x += text_w(k_hotkeys[i].label) + 9;
    }
    return x - 3;
}

static int top_image(const uint32_t** pixels, int* w, int* h) {
    if (!s_enabled) return 0;
    if (gpu_display_is_depth24() || mdec_recently_active(30)) return 0;   /* movies */
    if (!s_show_hotkeys || !s_bottom_shown) return 0;   /* in the same scenes as the bottom bar */
    enum { N = (int)(sizeof k_hotkeys / sizeof *k_hotkeys) };
    char keys[N][16], sig[96] = "";
    for (int i = 0; i < N; i++) {
        host_keymap_label(k_hotkeys[i].action, keys[i], sizeof keys[i]);
        strncat(sig, keys[i], sizeof sig - strlen(sig) - 2);
        strcat(sig, "|");
    }
    if (strcmp(sig, s_top_sig) != 0 || s_top_w == 0) {
        int tw = hotkey_layout(keys, 1);
        if (tw > VS_PROMPT_MAX_W) tw = VS_PROMPT_MAX_W;
        memset(s_px, 0, sizeof s_px);
        int saved_w = s_w;
        s_w = tw;   /* rbox/put clip to s_w */
        rbox(0, 0, tw, VS_PROMPT_H, COL_BG);
        hotkey_layout(keys, 0);
        s_w = saved_w;
        for (int y = 0; y < VS_PROMPT_H; y++)
            memcpy(&s_top[y * tw], &s_px[y * VS_PROMPT_MAX_W], (size_t)tw * 4u);
        s_top_w = tw;
        snprintf(s_top_sig, sizeof s_top_sig, "%s", sig);
    }
    *pixels = s_top;
    *w = s_top_w;
    *h = VS_PROMPT_H;
    return 1;
}

static int option_on(const char* id) {
    char v[16] = "";
    if (!psx_mod_current_option_value(id, v, sizeof v) || !v[0]) return 1;
    return strcmp(v, "false") != 0;
}

static void vs_prompts_activate(void) {
    s_enabled = 1;
    s_show_controls = option_on("controls_bar");
    s_show_hotkeys = option_on("hotkey_bar");
    const int game_icons = option_on("game_icons");
    vs_glyphs_set_enabled(game_icons);
    fprintf(stdout, "vagrantstory: button prompts on (button bar %s, F-key bar %s, game icons %s)\n",
            s_show_controls ? "on" : "off", s_show_hotkeys ? "on" : "off",
            game_icons ? "on" : "off");
    fflush(stdout);
    s_built_ctx = -1;
    host_osd_set_prompt_provider(prompt_image);
    host_osd_set_top_provider(top_image);
}

PSX_MOD_CONSTRUCTOR(vs_register_prompts_plugin) {
    (void)psx_mod_register_activation_plugin("vs.prompts", vs_prompts_activate);
}
