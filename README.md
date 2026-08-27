<div align="center">

<img src="docs/assets/aether.png" alt="Aether" width="128">

# Aether

**Native AV1 and VP9 importer for Premiere Pro.**

Drag a file onto the timeline and it plays. No transcoding, no proxies,
no waiting. One installer covers Premiere Pro, After Effects and Media Encoder.

Free, open source, Apache 2.0. Windows x64, **v1.3.3**.

[![core checks](https://github.com/neoHaDe/aether-premiere-av1-vp9-importer/actions/workflows/core-checks.yml/badge.svg)](https://github.com/neoHaDe/aether-premiere-av1-vp9-importer/actions/workflows/core-checks.yml)
[![C++](https://img.shields.io/badge/C++-17-00599C?logo=cplusplus&logoColor=white)](https://isocpp.org)
[![FFmpeg](https://img.shields.io/badge/FFmpeg-LGPL-007808?logo=ffmpeg&logoColor=white)](https://ffmpeg.org)
[![Premiere Pro](https://img.shields.io/badge/Premiere_Pro-2019--2026-EA77FF?logo=adobepremierepro&logoColor=white)](https://www.adobe.com/products/premiere.html)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

Windows · Premiere Pro, After Effects, Media Encoder ·
[Русская версия](README.ru.md)

### → [Download the latest release](../../releases/latest)

</div>

---

**Premiere cannot decode AV1 or VP9.** Not in 2019, not in 2026. An AV1 file
fails with *"File uses unsupported video compression type av01"* — the
container is parsed, the stream is recognised, and there is simply no decoder
behind it. VP9 usually arrives in WebM, which Premiere will not open at all.

Aether supplies the missing decoders. That is what you run into if you record
with OBS, where AV1 gives the same picture at a noticeably lower bitrate, or if
your footage comes from the web, where VP9 is what you are handed.

## What you need

The short answer: Windows, and an Adobe application. Nothing else.

| Requirement | Answer |
|---|---|
| **System** | Windows x64. No macOS: the decoding core is portable, everything else is Win32 |
| **Applications** | Premiere Pro, After Effects, Media Encoder. One installer covers all three |
| **Verified on** | Premiere Pro **2019** (13.1.5), **2025** (25.x), **2026** (26.x), After Effects **25.x**, Media Encoder **25.x** |
| **Not tried** | Premiere Pro 2020 – 2024. They sit between two verified points, see the note below |
| **Graphics card** | **not required.** The CPU decodes by default, and it is faster — measurements below |
| **NVIDIA** | optional: `av1_cuvid`, `vp9_cuvid`, tested on an RTX 5080 |
| **AMD** | optional: through D3D11VA, tested on the integrated Radeon of a 9800X3D |
| **Intel** | not tried — same path as AMD, the hardware was not here |
| **Async delivery** | on. Worth 5–12% on the GPU path, nothing on the CPU; can be switched off |
| **Panel in Premiere** | optional: Window → Extensions → Aether |
| **Privileges** | administrator, to install only |

The plug-in installs into the folder Adobe applications share, so all of them pick
it up at once — there is nothing to install per application.

The gap between 2019 and 2025 is a deliberate decision rather than an oversight.
The importer interface was version 21 in 2019 and is 24 today, so 2020–2024 sit
between two versions that are known good, and the plug-in asks the host for
whichever suite versions it actually has instead of demanding the newest — which
is exactly what 2019 exercised, taking version 7 of the frame-cache suite where
2025 gives 8. For the same reason Premiere 2026 worked without a single change.

## What it opens

What the plug-in claims, and what it does with it. **"Yes" means tested** — live
or by an automated check. Where nothing was tried, the table says so.

### Codecs and containers

| Format | Support |
|---|---|
| AV1 | **yes** |
| VP9 | **yes** |
| everything else | **handed back** — Premiere opens it itself |
| MP4, MOV, M4V | **yes** — with AV1 or VP9 inside |
| MKV, WebM | **yes** — with AV1 or VP9; newer Premiere versions open some H.264/AAC MKV themselves |
| Audio-only MKA, MKV, WebM | **yes** — when Adobe's native importer cannot take them |
| Audio-only MP4 and M4A | **no, deliberately** — Premiere opens those itself |
| OGV, FLV, TS, VOB | **no** — those carry other codecs, not these two |

The list of extensions is deliberately wider than the list of codecs: an MP4 can
hold anything, so the file is opened first and handed back if what is inside is
neither AV1 nor VP9. Taking someone else's file is worse than not taking your own.

### Picture

| Property | Support |
|---|---|
| 8-bit | **yes** |
| 10-bit | **yes** — native P010 planes when the host accepts them, otherwise 16-bit BGRA |
| 12-bit | **not tried** — the same path as 10-bit |
| Alpha, VP9 in WebM | **yes** — CPU decoding only, see below |
| Alpha in AV1 | **no** — stored differently, not done |
| Interlaced | **no** |

### Colour

| Property | Support |
|---|---|
| Matrix BT.601 / 709 / 2020 | **yes** — taken from the file, not guessed |
| Full and limited range | **yes** — also taken from the file |
| Colour space reported to the host | **yes** — primaries and transfer as ITU codes |
| HDR: PQ and HLG | **yes** — passed through as they are, Premiere does the tone mapping |
| Log curves (S-Log, V-Log, C-Log) | **expected** — same path, no real footage to try |

### Timing

| Property | Support |
|---|---|
| Constant frame rate | **yes** |
| Variable frame rate | **yes** — the frame is found by time, see below |
| Files that do not start at zero | **yes** — picture and sound share one clip zero |
| Timecode from the file | **no** |

### Audio

| Property | Support |
|---|---|
| AAC, Opus, FLAC | **yes**, tested |
| anything else FFmpeg decodes | **expected** — only video has a codec list |
| Multiple tracks | **yes**, they stay separate |
| Mono, stereo | **yes** |
| 5.1 and above | **not tried** — the channel count is passed through as is |


## Install

1. Download `AetherSetup-x.y.z.exe` from [Releases](../../releases).
2. Close Premiere Pro, After Effects and Media Encoder.
3. Run it. Administrator rights are required: Adobe plug-ins live in a shared
   system folder.

Or take `Aether-x.y.z-portable.zip` from the same page and copy the `Aether`
folder into `…\Adobe\Common\Plug-ins\7.0\MediaCore\` yourself. Same files, no
installer, nothing written to the registry. `INSTALL.txt` inside the archive
has the steps.

There is nothing to choose. The installer asks Adobe where the shared plug-in
folder is — the applications write that address into the registry themselves —
and lists the applications it found, so it is clear before installing anything
which of them will pick the plug-in up. Nothing found is not an error either:
the plug-in can be installed first and will be picked up when an application
appears.

To remove it, use *Apps & features* or the uninstaller in the plug-in folder.
If you installed from the archive, delete the folder.

### Why you would trust this download

You should not simply trust it, and this section is not asking you to. It is a
plug-in that goes into Adobe's own folder, from a stranger on the internet, and
the installer is not code-signed — Windows will say "unknown publisher" and it
is right to.

What is on offer instead:

- **The source is all here**, including the part that does the decoding. Nothing
  is fetched at runtime, and nothing phones anywhere.
- **Every release lists its SHA256**, and the number is checked against the file
  that is actually attached before the release goes out. If they disagree, do
  not install it and open an issue.
- **The checks run in public** on every push, on GitHub's own Windows runners —
  the badge at the top of this page links to them. Test media is generated on
  the spot rather than downloaded.
- **FFmpeg is the stock LGPL build**, unmodified, from
  [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds). Right-click any
  of the shipped DLLs → *Properties* → *Details* and the product version names
  the exact build — `n8.1.2-44-g7c533d0f86-20260818` at the time of writing —
  which is the one to look for on that project's releases page.
- **The portable archive is reproducible**: the same build produces a
  byte-identical zip, so the checksum is verifiable rather than a promise.
- **You can build it yourself.** `build.bat` needs Visual Studio and the Adobe
  SDK, and the SDK is the only piece not in this repository, because Adobe's
  terms do not allow redistributing it.

The one thing that would remove the Windows warning is a code-signing
certificate, which costs a few hundred dollars a year. When this project is
worth that to enough people, it will get one.

Whatever application it loads into is named in the first lines of the log, so a bug
report can say which version was involved:

```
host: import interface version 21 (SDK headers know 24)
host: PPro 13.1.5
```

## Diagnostics

There used to be exactly one way to tell whether the plug-in was installed:
drag a file onto the timeline and see whether it opened. A poor way to learn
anything, and a worse way to report a problem.

There is now a report: the system, every installed version of the Adobe
applications, the state of the plug-in, which decoders FFmpeg actually has, a
run over five bundled clips — and **a check of the file that will not open**.

That last one is the point. A report saying "all fine" to somebody whose file
will not open is worse than no report: it answers a question nobody asked. The
file goes through the same checks as the bundled ones, plus what the container
actually holds — codec, bit depth, colour, frame rate, tracks.

Seeking is checked with three requests: frame 0, the middle, frame 0 again. The
first frame is nearly always delivered because it is a keyframe; what breaks is
coming back.

**Copy report** replaces the home folder with `%USERPROFILE%`, the user name
with `<user>` and the machine name with `<machine>`. The report goes into a
public issue, and without that we would be teaching people to publish more than
they meant to.

There are two doors to it, and both are needed:

```
Aether.exe             the window next to the plug-in
Window → Extensions    the panel inside Premiere
```

The panel is more convenient; the window is more dependable. Diagnostics are
wanted exactly when Premiere will not start because of a driver — and a panel
inside Premiere is useless at that moment.

Only one program does the work either way. The panel is HTML and cannot decode
video at all, so it runs `AetherDiagnose.exe` — the same engine the window uses
— and displays its answer. Writing the checks a second time in JavaScript would
have created a second truth, and two truths drift apart.

## What works

- **AV1 and VP9** video on the timeline, decoded on the GPU or the CPU, and which
  of the two wins is not the obvious answer, see the measurements below
- **Multi-track audio kept separate.** A recording can carry several audio streams
  at once, and they arrive as separate tracks instead of being mixed into one
- Scrubbing, seeking and export
- **Files with no video at all**, when the container is one Premiere cannot open:
  a Matroska or WebM holding only audio. The same rule as everywhere else decides
  it — an audio-only MP4 or M4A is refused, because Premiere opens those itself

Files with any other codec are handed straight back to Premiere's own importer, so
nothing about your existing footage changes.

One caveat about MKV and WebM: Premiere 25.2 added native import for some
H.264/AAC MKV files. Aether does not take those away from it: the plug-in
demuxes AV1/VP9 itself and refuses an MKV holding H.264. A recent Premiere can
then open that file with its own importer; an older one may still be unable to.
WebM and audio-only Matroska support likewise depends on what the host can
handle after Aether has declined a file.

### After Effects gets it too

Adobe plug-ins of this kind live in a folder shared by the whole suite, so the
installer puts the importer in front of After Effects as well. That was a guess
until it was checked; now it is in the log:

```
host: FXTC 25.3.2
imOpenFile8: accepted, AV1 2560x1440, decoder libdav1d
audio: track 0 ... track 3
frame 0 delivered (2560x1440, stride 10240)
```

`FXTC` is After Effects. It imported the same OBS recording, kept all four audio
tracks apart, and pulled 93 frames during a preview — about 42 per second at 1440p,
which is After Effects setting the pace, not the decoder.

Media Encoder answers the same way. A full export of that recording pulled all
**1265 frames with no errors**, at 71 frames per second — the clip is 21 seconds
long and took 17.8 seconds to render, so the plug-in is not what the export waits
for. Exporting out of Premiere Pro CC 2019 works as well.

Rendering out of After Effects works too. Tested on one recording per application.

## Performance

Measured on an RTX 5080 with a 16-thread CPU; VP9 lands within a few per cent of AV1.

### Hardware decoding is not the win you would expect

Per frame, sequential playback:

| 2560x1440@60 | Total | Decode | GPU→RAM | Colour |
|---|---|---|---|---|
| `av1_cuvid` (GPU) | 7.7 ms — 130 fps | 0.13 ms | **3.4 ms** | **4.2 ms** |
| `libdav1d` (CPU) | 1.25 ms — **800 fps** | 0.55 ms | — | 0.7 ms |

| 3840x2160@60 | Total | Decode | GPU→RAM | Colour |
|---|---|---|---|---|
| `av1_cuvid` (GPU) | 13 ms — 77 fps | 0.14 ms | 3.4 ms | 9.5 ms |
| `libdav1d` (CPU) | 3.4 ms — **291 fps** | 0.63 ms | — | 2.8 ms |

The software decoder is several times faster end to end, and the reason is
structural: Premiere needs frames in system memory, so hardware decoding must pay
for a copy out of VRAM that software decoding never makes. Decoding itself is
negligible either way, under a millisecond.

Two caveats before reading too much into this. The measurement is one machine with
a strong CPU; on a weak one dav1d would lose. And a benchmark has the machine to
itself, whereas a real edit has Premiere competing for the same cores. Both paths
are far above the 60 fps playback needs, so for most work the difference is not
visible. Switch with the setting below and judge on your own footage.

### What 10-bit costs

A 10-bit source is offered as native P010 planes first, then `BGRA_4444_16u`, then
8-bit, so the host can fall back if it will not take a planar buffer. Premiere 26
accepts P010; its `GetRowBytes` for that format can report zero, and the stride is
then taken from the buffer size. An 8-bit file is still offered one format, and
the measurements above are unchanged.

The wider output is not free, though, and the price is the colour conversion rather
than the decoding:

| 2560x1440@60 | Total | Colour |
|---|---|---|
| 8-bit out | 4.8 ms — 208 fps | 3.9 ms |
| 16-bit out | 16 ms — 62 fps | 15.2 ms |

That is close enough to 60 fps to matter on a 1440p timeline. Of those 15.2 ms only
0.3 belongs to the range conversion described below; the rest is FFmpeg's own
conversion into a 16-bit-per-component layout. At 1280x720 the same comparison is
609 fps against 219.

### Seeking

| | |
|---|---|
| Step backwards | 5.6 ms/frame (71 ms without the frame cache) |
| Random seek | 75 ms — one keyframe interval, inherent to the codec |

## Settings

Decoding runs on the CPU by default. To change that, open **Aether —
settings** from the Start menu; it is installed with the plug-in.

| | |
|---|---|
| Automatic | CPU on machines with 8 logical processors or more, GPU below that |
| CPU (dav1d) | the faster path on this hardware, see the measurements above |
| GPU (NVIDIA / Intel / AMD) | leaves the CPU free for the rest of the edit |

Asynchronous delivery is switched in the same place: `async = off` turns it
off, and `AETHER_ASYNC=0` does the same from the environment. It is on by
default — it costs nothing on the CPU path and is worth 5–12% on the GPU path,
measurements below.

The choice lands in `%LOCALAPPDATA%\Aether\settings.ini` and applies when
Premiere restarts. The environment variable `AV1IMPORTER_HARDWARE=0` (or `=1`)
overrides the file without changing it, which is the quick way to test a hunch.
Either way, the decoder actually used is recorded in the log.

The same window and Premiere panel also control:

- **Aether enabled** — when off, a restarted Adobe application receives priority
  0 and every file is handed to the next importer without loading FFmpeg;
- **Aether RAM frame cache** — 0 (off) through 4 GiB, default 512 MiB. This caps
  only decoded frames retained by Aether, not Premiere's whole process;
- **reduced preview cache** — lossless BGRA8 Draft/Low frames up to 2 MiB,
  stored under `%LOCALAPPDATA%\Aether\preview-cache\v1`, limit 2 GiB by default.
  Full-size playback, native YUV/P010 delivery, audio and export never use it.
  It is available but remains off by default until a cold/warm Premiere project
  benchmark confirms the intended win without a playback regression.

`cache_memory_mb=0` and `AETHER_CACHE_MB=0` now mean **RAM cache off**. Older
builds treated zero as unlimited. The other overrides are `AETHER_ENABLED`,
`AETHER_PREVIEW_CACHE` and `AETHER_PREVIEW_CACHE_MB`.

There is deliberately no modal settings dialog in the importer API. The optional
Premiere panel is convenient, while the standalone window remains available when
a graphics driver fault stops Premiere from starting.

## Building from source

Not needed to use the plug-in, only to modify it.

You will need, none of which are in this repository:

- **Adobe Premiere Pro C++ SDK** into `sdk/`, from
  [developer.adobe.com](https://developer.adobe.com/premiere-pro/); needs an Adobe ID,
  and you accept Adobe's Developer Terms yourself
- **FFmpeg** into `ffmpeg/`, a BtbN `win64-lgpl-shared` build (headers, `.lib` and `.dll`)
- **Visual Studio Build Tools 2022** with the C++ workload
- **Inno Setup 6**, only to build the installer

```
build.bat                        the plug-in -> build\Release\Aether.prm
build-test.bat                   the test programs
release-check.bat                strict pre-release gate; SDK checks may not skip
installer\build-installer.bat    the installer -> dist\
```

Three console programs let you test almost everything without launching Premiere,
which is what makes this project practical to work on:

```
build\decoder_test.exe <file.mp4>                          metadata, timings, audio levels, checks
build\plugin_test.exe  <Aether.prm>                   plug-in loading and its answers
build\host_test.exe    <Aether.prm> <file.mp4>        the whole import, from a fake host
```

`build-test.bat` deliberately skips the SDK-dependent programs when the SDK is
absent, which is useful for the public core CI but is not sufficient before a
release. GitHub cannot run `plugin_test` / `host_test`: Adobe's SDK is not ours
to put on a hosted runner. That gate is `release-check.bat` on a machine that
already has the SDK. It requires the SDK, builds and stages the actual
plug-in with its FFmpeg DLLs (hashed against `tools/ffmpeg-dll.sha256`), runs the core checks, verifies the `.prm`, and
drives representative files through all fake-host profiles. Any skipped layer
is a failure.


## How it works

The implementation notes live in **[docs/internals.md](docs/internals.md)** —
what the Premiere SDK does not tell you, why FFmpeg is delay-loaded, how a
timeline frame differs from a source frame, where alpha hides, what HDR is
declared as, and the rest of it.

None of it is needed to use the plug-in. It is there because every one of those
sections is a bug that cost a day to find, written down so it costs nobody
else one.

## Licensing, Adobe's terms, and AV1 patents

Short version: Apache 2.0 covers this repository's own code and nothing else. Full
detail in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

**This project's code** is under the Apache License 2.0, see [LICENSE](LICENSE) and
[NOTICE](NOTICE). That licence applies only to the source here. It does not extend
to the Adobe SDK or to FFmpeg, and it could not: neither is ours to license.

Releases up to 1.1.0 were MIT. The change was made on purpose: Apache 2.0 grants
patent rights explicitly rather than leaving them to be inferred, and withdraws
them from anyone who attacks the project with patents. For a codec plug-in that is
a live question rather than boilerplate; see the patent note at the end. Nothing
MIT permitted has been taken away; the conditions added are attribution, a note of
what you changed, and keeping [NOTICE](NOTICE) with redistributions.

**The Adobe SDK is not redistributed.** No headers, no sample code, no utilities,
no other SDK component appears in this repository or in any release. That is by
design, see `.gitignore`. Building from source requires you to obtain the SDK from
[Adobe](https://developer.adobe.com/premiere-pro/) and accept Adobe's own Developer
Terms. The plug-in uses only the public, documented API and does not modify Adobe
software. What the release ships is the compiled plug-in, our own object code,
which is what Adobe's terms contemplate for plug-ins.

Adobe, Premiere Pro and Media Encoder are trademarks of Adobe Inc. This project is
independent: not affiliated with, sponsored by, or endorsed by Adobe.

**FFmpeg** is used under LGPL v3: unmodified BtbN builds, linked dynamically, with
the licence text shipped next to the plug-in. You may swap the DLLs for your own
build of the same versions; that relinking freedom is what LGPL requires.

**AV1 and patents.** AV1 is published by the Alliance for Open Media as
royalty-free under the AOMedia Patent License 1.0, but that covers only the
Necessary Claims of AOMedia members, and third-party claims are possible. In 2026
Dolby sued Snap over AV1, which shows royalty-free is not an absolute guarantee.
This plug-in only decodes and has no encoding path; decoding is done by your GPU
vendor's driver or by dav1d (BSD-2-Clause). For an open-source plug-in the
practical exposure is far lower than for a large commercial product, but it is not
zero. **None of this is legal advice.** If you ship this commercially, get your own.
