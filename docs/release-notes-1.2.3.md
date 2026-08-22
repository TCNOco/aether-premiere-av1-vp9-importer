# Aether 1.2.3

A panel inside Premiere, a diagnostic that answers "why does this file not
open" without a week of correspondence, and a window that no longer looks like
it was built in 2005.

## Window → Extensions → Aether

Until now there was exactly one way to tell whether the plug-in was installed:
drag a file onto the timeline and see whether it opened. That is a poor way to
learn anything, and a worse way to report a problem.

The plug-in now ships a panel. Settings and diagnostics, without leaving
Premiere, in the menu where extensions live.

The panel does not compute anything itself. It reads the same `settings.ini`
the plug-in reads, and runs the same diagnostic engine the window runs. Writing
the checks a second time in JavaScript would have created a second truth, and
two truths drift.

It is signed, so no developer mode is needed. **Do not edit the files inside
it** — any change breaks the signature, and Premiere then stops showing the
panel silently, with no message at all.

The panel is optional. Decoding works with or without it.

## Diagnostics

A report you can attach to an issue, in one click. It covers:

| | |
|---|---|
| **System** | Windows build, CPU and thread count, memory, GPU and driver version |
| **Adobe** | every installed version of Premiere Pro, After Effects and Media Encoder |
| **Aether** | where the plug-in is, its version, whether it loads, which decoders FFmpeg actually has |
| **Decoding** | five bundled clips: AV1 8-bit and 10-bit, VP9, variable frame rate, Opus |
| **Your file** | the one that will not open — what is inside it, and whether it decodes |

That last row is the point. A report that says "all PASS" to somebody whose
file will not open is worse than no report: it answers a question nobody asked.
Drop the file in and it gets the same treatment as the bundled ones, plus what
the container actually holds — codec, bit depth, colour, frame rate, tracks.

Seeking is checked with three requests: frame 0, then the middle, then frame 0
again. The first frame is nearly always delivered because it is a keyframe;
what breaks is coming back.

**Copy report** puts the text on the clipboard with the home folder replaced by
`%USERPROFILE%`, the user name by `<user>` and the machine name by `<machine>`.
The report goes into a public issue, and without that we would be teaching
people to publish more than they meant to.

There is a console version too — `AetherDiagnose.exe`, next to the plug-in. It
is the engine the panel calls; `--json` makes it machine-readable.

## The window

`AetherSettings.exe` is now `Aether.exe`, with two pages instead of one, and it
follows the system dark theme.

Three separate things were wrong with how it looked, and only the first is the
obvious one:

- **No manifest.** Without one Windows grants neither modern control styles nor
  DPI awareness. Neither can be switched on from code.
- **The icon was never loaded.** The resource was in the file all along; nothing
  ever put it into the window class.
- **Everything weighed the same.** Heading, options and explanations shared one
  font, one colour and one background.

Declaring per-monitor DPI awareness without handling `WM_DPICHANGED` is worse
than not declaring it: Windows stops scaling the window itself, and on a 150%
display it comes out tiny. That handler is now there.

### Dark mode took two attempts

Checkbox and radio labels are drawn by the theme engine, which takes its colour
from the theme and not from our device context. `SetTextColor` is simply
ignored. The result was black text on black, and push buttons outlined in white.

The undocumented `uxtheme` calls everyone reaches for did not fix it —
measured, not guessed: the label stayed at 32 out of 255. So the radio buttons
and the buttons are drawn by hand. More code, and entirely predictable: the
colours come from one palette, so the two themes cannot break separately.

The list header needed its own fix. "Проверка" and "Что вышло" are drawn by a
`SysHeader32` nested inside the list, which sends its drawing notifications to
the list rather than to us — they never reached the window at all.

### A bug the theme work uncovered

The window was missing `WS_CLIPCHILDREN`, so painting the button bar erased the
buttons sitting in it. Whichever drew last was the one you saw: a button
vanished in one run out of three, and the diagnostic list came out empty while
holding a full set of rows. It only shows up on a second screenshot, because it
depends on ordering.

## Versions are visible everywhere now

1.2.2 put a version resource in the plug-in. `Aether.exe` and
`AetherDiagnose.exe` did not have one, which is the same gap in a different
file. All three now report `1.2.3` in Properties, and the diagnostic warns when
the plug-in and the window disagree — a stale copy of one of them is exactly
the kind of thing that wastes an evening.

## Known limitations

- No alpha channel for AV1 — it is stored differently from VP9's.
- No timecode from the file, no deinterlacing.
- Log curves (S-Log, V-Log, C-Log) are passed on but have never been seen in
  practice.
- Intel and AMD hardware decoding paths exist but have never been run.
- The panel uses CEP, which Adobe has deprecated in favour of UXP. It works in
  Premiere Pro 2020 through 2026; it will need rewriting eventually.
- Windows only.

## Install

Download `AetherSetup-1.2.3.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

There is also **`Aether-1.2.3-portable.zip`** for anyone who would rather copy a
folder than run an unsigned installer from a stranger — a reasonable position.
It now holds two folders: `Aether` goes into `…\Adobe\Common\Plug-ins\7.0\
MediaCore\`, and `com.nehade.aether` goes into `…\Common Files\Adobe\CEP\
extensions\` if you want the panel. `INSTALL.txt` in the archive spells it out.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.2.3.exe        SHA256  e1646f67fb9417d49463013ecab53ff68936c87a41215699cf7ab2b9f6b2027c
Aether-1.2.3-portable.zip    SHA256  1286d7d7fd2fcedfcaaa13e1631a13c11aeda781cd510974a72b137d6605a192
```

The README has a section on what else there is to go on besides these numbers.
