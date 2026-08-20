# AV1 / VP9 Importer for Premiere Pro 1.1.0

**VP9 is now supported**, alongside AV1.

Includes everything from 1.0.1, which fixed a buffer overrun that could corrupt
memory inside Premiere — if you are still on 1.0.0, this is the upgrade to take.

## New: VP9

Premiere cannot decode VP9 either, and VP9 usually arrives in WebM — a container
Premiere cannot even open. Both halves are now handled by the plug-in: it demuxes
the container itself and decodes the stream, on the GPU where the hardware allows
(`vp9_cuvid` on NVIDIA; Intel and AMD paths exist but are untested).

That matters mostly for footage from the web, where VP9 is what you are usually
given.

Measured on the same 2560x1440@60 material, RTX 5080:

| | AV1 | VP9 |
|---|---|---|
| Sequential playback | 210 fps | 207 fps |
| Step backwards | 5.6 ms/frame | 6.3 ms/frame |

Codec support is now a table rather than a single hard-coded check, so adding the
next one is a few lines. The list stays deliberately short: the plug-in takes
priority over Premiere's own importer, and every extra codec means intercepting
files Premiere was opening perfectly well without it. Anything not on the list is
still handed straight back.

## Unchanged

AV1 behaves exactly as before — verified by re-running the full check suite against
the same footage. Multi-track audio, the frame cache for backward scrubbing, and
export all work as they did.

Known limitations are also unchanged: 8-bit output only (HDR and 10-bit are decoded
but delivered as 8-bit), and long recordings remain untested.

## Install

Download `AV1Importer-Setup-1.1.0.exe`, close Premiere Pro and Media Encoder, run
it. Administrator rights are required. Installs over any earlier version; no need
to uninstall first.

The installer is not code-signed, so Windows will warn about an unknown publisher.
Verify the download instead:

```
SHA256  d7b0b48dd4bce09b9604f7efee5c324cfe974d5235a45b3581241babe76ebb1b
```
