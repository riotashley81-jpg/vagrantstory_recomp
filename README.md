# Vagrant Story Recompiled (PC)

An unofficial native PC build of **Vagrant Story** (PlayStation, USA,
SLUS-01040), made by statically recompiling the game with
[psxrecomp](https://github.com/mstan/psxrecomp) and the
[recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui) launcher.

**This repository contains no game data.** There is no disc image, no
executable, no art, audio or text from the game, and no code translated from
it. To build and play you need your own legally obtained copy of the game.
The recompiler reads your disc and writes the game code (`generated/`)
on your machine only. That folder is ignored by git, so do not commit it.

## Playing (release download)

The [Releases](../../releases) page has a Windows build that also carries
none of the game's code: it reads Vagrant Story's code from your disc when
you play, runs it through psxrecomp's interpreter, and compiles the busy
parts in the background with the bundled TinyCC (cached for next time).

1. Unzip it anywhere.
2. Run `VagrantStory_Recompiled.exe` and point the launcher at your own
   dump of Vagrant Story (USA), SLUS-01040 (`.cue` + `.bin`).
3. Adjust the extras in the launcher's Mods page (all on by default;
   widescreen defaults to 16:9).

Vagrant Story is © Square Enix. This project is not affiliated with or
endorsed by Square Enix.

## What it adds

All of these are mod packages under `mods/preloaded/packages`, toggled in the
launcher's Mods page:

- **Widescreen** (16:9, 16:10, 21:9 or fit-to-window) for the 3D field and
  battles, with the HUD kept at 4:3. The depth-of-field blur and cutscene-cut
  ghosting reach the side margins too. Menus and the title screen stay 4:3.
- **Button prompt bar**: a small bar at the bottom of the screen showing
  what each button does in the current context, and one at the top listing
  the F-key shortcuts (save states, fast forward, scanlines). The symbols
  switch between keyboard and controller (PlayStation or Xbox)
  automatically; the save-state menu follows the controller too. Either
  bar can be hidden in the mod's settings.
- **In-game button icons** follow your device too: the game's own icons in
  menus, the HUD and text become A/B/X/Y and LB/RB/LT/RT on an Xbox-style pad,
  or your bound keys on the keyboard (Button Prompts setting, on by default).
- **Right-stick camera**: the right stick rotates the camera like L1/R1.
  R3 still toggles first-person view; there the right stick (or D-pad)
  looks around, up is up, and the left stick returns to the normal camera.
- **Skippable boot logos**: Enter, Circle or Start skips the company logos.
- The Windows Volume Mixer lists the game by name with its icon.
- Keyboard default: Space is Jump.

Higher internal resolution, Smooth motion (frame generation) and the render
thread come from psxrecomp and are set in the launcher's Display settings.

## Layout

| Path | What it is |
|---|---|
| `game.toml` | Title config for the recompiler and runtime |
| `src/` | This port's runtime plugins (widescreen, prompts, stick camera, logo skip) |
| `mods/` | The mod packages that expose those features in the launcher |
| `patches/psxrecomp`, `patches/recomp-ui` | Changes to the two framework submodules |
| `scripts/apply_patches.*` | Applies those patches on a local branch |
| `assets/` | App icon (Ashley) |

## Building (Windows, MSVC)

Requirements: Visual Studio 2022 (C/C++ workload), CMake 3.25+ and Ninja,
Git for Windows, Python 3.10+, and PowerShell 7 (`pwsh`). Run all commands
from an *x64 Native Tools* prompt with Git's `bin` folder ahead of WSL on
`PATH`, so `bash` is Git Bash.

1. Clone with submodules and apply the patches:

   The framework has deep source paths, so enable long paths in Git first
   (`git config --global core.longpaths true`) or clone near a drive root.

   ```
   git clone --recursive https://github.com/<you>/<this-repo>.git
   cd <this-repo>
   pwsh scripts/apply_patches.ps1
   ```

2. Put your own dump somewhere (for example `disc/Vagrant Story (USA).cue`
   and its `.bin`; `disc/` is ignored by git) and set `disc` in `game.toml`
   to it. The digests in `game.toml` identify the expected dump
   (Redump "Vagrant Story (USA)", data track MD5
   `8531bc73ea7cd33a4c8e97bdf139ba9c`).

3. Generate the game code from your disc (writes `generated/`):

   ```
   python psxrecomp/psxrecomp_cli.py generate --config game.toml --project-root . --no-toolchain-download --disc "disc/Vagrant Story (USA).cue"
   ```

   `--no-toolchain-download` stops the CLI from fetching its own 1.5 GB
   clang toolchain; the MSVC environment is enough.

4. Configure and build:

   ```
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

5. Run `build/VagrantStory_Recompiled.exe` and point the launcher at your
   `.cue`. Saves and memory cards go where you set them in the launcher.

To build the release variant with no game code inside, skip step 3 and
configure with `-DPSXRECOMP_DISC_CODE=ON`. Copy the framework's
`overlay_toolchain/` (TinyCC, the emitters and a pinned embeddable Python)
next to the executable so it can compile from the disc at runtime.

See `psxrecomp/docs/GAME_PROJECT_SETUP.md` for the framework's own build
documentation.

## Credits

- [psxrecomp](https://github.com/mstan/psxrecomp) and
  [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui), the
  recompiler, runtime and launcher this builds on.
- [rood-reverse](https://github.com/ser-pounce/rood-reverse), the Vagrant
  Story decompilation, used for RAM addresses and symbols.
