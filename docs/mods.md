# Mods

BotW Recompiled can load file replacement mods. A mod is a folder of game files. When the
game opens a file, the runtime checks the enabled mods first and falls back to the game dump.
Nothing in the game dump is changed.

**Status: untested in game.** The file index has a unit test (see below) and the code builds
for Windows, but no mod has been played through with this build yet. Please report what works
and what does not.

## Where mods go

Mods live in the `mods` folder inside the user folder. The user folder is
`%APPDATA%\BotWRecomp` unless `BOTW_USER_ROOT` points somewhere else.

```
%APPDATA%\BotWRecomp\
  mods.ini
  mods\
    BetterSprint\
      rules.txt
      content\
        Actor\Pack\GameROMPlayer.sbactorpack
    MoreArrows\
      rules.txt
      content\
        Pack\Bootup.pack
      aoc\
        0010\Pack\AocMainField.pack
    OldTest\
      content\
        ...
```

- `content\` replaces or adds files in the game's `content` folder. The path inside it is the
  same as in the game dump, so `content\Actor\Pack\GameROMPlayer.sbactorpack` replaces
  `content\Actor\Pack\GameROMPlayer.sbactorpack`.
- `aoc\` is optional and does the same for the DLC files. Both `aoc\0010\...` and the Cemu
  layout `aoc\content\0010\...` work.
- Paths are not case sensitive. `Content\actor\pack\...` matches the same file.
- A mod can also add new files and new folders. They show up in directory listings next to
  the game's own files.
- Stick to plain ASCII folder and file names for now. Other names may be skipped.

This is the same `content` and `aoc` layout Cemu graphic packs use, so most Cemu content mods
can be copied in as they are.

## rules.txt

`rules.txt` is optional. Only the `[Definition]` section is read, in Cemu graphic pack style:

```
[Definition]
titleIds = 00050000101C9400,00050000101C9500,00050000101C9300
name = Better Sprint
description = Faster sprint and less stamina use.
version = 3
```

`name` is shown in the log and in the menu. Without it the folder name is used.
`description` and `version` are optional. Note that in many Cemu packs `version` is the rules
file format version (often 3 to 7), not the mod's own version. If `titleIds` is there and does
not include the USA title (`101C9400`), a warning is written to the log, but the mod still
loads.

Any other sections (presets, `[TextureRedefine]`, and so on) are ignored.

## mods.ini

`mods.ini` sits in the user folder, next to `settings.ini`. It says which mods are on and in
what order:

```
[Enabled]
BetterSprint = 1
MoreArrows = 1
OldTest = 0

[Order]
order = MoreArrows,BetterSprint
```

- `[Enabled]`: the key is the mod's folder name and the value is `1` (on) or `0` (off).
  `true`, `yes`, and `on` also count as on. A mod with no line here is off. Folder names are
  matched without regard to case. If the same folder appears twice, the last line wins.
- `[Order]`: optional. `order` is a comma separated list of folder names. Later entries win,
  so in the example `BetterSprint` beats `MoreArrows` when both have the same file. Enabled
  mods that are not in the list load first (lowest priority), in alphabetical order.
- Without `[Order]`, enabled mods load in alphabetical order of their folder names, and the
  later one wins.
- Lines starting with `#` or `;` are comments.

If `mods.ini` does not exist, no mods are loaded.

## Turning mods off for one run

```
BotWRecomp.exe --no-mods
```

This skips the `mods` folder for that run. `mods.ini` is not changed.

## What the log says

At startup `runtime.log` lists the mods in load order, how many files each one has, how many
replace a game file and how many are new, and every file that more than one mod provides
(the first 50, then a total):

```
mods: 3 mod folders in C:\Users\you\AppData\Roaming\BotWRecomp\mods, 2 enabled
mod 1: More Arrows [MoreArrows]: 2 files (1 content, 1 aoc), 2 replace game files, 0 new
mod 2: Better Sprint [BetterSprint] version 3: 1 files (1 content, 0 aoc), 1 replace game files, 0 new
mods: 2 mods loaded, 3 overlay files, 3 replace a game file, 0 new
```

A mod that is enabled in `mods.ini` but has no folder gets a warning. On exit the log also
says how many mod files the game actually opened.

The F1 menu has a read-only **Mods** row that shows how many mods are loaded, or that mods are
off for this run.

## Other things to know

- Mods are read only. Saves still go to the normal save folder.
- The game is told the DLC is installed only when `0010\Pack\AocMainField.pack` exists, either
  in the game dump's `aoc` folder, in a mod, or in a `aoc` folder in the user folder. That user
  `aoc` folder is for a DLC dump, not a mod, so it is used even with `--no-mods`, and mods win
  over it.
- Changes to mods or `mods.ini` take effect the next time the game starts.

## Not supported yet

- Code patches: `patches.txt`, `patch_*.txt`, and Cemu assembly patches. They are counted in
  the log and skipped.
- Shader replacements (`*_ps.txt`, `*_vs.txt`, `*_gs.txt`) and graphic pack presets.
- Non-ASCII paths.
- Turning mods on or off from inside the game.

## Unit test

The index has a small test that builds a fake game and user folder in the temp directory:

```
cmake -B build -DBOTW_BUILD_TESTS=ON
cmake --build build --target BotWModTests
./build/BotWModTests
```

It covers `mods.ini` and `rules.txt` parsing, load order, which mod wins a conflict, case
insensitive lookup, merged directory listings, DLC detection, and `--no-mods`.
