# Deadstation Portable recomp SDK

The runtime and port layer for Deadstation Portable's recomp modules. A recomp module is one PSP game's code translated ahead of
time to C and compiled for the TrimUI Brick, run by Deadstation Portable in place of its emulator. Each module is its own
repository, with this one as a submodule (`sdk/`).

The translator and the runtime's base are [Nakagawa Recomp](https://github.com/Jstar269/nakagawa-recomp). GPL-3.0-or-later.

---

## AI Slop Section

### What every module follows

1. **A module, not a replacement.** Deadstation Portable stays running and hosts the module as a child process. MENU, the menu,
   Options and the Recomp Store are Deadstation Portable's; the module is the game.
2. **No game data in git.** No disc images, nothing extracted or decrypted from them, no translated code: `tools/prepare-game.sh`
   writes all of that to a work folder outside every repository.
3. **The disc is checked.** The manifest names the disc by its UMD ID; the game image the module builds from it on its first run
   must match the manifest's SHA-256.
4. **Whatever a module lets the player choose is a setting declared in its manifest,** shown in Options.
5. **Native.** Nothing simulated at run time that costs speed: the GE drawn on the Brick's GPU, sound mixed for the speaker.

### Layout

- `nakagawa-recomp/`: the translator (`tools/analyze.py`, `tools/codegen.py`), the disc and decrypt tools (`src/core`) and the
  runtime (`src/rt`), a submodule at a pin (`nakagawa.pin`). `patches/nakagawa/`: this SDK's changes to it, one patch per file,
  applied by `tools/prepare-nakagawa.sh`.
- `cmake/nakagawa.cmake`: the runtime as a static library with this SDK's backends, and a game linked against it.
- `runtime/`: the SDK's own runtime parts: HLE handlers Nakagawa hasn't got (`hle_sdk.c`), the GE worker, the game image builder,
  and the Brick's backend (`brick/`: Vulkan presenter, panel, ALSA sound, pad, the link to Deadstation Portable).
- `tools/`: `prepare-game.sh` (a disc to translated C), `promote_registers.py` (guest registers into C locals), `chdcd2iso.c` (a
  CD-type CHD of a PSP disc back to its ISO), `chdsha1.c`, `psp_iso.py`, `toolchain/tg5040.cmake`.

### A module's hooks (all optional, weak)

- `deadstation_module_setting(key, value)`: a setting from the manifest, at the start and when it changes.
- `sdk_module_frame()`: once a frame on the game's thread.
- `sdk_sprite_draw(SdkSpriteDraw *)`: each 2D draw, to move, extend or drop it (HUD and menu layout).
- `sdk_vplus_rows()`: rows shown above and below the PSP's 272 (a 4:3 picture).
