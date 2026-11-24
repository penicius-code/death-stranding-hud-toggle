# DSHudToggle

A hotkey that hides and shows the gameplay HUD in Death Stranding Director's Cut. Single-player only.
Version 1.1.0 | Tested in game on 2026-10-22 with the Steam version (`ds.exe`, build dsq 179) | Status: keyboard toggle working; controller toggle not working (see Known issues)

**Download:** [latest release](https://github.com/penicius-code/death-stranding-hud-toggle/releases/latest) (zip; install steps below)

> Unofficial fan-made modification. Not affiliated with, endorsed by or sponsored by Kojima Productions, 505 Games or Sony Interactive Entertainment. No game files are included; a legitimate copy of the game is required.

## The problem

The game has no option to switch the HUD off during play. Hiding it through a post-processing overlay did not work on this setup, and a memory table that was available only hid the compass and two markers.

## What I built

A small plugin (`DSHudToggle.asi`, about 850 lines of C) that hides and shows the whole gameplay HUD when you press Delete. It changes: one or two bytes at two places in the game's HUD drawing code, in the memory of the running game only. Nothing is written to the game's files, and the only file it creates is its own log.

## How it works

- The engine still contains a "draw HUD?" setting from the game it was first built for, with no menu entry in this game. One function answers that question and has exactly three callers: one HUD update and two HUD draw calls. Each caller skips its work when the answer is no.
- `draw` mode (default): in both draw calls, the conditional jump after the check becomes an unconditional one (1 byte each). The HUD keeps updating in the background and is simply not drawn.
- `gate` mode: the check itself is changed so it always answers no (4 bytes). This is the engine's own "HUD off" path and also pauses HUD logic while hidden.
- The three places are found by byte patterns that are unique in the game's executable, not by fixed addresses. The plugin verifies that both draw calls really call the check function before it touches anything.
- If the bytes it finds are not the expected ones (for example after a game update), it writes a line to the log and does nothing.
- If it finds one of its own changes already in place at start-up, it restores the original bytes first.
- The changes exist only in the memory of the running game, so they are gone when the game closes.
- The hotkey is polled only while the game window is in front.

```text
src/dshudtoggle.c     the plugin
DSHudToggle.ini       settings (goes next to the .asi)
build.cmd             build script (Zig 0.16.0)
tests/                unit tests for the controller code (no hardware needed)
tests/manual/         two checks that need a connected controller
```

## Requirements

| Requirement | Version | Link |
|---|---|---|
| Death Stranding Director's Cut (Steam) | build dsq 179 | |
| Ultimate ASI Loader | a current release | [ThirteenAG/Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) |
| Zig (to build) | 0.16.0 | [ziglang.org/download](https://ziglang.org/download/) |

Nothing is bundled. This repository contains source only; there is no pre-built plugin here.

## Build

1. Install Zig 0.16.0, or unpack it anywhere and set `ZIG` to the full path of `zig.exe`.
2. Run `build.cmd`. The result is `build\DSHudToggle.asi`.

The same command runs in this repository's CI on every push, together with the unit tests. The CI log prints the SHA-256 of the built file. On 2026-11-24 the CI build of 1.1.0 was byte-identical to the plugin that was tested in the game (SHA-256 `1b0deb5688d1cfe5bb4d18ae6157f55d12301f49fa6548aa02dbe74ddf300149`).

## Install

1. Install Ultimate ASI Loader into the game folder as described on its page.
2. Download the zip from the [latest release](https://github.com/penicius-code/death-stranding-hud-toggle/releases/latest) (or build the plugin yourself, see Build) and copy `DSHudToggle.asi` and `DSHudToggle.ini` into the game folder, next to `ds.exe`. Do this while the game is closed.
3. Start the game and press Delete during play. The HUD disappears; press again and it comes back.

To confirm it loaded: set `Log = 1` in the ini, start the game, and read `DSHudToggle.log` next to the plugin. It names the version and the three places it found.

## Uninstall / roll back

1. Close the game.
2. Delete `DSHudToggle.asi`, `DSHudToggle.ini` and `DSHudToggle.log` from the game folder.
3. Saves are not affected. The game's files were never changed, so there is nothing to restore.

## Configuration

`DSHudToggle.ini`, section `[Settings]`. Changes take effect at the next game start.

| Key | Default | Meaning |
|---|---|---|
| `Mode` | `draw` | `draw` = do not draw the HUD (it keeps updating); `gate` = the engine's own "HUD off" path |
| `Key` | `DELETE` | Keyboard key. Names such as `DELETE`, `F1`-`F24`, `HOME`, `PAUSE`, `NUMPAD0`-`9`, letters, digits, `MOUSE4`, `MOUSE5`, or a code like `0x2E`. Modifiers: `CTRL+`, `ALT+`, `SHIFT+` |
| `Controller` | `NONE` | Button combination, all held together (see Known issues) |
| `StartHidden` | `0` | `1` = hide the HUD as soon as the game starts |
| `Log` | `0` | `1` = write `DSHudToggle.log` |

## Compatibility

| Item | Status | Notes |
|---|---|---|
| Steam `ds.exe`, build dsq 179 | works | Keyboard toggle in `draw` mode confirmed in game. |
| Other game builds | unknown | If the byte patterns do not match, the plugin logs that and stays inactive. |
| Other `.asi` plugins | no overlap found | The changed bytes were compared with the places used by the fix plugin and loader installed on the test setup; none overlap. |

## Known issues

- **The controller toggle does not work inside the game.** The code reads Xbox-style controllers through XInput and DualSense / DualShock 4 controllers directly. In unit tests and in a separate test process it reads a connected DualSense correctly. Inside the running game the plugin never saw the controller, although it was connected. The cause was not found (a guess, not checked: something in the game process hides the device from enumeration). `Controller` is therefore `NONE` by default.
- `gate` mode passed the offline tests but was not compared with `draw` mode in game.
- Not yet checked in game: whether anything stays on screen while hidden (subtitles, markers, prompts).

## Engineering notes

**Problem.** A HUD toggle "that will not conflict with anything" was wanted, after an overlay-based attempt had failed.

**Diagnosis.** Reading the game's executable showed that the engine still carries a HUD on/off setting without a menu, and that a single function decides whether the HUD is drawn. Counting the callers of that function gave exactly three. That made a small, verifiable change possible instead of intercepting graphics or input calls.

**What was built to check it before it ran in the game.** A pattern self-test against the executable; the same toggle as an external script on a dummy process and on a simulated memory image; the plugin loaded into an unrelated process (it must disable itself) and into a test program (both modes, start hidden, a leftover change restored, unexpected bytes refused). All passed before the first install.

**What did not work.** Version 1.1.0 added a read-only reader for PlayStation controllers, because on the test setup the game read the controller in a way XInput could not see. Unit tests for the report formats passed (`tests/test_ps.c`, `tests/test_mute.c`). In the game the plugin's device scan found nothing, while the same scan code in a separate process found and read the controller. The keyboard toggle was kept and the controller option switched off by default.

**Rollback.** Remove three files from the game folder. Nothing on disk was changed, and the in-memory changes end with the game process.

## Changelog

See [CHANGELOG.md](CHANGELOG.md). Latest: 1.1.0 (2026-10-22): PlayStation controller reader added (not working in game, off by default).

## Credits and licence

Loaded by Ultimate ASI Loader by ThirteenAG. Built with Zig. Neither is included here.

MIT, see [LICENSE](LICENSE). The licence covers my code only. The source contains three short byte patterns used to locate code in the game's executable; no game files are included. DEATH STRANDING is a trademark of Sony Interactive Entertainment LLC.

Built with AI assistance (Claude); designed, tested and maintained by me.
