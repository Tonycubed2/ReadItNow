# Read It Now - AutoTurn

A patched build of **Read It Now** by **Shazdeh** for Skyrim Special Edition / Anniversary Edition.

Original mod: https://www.nexusmods.com/skyrimspecialedition/mods/168337
Original source: https://github.com/Shazdeh/Read-It-Now

All credit for Read It Now itself (the "Read" prompt when you pick up a book, the plugin and the idea) goes to Shazdeh.
This build fixes page turning and adds hands-free reading with **Voiced Books of Skyrim** (VBoS).

## What this build changes

**Page-turn fix**
- In the original, a book opened from the Read prompt was dropped into the world and disabled while the Book Menu was
  open, so the first spread showed but the pages would not turn.
- Now the book opens straight from your inventory, the same way clicking it in the Inventory menu does. No world copy,
  no "Take" button, nothing to hand back. The old drop-and-activate method is still available (`iOpenMethod = 1`),
  without the disable that broke it.
- The prompt text no longer points at a destroyed temporary string.

**Auto page turning with Voiced Books of Skyrim**
- While VBoS reads a book aloud, the pages turn by themselves when the narrator finishes the open spread.
- Works for any book you open, not only ones opened with the Read prompt.
- Uses per-word timing data for the VBoS recordings when it has it, and otherwise estimates the turn from the
  spread's share of the book's words.
- Paging back to a spread that was already read leaves it alone.

**Resume where you left off**
- Close a book mid-narration (to fight, say) and the narration pauses instead of stopping.
- Open the same book again and it continues from where it stopped, back on the pages you were on.
- Opening a different book drops the paused narration. After loading a save, books start from the beginning.

**Read while walking**
- While a book is being narrated, press your Forward key (W, or the left stick forward) and the book screen closes.
  The narration keeps playing and a small parchment page in the upper-right corner shows the page the narrator is on.
- If the book was read from the Inventory or a container, that menu closes too.
- In combat the narration pauses and continues when the fight is over. If a fight starts while the full book is open
  (with menus that don't pause the game, such as Skyrim Souls RE), the book switches to the mini page and pauses by
  itself.
- Other menus (inventory, map...) hide the mini page while they're open.
- Stand still for `fReopenAfterStill` seconds out of combat and the full book opens again on the narrator's pages.

## Requirements

- Skyrim SE / AE
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [Read It Now](https://www.nexusmods.com/skyrimspecialedition/mods/168337) (its `Read It Now.esp` and scripts) and
  its own requirements, including SkyPrompt
- For auto page turning, resume and read while walking: Voiced Books of Skyrim

This build replaces `ReadItNow.dll` and `ReadItNow.ini` from the original. Install it after Read It Now and let it
overwrite.

## Settings (`SKSE\Plugins\ReadItNow.ini`)

### [Main]

| Setting | Default | What it does |
|---|---|---|
| `bShowForSkillBooks` | `1` | Show the Read prompt for skill books. |
| `bShowForSpellTomes` | `1` | Show the Read prompt for spell tomes. |
| `iOpenMethod` | `0` | How the prompt opens the book. `0` = straight from your inventory (recommended). `1` = old method: drop the book, read it from the world, then hand it back. |

### [AutoTurn]

| Setting | Default | What it does |
|---|---|---|
| `bAutoTurnPages` | `1` | Turn pages automatically while Voiced Books of Skyrim reads the book. |
| `bResumeOnReopen` | `1` | A book closed mid-narration pauses; reopening it continues there. `0` = Voiced Books' own behavior. |
| `bReadWhileWalking` | `1` | Forward key while a book is narrated switches to the mini page. |
| `fReopenAfterStill` | `8.0` | Seconds of standing still (out of combat) before the full book opens again. `0` = never. |
| `fMiniPageWidth` | `0.25` | Mini page width, as a fraction of the screen width (0.1 to 0.9). |
| `fMiniPageHeight` | `0.5` | Mini page height, as a fraction of the screen height (0.1 to 0.9). |
| `fMiniPageOpacity` | `0.92` | Mini page opacity (0.2 to 1.0). |
| `fTurnDelay` | `0.75` | Seconds to wait after the narrator finishes the open pages. Raise it if pages turn too early; lower it (even negative, e.g. `-0.5`) if too late. |
| `iTurnMethod` | `1` | How the page turn is sent. `1` = a Windows key press (recommended; Skyrim must be the active window). `2` = an input-handler event and `0` = the game's input queue; neither reaches the Book Menu. |
| `iTurnKey` | `205` | DirectInput code of the key pressed for method 1. `205` (0xCD) = Right Arrow. The mapped Next Page key (D) does not turn book pages. |
| `bProbeTurnMethods` | `0` | Test mode: when a book opens, try each turn method once, 3 seconds apart, and log which one worked. No automatic turning while on. |
| `bDebugLog` | `1` | Write details to `Documents\My Games\Skyrim Special Edition\SKSE\ReadItNow.log`. |

## Timing data

Page turns are most accurate with timing files in `SKSE\Plugins\ReadItNow\VBoSTimings\<audio name>.txt`, one spoken
word per line: `start end word` (seconds). The mod matches the last words of the open spread against these to find
the exact moment the narrator finishes it.

`tools/make_vbos_timings.py` builds these files from faster-whisper JSON transcripts (with word timestamps) of the
VBoS audio:

```
python tools/make_vbos_timings.py <folder with .json transcripts> <output folder> [durations.csv]
```

`durations.csv` is optional: lines of `<audio file name>,<length in seconds>`. The length is written into each timing
file so it is only used for that exact recording.

The transcripts and audio themselves are not in this repository.

## Building

You need:
- Visual Studio 2022 with the "Desktop development with C++" workload (MSVC, CMake and Ninja)
- [vcpkg](https://github.com/microsoft/vcpkg), with the `VCPKG_ROOT` environment variable pointing at it
- `SkyPrompt/API.hpp` from [SkyPromptAPI](https://github.com/QTR-Modding/SkyPromptAPI) (API 2.0), placed at
  `<project>/SkyPrompt/API.hpp` (it is not included in this repository)

vcpkg installs CommonLibSSE NG, SimpleIni and Dear ImGui (DX11 and Win32 bindings) from `vcpkg.json` on the first
configure.

Open an **x64 Native Tools Command Prompt for VS 2022**, then:

```
cd /d <path to this repository>
```

```
cmake --preset release
```

```
cmake --build build/release --config Release
```

The DLL is written to `build\release\ReadItNow.dll`. Put it in `SKSE\Plugins` together with
`dist\SKSE\Plugins\ReadItNow.ini` and the `dist\SKSE\Plugins\ReadItNow` folder.

To have the build copy the DLL and the `dist` files into your mod manager for you, set the `SKYRIM_MODS_FOLDER`
environment variable to your mods folder before configuring (they go to `<mods folder>\ReadItNow`).

The compiler warning `D9025 : overriding '/DENABLE_SKYRIM_VR=1' with '/UENABLE_SKYRIM_VR'` is expected: VR support is
switched off on purpose.

## Credits

- **Shazdeh**: Read It Now, the original mod and source this build is based on
- **Voiced Books of Skyrim** and its narrators, whose recordings the auto page turning follows
- **QTR-Modding**: SkyPrompt and SkyPromptAPI
- **powerofthree**: Photo Mode, whose ImGui hook points the mini page uses
- [CommonLibSSE NG](https://github.com/CharmedBaryon/CommonLibSSE-NG), a fork of the
  [powerof3 fork](https://github.com/powerof3/CommonLibSSE) of the original `CommonLibSSE` by
  [Ryan McKenzie](https://github.com/Ryan-rsm-McKenzie)
- [SKSE Templates by mrowrpurr](https://github.com/SkyrimScripting/SKSE_Templates)
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut
- [faster-whisper](https://github.com/SYSTRAN/faster-whisper), used to make the timing data
- Patched build by **tonycubed2**

## License

MIT, as the original. See [LICENSE](LICENSE): Shazdeh's copyright notice is kept as the license requires.
