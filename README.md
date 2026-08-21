# AV1 / VP9 Importer for Adobe Premiere Pro, After Effects and Media Encoder

[![core checks](https://github.com/neoHaDe/premiere-av1-vp9-importer/actions/workflows/core-checks.yml/badge.svg)](https://github.com/neoHaDe/premiere-av1-vp9-importer/actions/workflows/core-checks.yml)

Drag an AV1 or VP9 file onto the timeline and it just works: no transcoding,
no proxies.

[Русская версия](README.ru.md)

Premiere Pro **cannot decode AV1 or VP9** — not in 2019, not in 2025. For AV1 its
demuxer parses the container and recognises the stream as `av01`, but there is no decoder, so the
import fails with *"File uses unsupported video compression type av01"*. VP9
usually arrives in WebM, which Premiere cannot even open. This plug-in adds the
missing decoders, using FFmpeg and, where the hardware allows, the GPU.

That matters if you record with OBS: AV1 gives the same picture quality at a
noticeably lower bitrate, but the recordings were unusable in Premiere. VP9 matters
if your source footage comes from the web, where it is what you are usually given.

## Will it work for you

**Applications.** The plug-in installs into the folder Adobe applications share, so
every one of them picks it up. Which of them were actually tried:

| | |
|---|---|
| Premiere Pro CC 2019 — 13.1.5 | tested, the oldest version verified |
| Premiere Pro 2020 – 2024 | not tried, expected to work — see the note below |
| Premiere Pro 2025 — 25.x | tested, the main target |
| After Effects 25.3.2 | tested: import, audio tracks, preview, render |
| Media Encoder 25.6.4 | tested: a whole export, 1265 frames, no errors |
| macOS | no. The decoding core is portable, the rest is Win32 |

The gap in the middle is a deliberate decision rather than an oversight. The
importer interface was version 21 in 2019 and is 24 today, so 2020–2024 sit between
two versions that are known good, and the plug-in asks the host for whichever suite
versions it actually has instead of demanding the newest — which is exactly what
2019 exercised, taking version 7 of the frame-cache suite where 2025 gives 8.

**Formats.**

| | |
|---|---|
| AV1 | yes — MP4, MKV, WebM, MOV, M4V |
| VP9 | yes — the same containers |
| Multi-track audio | yes, tracks stay separate |
| Variable frame rate | yes — the picture is found by time, not by frame number |
| Files that do not start at zero | yes — picture and sound share one clip zero |
| 10-bit | yes — delivered as 16-bit, see below |
| HDR | decoded, but the transfer metadata is not passed on yet |
| Alpha channel | yes, for VP9 in WebM — decoded on the CPU, see below |
| Audio-only MKV, MKA, WebM | yes — Premiere cannot open those containers at all |
| Audio-only MP4 and M4A | no, deliberately — Premiere opens those itself |
| Every other codec | handed straight back to Premiere's own importer |

**Hardware.** Windows x64, nothing else. Without a supported GPU the plug-in decodes
on the CPU, and on this hardware that path is the faster one anyway.

## Install

1. Download `AV1Importer-Setup-x.y.z.exe` from [Releases](../../releases).
2. Close Premiere Pro and Media Encoder.
3. Run it. Administrator rights are required: Adobe plug-ins live in a shared
   system folder.

To remove it, use *Apps & features* or the uninstaller in the plug-in folder.

Whatever application it loads into is named in the first lines of the log, so a bug
report can say which version was involved:

```
host: import interface version 21 (SDK headers know 24)
host: PPro 13.1.5
```

## What works

- **AV1 and VP9** video on the timeline, decoded on the GPU or the CPU, and which
  of the two wins is not the obvious answer, see the measurements below
- **Multi-track audio kept separate.** OBS writes microphone, game, Discord and
  music as distinct streams, and they arrive as distinct tracks
- Scrubbing, seeking and export
- MP4, MKV, WebM, MOV, M4V, MKA containers
- **Files with no video at all**, when the container is one Premiere cannot open:
  a Matroska or WebM holding only audio. The same rule as everywhere else decides
  it — an audio-only MP4 or M4A is refused, because Premiere opens those itself

Files with any other codec are handed straight back to Premiere's own importer, so
nothing about your existing footage changes.

One caveat about MKV and WebM: Premiere has no native support for those containers
at all, so the plug-in demuxes them itself. That works, but only for the codecs
above. An MKV holding H.264 is refused by the plug-in and Premiere cannot open it
either.

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

A 10-bit source is offered to the host as `BGRA_4444_16u` first and 8-bit second,
so the host can fall back if it wants to. Nothing changes for an 8-bit file: it is
still offered one format, and the measurements above are unchanged — 205 fps before
this was added, 205 after.

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

Decoding runs on the CPU by default. To change that, open **AV1 Importer —
settings** from the Start menu; it is installed with the plug-in.

| | |
|---|---|
| Automatic | CPU on machines with 8 logical processors or more, GPU below that |
| CPU (dav1d) | the faster path on this hardware, see the measurements above |
| GPU (NVIDIA / Intel / AMD) | leaves the CPU free for the rest of the edit |

The choice lands in `%LOCALAPPDATA%\AV1Importer\settings.ini` and applies when
Premiere restarts. The environment variable `AV1IMPORTER_HARDWARE=0` (or `=1`)
overrides the file without changing it, which is the quick way to test a hunch.
Either way, the decoder actually used is recorded in the log.

There is deliberately no settings dialog inside Premiere. The setting matters most
when a graphics driver fault stops Premiere from starting, and inside Premiere it
would be out of reach at exactly that moment.

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
build.bat                        the plug-in -> build\Release\AV1Importer.prm
build-test.bat                   the test programs
installer\build-installer.bat    the installer -> dist\
```

Three console programs let you test almost everything without launching Premiere,
which is what makes this project practical to work on:

```
build\decoder_test.exe <file.mp4>                          metadata, timings, audio levels, checks
build\plugin_test.exe  <AV1Importer.prm>                   plug-in loading and its answers
build\host_test.exe    <AV1Importer.prm> <file.mp4>        the whole import, from a fake host
```

`host_test` is the interesting one. It impersonates Premiere and drives the built
`.prm` through open, info, frames and audio, then does it again pretending to be
an older host: one that only knows version 7 of the frame-cache suite, one from the
Premiere 13.0 era, one that refuses the cache suite altogether. Each run must
deliver the same pixels.

That covers the failure this project actually had. A host does not hand out a suite
version it has never heard of, so asking for the newest one and nothing else leaves
the pointer null on every older Premiere — which looks exactly like the feature not
existing. Reverting the fix makes two of the five profiles fail, so the check is
not decorative.

What it cannot show: whether an older Premiere sends different selectors in a
different order, whether older SDK headers laid the structs out differently, or how
it reads the `IMPT` resource. Only a real installation answers those.

`decoder_test` ends with correctness checks and exits non-zero if any fails: a
cached frame against a freshly decoded one, buffer bounds when Premiere asks for a
reduced size, and whether the same audio range reads identically twice. The last
one found a real bug the first time it ran.

Two scripts turn that into something a build server can run:

```
tools\make-test-media.ps1     generates the media with FFmpeg - nothing is downloaded
tools\run-core-checks.ps1     runs the core against it and judges the result
```

The judging matters as much as the decoding. Five files must be accepted and pass
every check; two must be **refused** — an audio-only MP4 and an H.264 file, both of
which Premiere opens perfectly well on its own. A plug-in that grabs more than it
should is worse than one that grabs less.

That pair runs on every push, on GitHub's Windows runners. The plug-in itself
cannot be built there: it needs the Adobe SDK, which may not be redistributed, so
no build server can have a copy. What is checked is the layer deliberately kept
free of any Adobe dependency — most of the code, and nearly all of the risk.

## How it works

`src/AV1Decoder.*` is the decoding core. It deliberately knows nothing about the
Adobe SDK, so it can be built and exercised by a console program, which removes
most of the debugging from the slow build-install-restart-Premiere loop.
`src/AV1Importer.*` is the SDK layer and only translates Premiere's requests into
calls on the core.

### Three conditions Premiere never tells you about

Each one fails identically: the file will not open, exactly as if no plug-in were
installed. There is no message and no log entry.

1. **Priority 100 or higher.** Below that, Premiere handles MP4 itself and never
   reaches the plug-in. Only 100+ overrides Adobe's own importers.
2. **Answer `imGetSupports7/8`** with `malSupports7/8`. This is the handshake about
   which interface version the plug-in speaks. Without it Premiere reads the file
   info and walks away, never asking for a pixel format or a frame.
3. **`subType` must be `imUncompressed`**, not `av01`. The plug-in does the
   decoding and hands over ready pixels; saying `av01` sends Premiere off to look
   for a decoder it does not have, and it reports exactly the error you were
   trying to fix.

### Why the C++ runtime is linked statically

Premiere Pro CC 2019 keeps its own `msvcp140.dll` — version 14.00.24210, built with
Visual Studio 2015 — in its program folder. Windows resolves DLLs by name against
what is already loaded in the process, so a plug-in built with a newer Visual Studio
gets handed that old runtime and fails to initialise. `LoadLibrary` returns 1114,
and Premiere reacts exactly as if the plug-in were not installed: no message, no log
entry, and the same "unsupported compression type av01" you were trying to fix.

Linking the runtime statically (`/MT`) removes the dependency instead of fighting
over it. It is safe here because no runtime object ever crosses the plug-in
boundary: Premiere is spoken to through its own structures, FFmpeg through a C API,
and the FFmpeg DLLs carry their own runtime anyway.

`plugin_test` reads the import table out of the built `.prm` and fails if
`MSVCP*` or `VCRUNTIME*` reappear there — the machine that builds the plug-in is
the one machine where this bug cannot be observed.

### Why FFmpeg is delay-loaded

Windows looks for dependent DLLs next to the *executable*, meaning next to
Premiere, and never next to the DLL being loaded. With ordinary linking Premiere
would simply fail to load the plug-in, without a word. So `AV1Runtime.cpp` loads
FFmpeg from the plug-in's own folder at startup, and the libraries themselves are
linked with `/DELAYLOAD`, so by the time a real call happens they are already in
the process.

Two non-obvious build requirements follow:

- **whole-program optimisation must be off.** With it the linker silently ignores
  `/DELAYLOAD`;
- **the import libraries are regenerated** into `ffmpeg/lib-msvc`, because the ones
  shipped with FFmpeg are produced by a different toolchain and `/DELAYLOAD` does
  not work with them either (LNK4199):

```
lib /def:ffmpeg\lib\avcodec-62.def /name:avcodec-62.dll /out:ffmpeg\lib-msvc\avcodec.lib /machine:x64
```

### Alpha hides in two places at once

A VP9 file with transparency will quietly arrive fully opaque unless two separate
things go right, and neither of them announces itself.

**The alpha is not in the video stream.** Matroska carries it as an extra block
attached to each frame and marks the track with an `alpha_mode` tag. The stream's
own pixel format still reads `yuv420p`, so asking the format whether there is
transparency gives the wrong answer. The tag is the only honest source.

**Only one of the two VP9 decoders returns it.** FFmpeg's native `vp9` decoder
drops the alpha silently; `libvpx-vp9` returns `yuva420p`. Hardware decoders drop
it too — `vp9_cuvid` was measured returning a solid opaque channel. So a file with
transparency is forced onto the CPU path even when the GPU is set as preferred.
Losing the alpha without a word is worse than decoding more slowly.

`decoder_test` checks the result rather than the intent: if the file declares
alpha, the delivered frames must contain more than one alpha value.

### Adobe's 16-bit is not the obvious 16-bit

In Premiere's `_16u` pixel formats white sits at **32768**, not at 65535 — the same
convention After Effects and Photoshop use, stated on page 64 of the SDK guide.
FFmpeg produces the full 0–65535 range, so every component has to be brought down
afterwards. `(v + 1) >> 1` does it exactly at both ends: 65535 becomes 32768 and 0
stays 0.

Getting this wrong would not crash anything. It would just make every 10-bit clip
twice as bright and clipped, which is the kind of bug that survives a long time.

`decoder_test` checks both halves on a 10-bit file: that the output carries more
than 256 distinct values per channel — otherwise it is 8-bit data in a wider buffer
— and that nothing exceeds 32768.

### A timeline frame is not a source frame

Premiere asks for frame number N of a timeline that runs at a fixed rate. A
source does not have to oblige. A screen recorder writes ten frames in one second
and sixty in the next, while the container header still states a single number,
and the two counts have nothing to do with each other: our test file holds 350
source frames across ten seconds that a 60 fps timeline divides into 601.

So the frame to show is not computed, it is **looked up by time**: the last frame
whose timestamp is no later than N divided by the rate. Knowing that a frame is
the last one requires seeing the one after it — the frame duration stored in the
container is no help, Matroska fills it in from the header and states the same
value for every frame no matter how far apart they really are.

Reading one frame ahead is not free, and the measurement says where: sequential
reading and backwards stepping do not change, but every seek used to pay for one
extra decode — 3% on random jumps, because a threaded decoder does not hand back
frames one at a time. The way out is that most files do not need the lookahead at
all: when a frame lands exactly on the requested instant, nothing later can be a
better answer, and it is delivered immediately. Constant frame rate therefore
pays nothing, and variable frame rate pays one frame.

The same section fixed the other half of the problem. Not every file begins at
zero: a recording off a transport stream, or anything muxed with `-copyts`,
starts wherever it starts. Frames and audio samples are now both counted from one
common clip zero — the container's own start — so a genuine offset between
picture and sound is preserved rather than flattened. Before that, such a file
came apart completely: every request returned the very first frame, and the sound
was silence.

Both are checked by measurement rather than by eye. Two of the generated test
files carry a flash in the picture and a click in the sound placed at the same
instant; the distance between them on the way out is the desync, and a run fails
if they end up more than two frames apart.

### The frame cache

Measurement, not intuition, decided this. Reading forward costs 4 ms per frame;
stepping *backwards* cost 71 ms. An inter-frame codec has to return to the nearest
keyframe and decode forward from there, and OBS writes a keyframe every second,
so every step back decoded half a second of video and threw away the result that
would be needed a moment later.

The cache is therefore filled **only while moving backwards**. Reading forward is
already sequential, and caching there would waste memory for nothing. Frames are
moved out of GPU memory first (fifty 1440p frames will not fit in VRAM), the
budget is 256 MB, and the frames farthest from the current position are evicted.

### The log

`%LOCALAPPDATA%\AV1Importer\log.txt`, rewritten each time Premiere starts. It
records every selector Premiere sends, by name, **including the ones the plug-in
rejects**. The cause of failure hid among exactly those. Inside Premiere every
importer failure looks the same, so this log is the main debugging tool.

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
