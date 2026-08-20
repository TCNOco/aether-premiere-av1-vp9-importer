# AV1 Importer for Premiere Pro 1.0.1

Bug-fix release. Upgrade over 1.0.0 — one of these could corrupt memory inside
Premiere.

## Fixed

**Reduced-quality playback could overrun the frame buffer.** Premiere asks for a
smaller frame when playback quality is set below full; the buffer it hands over is
that smaller size, but the plug-in always wrote full resolution into it. Frames are
now scaled to the requested size. This never fired in testing only because Premiere
happened to ask for full-size frames — at 1/2 or 1/4 quality it would have.

**The same audio range read twice returned different samples.** AAC windows
overlap, so the first frames after a seek are inaccurate, and how many the decoder
discards depends on which packet it landed on. Seeking now starts 200 ms early and
decodes up to the target, which makes the result exact and repeatable. Audible as
drift or wrong audio when scrubbing.

**Hardware decoding on non-NVIDIA machines.** A CUDA device was created for the
Intel and AMD decoders too; without NVIDIA hardware that silently produced no
device context. Each decoder now gets its matching device type, and one that is
unavailable is skipped rather than opened without a context. Note that only the
NVIDIA path has actually been tested.

**Frame cache size now follows the resolution.** It was a fixed 256 MB, which holds
about a second of 1440p but only a fifth of a second of 4K. The budget is now
computed from the frame size (roughly one second of video, between 128 and 512 MB).

**Variable frame rate no longer shifts frame numbering.** Frame indices derived
from timestamps are checked for monotonicity, falling back to sequential counting
when a file's timing is irregular or broken.

## Changed

- Error and log messages are in English — the log is what gets pasted into issues.
- Colour conversion uses bicubic rather than bilinear; this only matters now that
  frames are actually scaled.
- `decoder_test` runs correctness checks and fails the run if any breaks: a cached
  frame against a freshly decoded one, buffer bounds at reduced size, and audio
  repeatability. The audio bug above was found by these checks, not by hand.

## Unchanged

Everything from 1.0.0 works as before: hardware decoding, multi-track audio kept
separate, the frame cache for backward scrubbing, and export.

Known limitations are also unchanged — 8-bit output only (HDR and 10-bit AV1 are
decoded but delivered as 8-bit), and long recordings remain untested.

## Install

Download `AV1Importer-Setup-1.0.1.exe`, close Premiere Pro and Media Encoder, run
it. Administrator rights are required. The installer is not code-signed, so
Windows will warn about an unknown publisher — verify the download instead:

```
SHA256  16ad77b4e945c32165b9f2b0f391f3351b86bd0332f1522659e6c123ae9cf377
```

Download `AV1Importer-Setup-1.0.1.exe`, close Premiere Pro and Media Encoder, run
it. Administrator rights are required. Installing over 1.0.0 replaces it; no need
to uninstall first.
