# Mods

BotW Recompiled supports Cemu-style graphic packs and file replacement mods. When the game opens a
file, loads a texture, compiles a shader, or initializes memory, the runtime checks enabled mods in
load order and falls back to the game dump. Nothing in the game dump is modified.

## Where mods go

Mods live in the `mods` folder inside the user folder (`%APPDATA%\BotWRecomp` unless
`BOTW_USER_ROOT` points somewhere else):

```
%APPDATA%\BotWRecomp\
  mods.ini
  mods\
    BetterSprint\
      rules.txt
      patches.txt
      content\
        Actor\Pack\GameROMPlayer.sbactorpack
    Clarity\
      rules.txt
      3904895d1e2b3c4d_0000000000000079_ps.txt
    LinkleMod\
      Actor\
        Pack\GameROMPlayer.sbactorpack
      Model\
        ...
```

## Supported folder layouts

You can drop mods in using any of the common Breath of the Wild mod layouts:

1. **Standard Cemu graphic pack layout**: `<ModFolder>\content\...`, `<ModFolder>\aoc\...`, and an
   optional `<ModFolder>\rules.txt`.
2. **Direct content layout**: Recognized BotW top-level content folders (`Actor`, `Pack`, `Model`,
   `UI`, `Map`, `Sound`, `System`, `Message`, `Effect`, `Physics`, `Event`, `Voice`, `Terrain`,
   `Quest`, `Font`, `Movie`, `Shader`, `Layout`, `Anim`, `Demo`, `NavMesh`, `Rumble`) placed
   directly inside `<ModFolder>\` without a `content\` wrapper are automatically mapped into
   `/vol/content/...`.
3. **Single-child nested archive folders**: If extracting a `.zip` creates an extra wrapper folder
   (for example `mods\MyMod\MyMod\content\...` or `mods\MyMod\v1.2\rules.txt`), the loader
   automatically unwraps up to two single-child folders so the mod works without rearranging
   directories.
4. **DLC / BCML / UKMM layouts**: Both `aoc\0010\...` and `aoc\content\0010\...` are mapped to the
   DLC mount.
5. **Full Unicode / UTF-8 paths**: Folder names and filenames with non-ASCII characters are
   supported across the launcher and runtime.

## Installing and managing mods in BotWLauncher.exe

Open **Mods** from the launcher sidebar (or right-click the game in the list and choose **Manage
mods...**):

- **Install Mod (.zip / Folder)...**: Pick a `.zip` archive or an extracted mod folder. The
  launcher extracts or copies it into `%APPDATA%\BotWRecomp\mods\<ModName>`, enables it in
  `mods.ini`, and selects it in the list.
- **Drag and drop**: Drop any `.zip` mod archive or mod folder directly onto the launcher window to
  install and enable it immediately.
- **Enable / Disable & Priority**: Check or uncheck mods, or use **Move up** / **Move down** to
  change priority. Later mods in the list have higher priority and win file conflicts.
- **Preset selector**: When the selected mod defines `[Preset]` options in `rules.txt`, dropdown
  selectors appear in the details panel so you can pick presets per category.
- **Conflict & content summary**: The details panel shows file counts (`content`, `DLC`, `patches`,
  `shaders`, `texture rules`) and lists which enabled mods the selected mod overrides or is
  overridden by.

## Launcher Mods window

The **Mods** window in `BotWLauncher.exe` shows each mod's settings as drop-downs, one per preset
category, with the same conditions the game uses. See [launcher.md](launcher.md#mods).

## In-game F1 Mods tab

Press **F1** in game and switch to the **Mods** tab:

- **Enter / A**: Toggle the selected mod `[ON]` or `[OFF]`.
- **Left / Right**: Cycle active `[Preset]` options for the selected mod.
- **U / D** (or **X / Y** on controller): Move the selected mod earlier or later in load order.
- **R**: Reload the mod overlay, `[TextureRedefine]` rules, and data patches immediately
  (`mods::reload()`). Files that the game only reads once at boot (`Bootup.pack`, `TitleBG.pack`)
  still require a game restart to reload.

## rules.txt, Presets, Expressions, and TextureRedefine

`rules.txt` supports `[Definition]`, `[Default]`, `[Preset]`, and `[TextureRedefine]`:

```ini
[Definition]
titleIds = 00050000101C9400,00050000101C9500,00050000101C9300
name = Shadow Resolution
description = Scales shadow map resolution.
version = 4

[Default]
$shadowScale = 1.0

[Preset]
name = Medium (1x)
category = Shadows
default = 1
$shadowScale = 1.0

[Preset]
name = High (2x)
category = Shadows
$shadowScale = 2.0

[TextureRedefine]
width = 1024
height = 1024
formats = 0x005
overwriteWidth = floor($shadowScale * 1024)
overwriteHeight = floor($shadowScale * 1024)
```

- **Presets**: Single-category and multi-category (`category = ...`) presets are supported,
  including `default = 1` and `condition = ...` expressions referencing variables from earlier
  categories.
- **Expression evaluator**: Variable assignments and `[TextureRedefine]` / `patches.txt` values can
  use arithmetic (`+`, `-`, `*`, `/`, `%`), comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`), logical
  operators (`&&`, `||`, `!`), ternary `?:`, and functions (`floor`, `ceil`, `round`, `trunc`,
  `abs`, `sqrt`, `min`, `max`, `clamp`).
- **TextureRedefine**: `[TextureRedefine]` sections (`width`, `height`, `depth`, `formats`,
  `formatsExcluded`, `tileModes`, `tileModesExcluded`, `overwriteWidth`, `overwriteHeight`,
  `overwriteDepth`, `overwriteFormat`, `overwriteLodBias`, `overwriteAnisotropy`) are evaluated
  with the active preset variables and fed into `GraphicPack2` ahead of the built-in resolution
  pack.

## Memory, Data, and Entry-Point Patches (`patches.txt` / `patch_*.txt`)

Mods can include `patches.txt`, `patch_*.txt` or `patch_*.asm` in Cemu format (`[BotW_...]`
sections with `moduleMatches = 0x6267BFD0`, the checksum Cemu uses for v1.5.0 / v208;
`0x6fd41a61` is accepted too):

```ini
[BotW_CustomValues]
moduleMatches = 0x6267BFD0

_customSpeed = 0x10500100
0x10500100 = .float ($speedMult * 1.5)
0x10500104 = .int 42
0x10500108 = .string "MOD"
0x02500100 = blr
```

Supported directives and instructions:
- **Symbols & origins**: `_symName = <expr>` and `origin = 0xXXXXXXXX` (with sequential write cursor
  advancement for unprefixed directive lines).
- **Data directives**: `.float <expr>`, `.double <expr>`, `.int` / `.uint` / `.word <expr>`,
  `.short` / `.ushort <expr>`, `.byte` / `.ubyte <expr>`, `.string "..."` / `.asciiz "..."`, and
  raw 32-bit hex values (`0xdeadbeef`).
- **PPC instructions & function entry hooks**:
  - Encodes `nop`, `blr`, `li rD, imm`, `lis rD, imm`, `b target`, and `bl target`.
  - Writing `blr` at a known guest function entry in `0x02000000..0x03FFFFFF` installs a no-op stub
    override in `dispatch`.
  - Writing `li r3, imm` followed by `blr` at a guest function entry installs a stub override that
    sets `r3 = imm` and returns.
  - Writing `b target` at a guest function entry redirects the dispatch table entry to `target`.
  - Reloading mods restores original guest memory bytes and dispatch entries before applying the
    new mod state.

## Custom Shader Replacements

Enabled mods can provide custom GLSL shader files in the mod root or a `shaders\` subfolder:
- Naming conventions: `<baseHash>_ps.txt`, `<baseHash>_vs.txt`, `<baseHash>_gs.txt`, or `.glsl`
  (with optional `0x` prefix and optional `<baseHash>_<auxHash>_ps.txt`).
- Active `$preset` variables are injected into custom GLSL sources (replacing `#define $var ...`
  placeholders, `$var` tokens, and prepending `#define var value` lines after `#version`).

## mods.ini

`mods.ini` sits in the user folder next to `settings.ini`:

```ini
[Enabled]
BetterSprint = 1
Clarity = 1

[Order]
order = Clarity,BetterSprint

[Presets]
Clarity = Serfrost
ShadowMod:Shadows = High (2x)
```

- `[Enabled]`: `1` (on) or `0` (off).
- `[Order]`: Comma-separated folder names from lowest priority to highest priority (later wins).
- `[Presets]`: Selected preset per mod (`ModFolder = PresetName`) or per category
  (`ModFolder:Category = PresetName`).

## Turning mods off for one run

```
BotWRecomp.exe --no-mods
```

Skips loading mods for that session without modifying `mods.ini`.

## Cemu community graphic packs

The packs from the Cemu community (github.com/cemu-project/cemu_graphic_packs) use the same
`rules.txt`, preset, shader and patch formats. In the launcher's Mods window, **Import Cemu zip...**
takes the pack archive you downloaded and **Import Cemu folder...** takes Cemu's own `graphicPacks`
folder. Only packs whose `rules.txt` lists a Breath of the Wild title id are copied into `mods\`,
and they are copied switched off. **Get packs online** opens the download page.

What can work here:

- Presets, `[TextureRedefine]` rules and custom shaders: supported.
- Data patches and function-entry patches (`blr`, `nop`, `li r3, N` + `blr`, `b`): supported.
- Patches in the middle of a function and code caves: skipped. Packs that depend on them, such as
  FPS++, do not work. 60 fps is built in, so leave the FPS packs off.

None of this has been tested in game yet.

## Unit tests

```
cmake -B build -DBOTW_BUILD_TESTS=ON
cmake --build build --target BotWModTests
./build/BotWModTests
```

Covers `mods.ini` and `rules.txt` parsing, `[Preset]` categories and conditions (including the
launcher's copy of the same rules, checked against the same pack), expression
evaluation, `[TextureRedefine]`, `patches.txt` data and function-entry overrides, custom shader
lookup, direct `Actor/` layouts and nested folder unwrapping, UTF-8 filenames, `move_order`,
`reload`, and `.zip` / folder installation.
