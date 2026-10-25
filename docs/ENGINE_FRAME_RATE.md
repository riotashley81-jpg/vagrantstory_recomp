# Engine-level 60 fps for Vagrant Story (plan)

Goal: replace Smooth motion (pixel/primitive guessing on the render thread)
with true in-between frames drawn by the game's own code, with the camera,
characters and animation placed halfway between two 30 fps logic ticks.

## The big shortcut: psxrecomp already has the machinery

psxrecomp ships **render passes** (`psxrecomp/docs/RENDER_PASSES.md`,
`FRAME_RATE.md`, `runtime/include/render_pass_motion.h`). A trusted game
plugin can, between two logic ticks:

- run the game's own draw section again (`psx_mod_run_guest_span`) inside a
  sandbox: guest time frozen, SPU/CD/timer stores dropped, and **all of RAM,
  scratchpad, CPU/GTE, GPU and VRAM restored bit for bit afterwards** -- so
  "drawing twice per tick has no side effects" is guaranteed by the runtime,
  not something we have to prove function by function;
- blend what the draw code reads (`psx_motion_track/apply`: PsyQ MATRIX with
  slerp, vectors, 12-bit angles, scalars), with per-tick limits so cuts and
  teleports snap instead of smearing;
- present the pass images between the game's own frames (FLIP source, HOLD
  blend), with a per-frame host-time budget that sheds passes on a slow PC.

Vigilante 8: 2nd Offense is the worked example upstream
(`v82_frame_rate_plugin.cpp`). What is left for VS is the game-specific part.

## What the decomp (rood-reverse, `Research/rood-reverse`) tells us

Field, battles and in-engine cutscenes all run in one loop in BATTLE.PRG
(`src/BATTLE/BATTLE.PRG/146C.c`, around line 7600-8210):

1. input and camera update per `_cameraMode` (0x800F18F0); camera state in
   `vs_battle_cameraCurrentSpherical` (0x800F19D0), get/set position and
   look-at at 0x8007ABEC..0x8007AC6C, transitions 0x8006EF5C/0x8006F204;
2. map draw: `func_8008EC48`, `func_8008AC78`, `func_8008B28C`;
3. `func_800AEF94(&vs_scratch.viewMatrix)`: builds the camera matrix and, for
   the 17 skeletal actors in `D_800F4538[]`, **steps their animation and
   computes the bone matrices** (`func_800AAE9C`, `func_800AF6E8`, ...);
4. `func_800941FC`: actor labels/markers; effects, HUD, screen transitions;
5. `func_8007629C(ot)`: flip `vs_main_frameBuf`, `DrawSync`,
   `vs_main_gametimeUpdate(vs_gametime_tickspeed)` (the VSync wait;
   tickspeed 2 = 30 fps, 4 in some modes), `PutDispEnv`, `PutDrawEnv`,
   `DrawOTag`.

Actor list: `vs_battle_actors` (0x800F1928). Logic tick: `vs_main_gametime`.

## Milestones

1. **Pass plumbing, no blending.** Plugin in `src/vs_frame_rate_mod.c` +
   mod package (off by default). Instruction hook at the start of the draw
   section to record registers; pass point at the entry of `func_8007629C`
   (decode DISPENV/DRAWENV to pick PENDING vs SHOWN). One pass per frame that
   replays the span unblended. Done when `render_pass_stats` shows passes
   with 0 span failures and `PSX_RENDER_PASS_VERIFY=1` gives
   `verify_mismatch` 0 over the opening cutscene and field play.
2. **Camera.** Blend the camera the draw code reads (the spherical/position
   state, or `vs_scratch.viewMatrix` after it is built). Smooth pans at 60.
3. **Characters.** Blend actor root transforms and skeleton poses. Animation
   is stepped inside `func_800AEF94`, so inside a pass either hold the step
   at half or blend the computed bone matrices (PSX_MOTION_ROTATION + VECTOR
   per bone). Measure motion per tick first to set the limits.
4. **The rest.** Map objects/doors, particles and spell effects (track or
   leave at the newer tick), the depth-of-field layer, widescreen margins,
   cutscene cuts (limits), menus/title/FMV untouched (no passes there).
5. **Budget and switch-over.** Passes run on the emulation thread (the
   opening cutscene already uses ~58-64% of a frame on the test PC; a pass
   replays the draw part only). Measure, then turn Smooth motion off for VS
   when the plugin is active and expose one launcher option.

Each milestone is reversible and lands behind the mod toggle; Smooth motion
stays available until milestone 5.
