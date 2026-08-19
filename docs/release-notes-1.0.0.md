# AV1 Importer for Premiere Pro 1.0.0

First working release. Drag an AV1 file onto the timeline and it works — no
transcoding, no proxies.

Premiere Pro 25.x cannot decode AV1: the import fails with *"File uses unsupported
video compression type av01"*. This plug-in adds the missing decoder.

## Install

Download **`AV1Importer-Setup-1.0.0.exe`** below, close Premiere Pro and Media
Encoder, and run it. Administrator rights are required — Adobe plug-ins live in a
shared system folder.

Windows will warn that the publisher is unknown: the installer is not
code-signed. Verify the download instead:

```
SHA256  ff9eb6f87c779d2c1e75a1e43f5b7031bb935cc69121878393a0e9a0de7ade7c
```

To remove it, use *Apps & features*.

## What's in it

- Video on the timeline, decoded on the GPU where the hardware allows
  (`av1_cuvid` on NVIDIA RTX 30 and newer; falls back to the CPU otherwise)
- **Multi-track audio kept separate** — OBS writes microphone, game, Discord and
  music as distinct streams, and they arrive as distinct tracks
- Scrubbing, seeking and export
- MP4, MKV, WebM, MOV, M4V

Files that are not AV1 are handed straight back to Premiere's own importer, so
nothing about existing footage changes.

## Performance

Measured on 2560x1440@60 AV1, RTX 5080:

| | |
|---|---|
| Sequential playback | 4 ms/frame — 250 fps, four times faster than real time |
| Step backwards | 5.9 ms/frame |
| Random seek | 75 ms |

## Requirements

Windows x64 · Adobe Premiere Pro 2025 (25.x)

Tested on 25.3. Other 25.x versions are likely fine; 24.x and earlier are untested.

## Known limitations

- **8-bit output only.** HDR and 10-bit AV1 are decoded but delivered as 8-bit,
  which will band on gradients.
- Only tested on recordings up to a few minutes. Hour-long files are untested.
- The installer is not code-signed, hence the SmartScreen warning.

## Licence

MIT — covering this project's own code only.

The Adobe SDK is **not** redistributed: no headers, no sample code, no other SDK
component is in the repository or in this release. Only the compiled plug-in, which
is our own object code, is distributed.

FFmpeg is used under LGPL v3 — unmodified BtbN builds, linked dynamically, licence
text shipped next to the plug-in and replaceable with your own build.

AV1 is royalty-free under the AOMedia Patent License 1.0, which covers only AOMedia
members' patents; third-party claims are possible. The plug-in only decodes and has
no encoding path. Details and the full notices are in `THIRD-PARTY-NOTICES.md`.

Adobe, Premiere Pro and Media Encoder are trademarks of Adobe Inc. Not affiliated
with, sponsored by, or endorsed by Adobe. Nothing here is legal advice.
