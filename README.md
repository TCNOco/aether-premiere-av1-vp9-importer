# AV1 Importer for Adobe Premiere Pro

Drag an AV1 file onto the timeline and it just works — no transcoding, no proxies.

[Русская версия](README.ru.md)

Premiere Pro 25.x **cannot decode AV1**. Its demuxer parses the container and
recognises the stream as `av01`, but there is no decoder, so the import fails with
*"File uses unsupported video compression type av01"*. This plug-in adds the missing
decoder, using FFmpeg and — where the hardware allows — the GPU.

That matters if you record with OBS: AV1 gives the same picture quality at a
noticeably lower bitrate, but the recordings were unusable in Premiere.

## Install

1. Download `AV1Importer-Setup-x.y.z.exe` from [Releases](../../releases).
2. Close Premiere Pro and Media Encoder.
3. Run it. Administrator rights are required — Adobe plug-ins live in a shared
   system folder.

To remove it, use *Apps & features* or the uninstaller in the plug-in folder.

**Requirements:** Windows x64, Adobe Premiere Pro 2025 (25.x). Hardware decoding
needs an NVIDIA GPU with AV1 support (RTX 30 series and newer); without one the
plug-in falls back to the CPU decoder automatically.

## What works

- Video on the timeline, hardware-decoded where available
- **Multi-track audio kept separate** — OBS writes microphone, game, Discord and
  music as distinct streams, and they arrive as distinct tracks
- Scrubbing, seeking and export
- MP4, MKV, WebM, MOV, M4V containers

Files that are *not* AV1 are handed straight back to Premiere's own importer, so
nothing about your existing footage changes.

One caveat about MKV and WebM: Premiere has no native support for those containers
at all, so the plug-in demuxes them itself. That works — but only for AV1 inside.
An MKV holding H.264 is refused by the plug-in and Premiere cannot open it either.

## Performance

Measured on 2560x1440@60 AV1, RTX 5080:

| | |
|---|---|
| Sequential playback | 4 ms/frame — **250 fps**, four times faster than real time |
| Step backwards | 5.9 ms/frame (71 ms without the frame cache) |
| Random seek | 75 ms — one keyframe interval, inherent to the codec |

Of those 4 ms, decoding takes 0.08 ms; the rest is moving the frame out of GPU
memory and converting colour. In other words the GPU is not the bottleneck, and
there is little left worth optimising.

## Building from source

Not needed to use the plug-in — only to modify it.

You will need, none of which are in this repository:

- **Adobe Premiere Pro C++ SDK** into `sdk/` — from
  [developer.adobe.com](https://developer.adobe.com/premiere-pro/), requires an Adobe ID
- **FFmpeg** into `ffmpeg/` — a BtbN `win64-lgpl-shared` build (headers, `.lib` and `.dll`)
- **Visual Studio Build Tools 2022** with the C++ workload
- **Inno Setup 6** — only to build the installer

```
build.bat                        the plug-in -> build\Release\AV1Importer.prm
build-test.bat                   the test programs
installer\build-installer.bat    the installer -> dist\
```

Two console programs let you test almost everything without launching Premiere,
which is what makes this project practical to work on:

```
build\decoder_test.exe <file.mp4>                    metadata, a frame, timings, audio levels
build\plugin_test.exe  build\plugin\AV1Importer.prm  plug-in loading and its answers
```

## How it works

`src/AV1Decoder.*` is the decoding core. It deliberately knows nothing about the
Adobe SDK, so it can be built and exercised by a console program — that removes
most of the debugging from the slow build-install-restart-Premiere loop.
`src/AV1Importer.*` is the SDK layer and only translates Premiere's requests into
calls on the core.

### Three conditions, without which Premiere ignores an importer — silently

Each one fails identically: the file will not open, exactly as if no plug-in were
installed. There is no message and no log entry.

1. **Priority 100 or higher.** Below that, Premiere handles MP4 itself and never
   reaches the plug-in. Only 100+ overrides Adobe's own importers.
2. **Answer `imGetSupports7/8`** with `malSupports7/8`. This is the handshake about
   which interface version the plug-in speaks. Without it Premiere reads the file
   info and walks away, never asking for a pixel format or a frame.
3. **`subType` must be `imUncompressed`**, not `av01`. The plug-in does the
   decoding and hands over ready pixels; saying `av01` sends Premiere off to look
   for a decoder it does not have — and it reports exactly the error you were
   trying to fix.

### Why FFmpeg is delay-loaded

Windows looks for dependent DLLs next to the *executable* — that is, next to
Premiere — never next to the DLL being loaded. With ordinary linking Premiere
would simply fail to load the plug-in, without a word. So `AV1Runtime.cpp` loads
FFmpeg from the plug-in's own folder at startup, and the libraries themselves are
linked with `/DELAYLOAD`, so by the time a real call happens they are already in
the process.

Two non-obvious build requirements follow:

- **whole-program optimisation must be off** — with it the linker silently ignores
  `/DELAYLOAD`;
- **the import libraries are regenerated** into `ffmpeg/lib-msvc`, because the ones
  shipped with FFmpeg are produced by a different toolchain and `/DELAYLOAD` does
  not work with them either (LNK4199):

```
lib /def:ffmpeg\lib\avcodec-62.def /name:avcodec-62.dll /out:ffmpeg\lib-msvc\avcodec.lib /machine:x64
```

### The frame cache

Measurement, not intuition, decided this. Reading forward costs 4 ms per frame;
stepping *backwards* cost 71 ms. An inter-frame codec has to return to the nearest
keyframe and decode forward from there, and OBS writes a keyframe every second —
so every step back decoded half a second of video and threw away the result that
would be needed a moment later.

The cache is therefore filled **only while moving backwards**. Reading forward is
already sequential, and caching there would waste memory for nothing. Frames are
moved out of GPU memory first (fifty 1440p frames will not fit in VRAM), the
budget is 256 MB, and the frames farthest from the current position are evicted.

### The log

`%LOCALAPPDATA%\AV1Importer\log.txt`, rewritten each time Premiere starts. It
records every selector Premiere sends, by name, **including the ones the plug-in
rejects** — the cause of failure hid among exactly those. Inside Premiere every
importer failure looks the same, so this log is the main debugging tool.

## Licence

MIT — see [LICENSE](LICENSE).

Uses FFmpeg under LGPL v3; the libraries are unmodified BtbN builds and are linked
dynamically. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Not affiliated with or endorsed by Adobe.
