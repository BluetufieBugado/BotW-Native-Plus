# Launcher

`BotWLauncher.exe` is a small Win32 program next to `BotWRecomp.exe`. It picks the game
folder, starts the game, and manages controls, save sets, and mods. It does not run any game
code itself. Build it with the rest of the project (see the [README](../README.md#launcher)).

## Main window

One row for the game, a toolbar, and a status line.

* **Game.** The icon is `meta\iconTex.tga` and the title is `longname_en` from
  `meta\meta.xml` in the game folder. Above the title is the platform and title id. Below it
  is one line: update version, region, renderer, Fullscreen when it is on, and how many mods
  are on (or Mods off).
* **Play.** Starts the game. Enter presses it. With no game folder set it reads
  **Choose game folder** and opens the folder picker. While the game runs it reads
  **Running** and is disabled.
* **Toolbar.** Settings, Controls, Saves, Mods on the left. Open user folder and Open log on
  the right.
* **Status line.** The last thing that happened, with a dot: green for fine, amber for a
  warning, red for an error.

Every dialog closes with Esc. Focus rings show once Tab or an arrow key is used.

## Settings

| Field | Meaning | Ini key |
| --- | --- | --- |
| Game folder | Folder with `code\`, `content\`, `meta\` from a v1.5.0 dump. | `game` |
| Game program | `BotWRecomp.exe`. Usually next to the launcher. | `exe` |
| Saves and logs | User data folder, passed as `BOTW_USER_ROOT`. Left empty, it is set to `%APPDATA%\BotWRecomp`. | `user` |
| Time limit | Seconds before the game closes. 0 means no limit. | `seconds` |
| Start in fullscreen | Passes `--fullscreen`. | `fullscreen` |

Save rejects paths that contain a double quote. Play checks that the folder has
`code\U-King.rpx` and asks before launching without it.

## What Play runs

```
BotWRecomp.exe --game <game folder> --renderer vulkan [--seconds N] [--fullscreen] [--no-mods]
```

`--seconds` is added only for N > 0. `--no-mods` is added when **Launch without mods** is
checked in the Mods dialog. Game builds without mod support ignore it.

The environment gets `BOTW_USER_ROOT=<user data>`, and `PATH` starts with
`C:\msys64\ucrt64\bin` (when it exists) and the game program's folder, so the SDL and MinGW
DLLs load. The working directory is the game program's folder.

## Controls

A table of every input with its keyboard key and controller button. Click a Keyboard or
Controller cell (or select a row and use Set key / Set controller button), then press the new
key or button. Backspace clears it, Esc keeps it. Changes go to `<user data>\controls.ini`
right away. Reset to defaults asks first, then writes the built-in map. The game reads the
file at startup.

## Saves

The live save is `<user data>\save\80000001\`. The list shows the save sets in
`<user data>\save-sets\`.

* **Import save** takes a folder or a `.zip`. Dropping one on the dialog does the same. The
  current save is backed up first.
* **Switch to** (or double-click) installs the selected set, after a backup.
* **Delete** removes the selected set.
* **Migrate old save** copies `save\2147483649` from older builds onto `save\80000001`.
* **Open saves folder** and **Refresh**.

Import, Switch to, Delete, and Migrate are disabled while `BotWRecomp.exe` runs.

## Mods

Each mod is a folder in `<user data>\mods\`:

```
mods\
  MyMod\
    rules.txt      optional
    content\       files that replace game content
    aoc\           files that replace DLC content
```

`rules.txt` is optional and uses the Cemu graphic pack layout. Only `[Definition]` is read:

```
[Definition]
name = Longer draw distance
description = Pushes object and grass fade-out further away.
version = 2
```

Without it the folder name is shown. A mod with neither `content\` nor `aoc\` is listed with
a note that it changes nothing.

State is in `<user data>\mods.ini`:

```
[Enabled]
DrawDistance=1
CameraTweaks=1
HudScale=0

[Order]
order=CameraTweaks,DrawDistance,HudScale
```

* A folder missing from `[Enabled]` is off.
* Mods load in list order. When two change the same file, the later one wins.
* Without `[Order]` the order is alphabetical by folder name. Move up and Move down write it.
* Other sections, comments, and keys for folders that are gone are kept.

The checkbox in front of each mod turns it on or off and saves at once. **Launch without
mods** keeps the list as it is and passes `--no-mods` for every launch until it is unchecked.

## Log

Open log shows the last 250 lines of `<user data>\runtime.log`. Open in editor opens the
whole file.

## BotWLauncher.ini

Next to the exe. One `key=value` per line:

| Key | Values |
| --- | --- |
| `game` | game folder |
| `exe` | path to `BotWRecomp.exe` |
| `user` | user data folder, may be empty |
| `seconds` | 0 or more |
| `fullscreen` | 0 or 1 |
| `no_mods` | 0 or 1, Launch without mods |
| `theme` | `system` (default), `light`, or `dark` |

Unknown lines are kept when the launcher writes the file.

## Look

Segoe UI, an 8 px grid, one muted green for Play and selection, and plain flat buttons for
everything else. Light and dark follow the Windows app setting unless `theme` says otherwise.
High contrast uses the system colors. The window handles per-monitor DPI changes.

## Tests

`launcher/launcher_data_test.cpp` checks the `meta.xml` and TGA readers and `mods.ini`
handling. It has no Win32 code:

```bash
g++ -std=c++20 -Ilauncher launcher/launcher_data_test.cpp launcher/game_info.cpp launcher/mods.cpp -o launcher_data_test
./launcher_data_test
```
