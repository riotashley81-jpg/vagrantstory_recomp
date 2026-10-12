#pragma once
/* Vagrant Story button-prompt bar (see vs_prompts.c). */
#define VS_PROMPT_MAX_W 960
#define VS_PROMPT_H     18

enum { VS_CTX_HIDDEN = 0, VS_CTX_FIELD, VS_CTX_BATTLE, VS_CTX_MENU, VS_CTX_TITLE };

/* 1 while BATTLE.PRG (the field/battle engine) is the resident overlay. */
int vs_battle_resident(void);

/* 1 while the 3D field is showing with no menu open (widescreen gate). */
int vs_wide_world_scene(void);
void vs_wide_world_tick(void);   /* once per guest VBlank */

/* Which prompt set fits the current game state (vs_game_state.c). */
int vs_prompts_context(void);

/* Shared with vs_glyphs.c (in-game button icons). */
#include <stdint.h>
int vs_prompts_device(void);               /* 0 keyboard, 1 Xbox-style, 2 PlayStation */
const uint8_t* vs_prompts_font8x8(char c); /* 8 rows, bit 0 = leftmost pixel */
const char* vs_prompts_key_name(int psx_button);   /* keyboard key for a PSX_KB_* */
void vs_glyphs_set_enabled(int on);       /* vs_glyphs.c */
