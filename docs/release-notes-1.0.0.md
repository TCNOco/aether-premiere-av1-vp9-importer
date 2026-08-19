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
SHA256  027267320786ab1f38d7a70b40f62b769d137162ee09babe8b249024537395a7
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

MIT. Uses FFmpeg under LGPL v3 — unmodified BtbN builds, linked dynamically; the
licence text ships alongside the plug-in. Not affiliated with or endorsed by Adobe.
