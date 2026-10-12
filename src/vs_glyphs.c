/*
 * Vagrant Story: in-game button icons that match the player's device.
 *
 * The game draws its button icons from the 4-bit font page it uploads to
 * VRAM at x=768: round face-button icons and L1/R1/L2/R2 caps used by the
 * menus and the battle HUD, and smaller face-button glyphs inside the text
 * font (dialogue, help lines). Once per VBlank this checks those three
 * areas; when they hold the game's own icons and the player is on an
 * Xbox-style pad or the keyboard, it repaints just the icons (A/B/X/Y and
 * LB/RB/LT/RT, or the bound keys) and puts the originals back for a
 * PlayStation pad. The rest of the page and all guest RAM are untouched;
 * a fresh upload or a savestate load is simply repainted again.
 *
 * Part of the "vs.enhancement.prompts" package ("game_icons" option).
 */
#include "mod_plugins.h"
#include "gpu.h"
#include "psx_keybinds.h"
#include "vs_prompts.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Areas of the font page, in VRAM halfwords (4 pixels each). */
typedef struct { int x, y, w, h; uint32_t orig_sum; } Area;
enum { AREA_ROUND = 0, AREA_CAPS, AREA_TEXT, AREA_N };
static const Area k_area[AREA_N] = {
    { 768, 128, 16, 16, 0xD984A22Au },   /* round icons: triangle, square, circle, cross */
    { 768, 145, 16, 12, 0xE8F8A4E9u },   /* caps: L1, R1, L2, R2 */
    { 850,  98, 12,  9, 0xB9EDC170u },   /* text-font glyphs: triangle, square, circle, cross */
};
#define AREA_MAX_WORDS (16 * 16)

/* The face-button order the icons sit in, and their shoulder caps. */
static const int k_face[4] = { PSX_KB_TRIANGLE, PSX_KB_SQUARE, PSX_KB_CIRCLE, PSX_KB_CROSS };
static const char* const k_face_xbox[4] = { "Y", "X", "B", "A" };
static const int k_shoulder[4] = { PSX_KB_L1, PSX_KB_R1, PSX_KB_L2, PSX_KB_R2 };
static const char* const k_shoulder_xbox[4] = { "LB", "RB", "LT", "RT" };
/* Round-icon letter colours: palette slots of the icon CLUT (VRAM 944,223),
 * which holds the PlayStation colours: 15 green (triangle), 14 pink (square),
 * 11 red (circle), 13 blue (cross), and 12, a pale yellow. Xbox: Y yellow,
 * X blue, B red, A green; the keyboard keeps each button's PlayStation colour. */
static const int k_round_xbox_ink[4] = { 12, 13, 11, 15 };
static const int k_round_ps_ink[4]   = { 15, 14, 11, 13 };
/* Text-font glyph boxes: first and last pixel column within AREA_TEXT. */
static const int k_text_col[4][2] = { { 1, 11 }, { 14, 22 }, { 26, 34 }, { 38, 46 } };

/* 3x5 capitals and digits for two-character labels; bit 2 = left. */
static const uint8_t FONT3X5[36][5] = {
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},   /* A-F */
    {3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},   /* G-L */
    {5,7,7,5,5},{6,5,5,5,5},{2,5,5,5,2},{6,5,6,4,4},{2,5,5,6,3},{6,5,6,5,5},   /* M-R */
    {3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},   /* S-X */
    {5,5,2,2,2},{7,1,2,4,7},                                                     /* Y-Z */
    {2,5,5,5,2},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},{5,5,7,1,1},{7,4,6,1,6},   /* 0-5 */
    {3,4,6,5,2},{7,1,2,2,2},{2,5,2,5,2},{2,5,3,1,6},                             /* 6-9 */
};

static int s_enabled = 0;
static int s_have_orig[AREA_N];
static uint16_t s_orig[AREA_N][AREA_MAX_WORDS];
static uint16_t s_ours[AREA_N][AREA_MAX_WORDS];
static uint32_t s_ours_sum[AREA_N];
static uint32_t s_written_sum[AREA_N];   /* last image this wrote, per area */
static char s_style[64] = "";   /* style the s_ours images were built for */

static uint32_t fnv(const uint16_t* w, int n) {
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < n; i++) {
        h = (h ^ (w[i] & 0xFFu)) * 0x01000193u;
        h = (h ^ (w[i] >> 8)) * 0x01000193u;
    }
    return h;
}

/* 4-bit pixel access within an area image (w halfwords per row). */
static int px_get(const uint16_t* img, int w, int col, int row) {
    return (img[row * w + (col >> 2)] >> ((col & 3) * 4)) & 15;
}
static void px_set(uint16_t* img, int w, int col, int row, int v) {
    if (col < 0 || row < 0 || col >= w * 4) return;
    uint16_t* p = &img[row * w + (col >> 2)];
    const int sh = (col & 3) * 4;
    *p = (uint16_t)((*p & ~(15u << sh)) | ((unsigned)v << sh));
}

/* Short label for a keyboard key: one character as is, else two. */
static void key_label(int psx_button, char out[3]) {
    const char* k = vs_prompts_key_name(psx_button);
    static const struct { const char* name; const char* abbr; } abbr[] = {
        { "Space", "SP" }, { "Enter", "EN" }, { "RShift", "RS" }, { "LShift", "LS" },
        { "RCtrl", "RC" }, { "LCtrl", "LC" }, { "Esc", "ES" }, { "Bksp", "BS" },
        { "Tab", "TB" },
    };
    for (size_t i = 0; i < sizeof abbr / sizeof *abbr; i++)
        if (!strcmp(k, abbr[i].name)) { snprintf(out, 3, "%s", abbr[i].abbr); return; }
    out[0] = (char)toupper((unsigned char)k[0]);
    out[1] = k[0] && k[1] ? (char)toupper((unsigned char)k[1]) : 0;
    out[2] = 0;
}

/* Draw a label into a box: one character in the 8x8 font, two in 3x5. */
static void draw_label(uint16_t* img, int w, int x0, int x1, int y0, int y1,
                       const char* s, int color) {
    const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    if (s[0] && !s[1] && bh >= 7) {   /* one character with room for 8x8 */
        const uint8_t* g = vs_prompts_font8x8(s[0]);
        int gl = 8, gr = -1;
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (1u << b)) { if (b < gl) gl = b; if (b > gr) gr = b; }
        if (gr < gl) return;
        const int gw = gr - gl + 1, gh = bh < 7 ? bh : 7;
        const int ox = x0 + (bw - gw) / 2, oy = y0 + (bh - gh) / 2;
        for (int r = 0; r < gh; r++)
            for (int b = gl; b <= gr; b++)
                if (g[r] & (1u << b)) px_set(img, w, ox + b - gl, oy + r, color);
        return;
    }
    const int tw = s[1] ? 7 : 3;
    const int ox = x0 + (bw - tw) / 2, oy = y0 + (bh - 5) / 2;
    for (int c = 0; c < 2 && s[c]; c++) {
        const char ch = s[c];
        const int idx = (ch >= 'A' && ch <= 'Z') ? ch - 'A'
                      : (ch >= '0' && ch <= '9') ? 26 + ch - '0' : -1;
        if (idx < 0) continue;
        for (int r = 0; r < 5; r++)
            for (int b = 0; b < 3; b++)
                if (FONT3X5[idx][r] & (4u >> b)) px_set(img, w, ox + c * 4 + b, oy + r, color);
    }
}

static void build(int device) {
    char face[4][3], shoulder[4][3];
    for (int i = 0; i < 4; i++) {
        if (device == 1) {
            snprintf(face[i], 3, "%s", k_face_xbox[i]);
            snprintf(shoulder[i], 3, "%s", k_shoulder_xbox[i]);
        } else {
            key_label(k_face[i], face[i]);
            key_label(k_shoulder[i], shoulder[i]);
        }
    }
    /* Round icons: a dark disc centre with a bright label. */
    const Area* a = &k_area[AREA_ROUND];
    memcpy(s_ours[AREA_ROUND], s_orig[AREA_ROUND], (size_t)a->w * a->h * 2u);
    for (int i = 0; i < 4; i++) {
        const int c0 = i * 16;
        for (int r = 4; r <= 10; r++)
            for (int c = c0 + 4; c <= c0 + 11; c++) px_set(s_ours[AREA_ROUND], a->w, c, r, 4);
        draw_label(s_ours[AREA_ROUND], a->w, c0 + 4, c0 + 11, 4, 10, face[i],
                   device == 1 ? k_round_xbox_ink[i] : k_round_ps_ink[i]);
    }
    /* Shoulder caps: the cap's own row shading under a new label. */
    a = &k_area[AREA_CAPS];
    memcpy(s_ours[AREA_CAPS], s_orig[AREA_CAPS], (size_t)a->w * a->h * 2u);
    static const int cap_bg[5] = { 6, 6, 4, 4, 3 };
    for (int i = 0; i < 4; i++) {
        const int c0 = i * 16;
        for (int r = 4; r <= 8; r++)
            for (int c = c0 + 2; c <= c0 + 12; c++) px_set(s_ours[AREA_CAPS], a->w, c, r, cap_bg[r - 4]);
        draw_label(s_ours[AREA_CAPS], a->w, c0 + 2, c0 + 12, 4, 8, shoulder[i], 12);
    }
    /* Text-font glyphs: transparent box, bright label. */
    a = &k_area[AREA_TEXT];
    memcpy(s_ours[AREA_TEXT], s_orig[AREA_TEXT], (size_t)a->w * a->h * 2u);
    for (int i = 0; i < 4; i++) {
        for (int r = 0; r < a->h; r++)
            for (int c = k_text_col[i][0]; c <= k_text_col[i][1]; c++) px_set(s_ours[AREA_TEXT], a->w, c, r, 0);
        draw_label(s_ours[AREA_TEXT], a->w, k_text_col[i][0], k_text_col[i][1], 0, a->h - 1, face[i], 15);
    }
    for (int k = 0; k < AREA_N; k++)
        s_ours_sum[k] = fnv(s_ours[k], k_area[k].w * k_area[k].h);
}

static void vs_glyphs_tick(void) {
    if (!s_enabled) return;
    static int phase = 0;
    if (++phase < 4) return;   /* a few times a second is plenty */
    phase = 0;

    uint16_t cur[AREA_N][AREA_MAX_WORDS];
    uint32_t sum[AREA_N];
    for (int k = 0; k < AREA_N; k++) {
        const Area* a = &k_area[k];
        if (!gpu_host_vram_read(a->x, a->y, a->w, a->h, cur[k])) return;
        sum[k] = fnv(cur[k], a->w * a->h);
        if (sum[k] == a->orig_sum && !s_have_orig[k]) {
            memcpy(s_orig[k], cur[k], (size_t)a->w * a->h * 2u);
            s_have_orig[k] = 1;
        }
    }
    for (int k = 0; k < AREA_N; k++)
        if (!s_have_orig[k]) return;   /* font page not seen yet */

    const int device = vs_prompts_device();
    char style[64];
    if (device == 2) {
        snprintf(style, sizeof style, "ps");
    } else if (device == 1) {
        snprintf(style, sizeof style, "xbox");
    } else {
        snprintf(style, sizeof style, "kb");
        for (int i = 0; i < 4; i++) {
            char l[3];
            key_label(k_face[i], l);
            strncat(style, l, sizeof style - strlen(style) - 1);
            key_label(k_shoulder[i], l);
            strncat(style, l, sizeof style - strlen(style) - 1);
        }
    }
    if (strcmp(style, s_style) != 0) {
        if (device != 2) build(device);
        snprintf(s_style, sizeof s_style, "%s", style);
    }

    for (int k = 0; k < AREA_N; k++) {
        const Area* a = &k_area[k];
        const uint16_t* target = device == 2 ? s_orig[k] : s_ours[k];
        const uint32_t target_sum = device == 2 ? a->orig_sum : s_ours_sum[k];
        if (sum[k] == target_sum) continue;
        /* Only repaint the game's own icons or an image we wrote; anything
         * else in that spot is the game's business. */
        if (sum[k] != a->orig_sum && sum[k] != s_written_sum[k]) continue;
        if (gpu_host_vram_write(a->x, a->y, a->w, a->h, target))
            s_written_sum[k] = target_sum;
    }
}

/* Called from the prompts plugin's activation with the "game_icons" option. */
void vs_glyphs_set_enabled(int on) {
    s_enabled = on;
    s_style[0] = 0;
}

PSX_MOD_CONSTRUCTOR(vs_register_glyphs_plugin) {
    (void)psx_mod_register_vblank_plugin("vs.prompts", vs_glyphs_tick);
}
