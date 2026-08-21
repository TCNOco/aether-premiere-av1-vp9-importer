# Aether 1.2.1

The project has a name: what was "AV1 / VP9 Importer" is now **Aether**. Along
with that, a colour fix that changes how every clip looks, a log that stopped
costing a third of the decoder's work, and a new test layer that feeds the core
deliberately broken files.

## The name

The rename goes all the way down — the plug-in file, the folder it installs into,
the entry in *Apps & features*, the settings window, the log, and the name
Premiere shows in its list of formats:

| | before | now |
|---|---|---|
| Installer | `AV1Importer-Setup-x.y.z.exe` | `AetherSetup-1.2.1.exe` |
| Plug-in | `MediaCore\AV1 Importer\AV1Importer.prm` | `MediaCore\Aether\Aether.prm` |
| In Premiere's format list | AV1 / VP9 Video (ffmpeg) | Aether - AV1 / VP9 (ffmpeg) |
| Start menu | AV1 Importer — settings | Aether — settings |
| Log and settings | `%LOCALAPPDATA%\AV1Importer\` | `%LOCALAPPDATA%\Aether\` |

Renaming an installed folder is the kind of change that quietly leaves the old
copy behind, and two importers in `MediaCore` both claiming `av01` would compete
for every file — with the winner decided by whatever order Premiere happens to
walk the folder in. Nobody could diagnose that from the outside. So the installer
removes the previous installation before writing the new one and keeps the same
application id, so *Apps & features* shows one entry rather than two. Verified by
installing the previous version and then this one over it.

The decoder setting — CPU or GPU — moves across on its own: the plug-in copies it
once if the new settings folder is empty while the old one is not.

Names inside the source code deliberately did not change. Files are still
`AV1Decoder.cpp`, `AV1Importer.cpp`, the namespace is still `av1imp`. Renaming
those would touch every file, change nothing a user can observe, and bury the
history of those files under a rename commit.

## Colours change with this update, and they change to the right ones

**Read this before updating a project in progress.** Footage will look slightly
different after installing 1.2.1. Nothing regressed — the previous versions were
converting colour with the wrong matrix, and this release corrects it.

Luma and chroma become RGB through a matrix, and there is more than one: BT.601
for standard definition, BT.709 for HD, BT.2020 for wide gamut. The plug-in never
chose one, so FFmpeg's scaler fell back to its default, BT.601 — which means
everything shot in BT.709, that is to say nearly all HD footage, came out wrong.
This was never limited to HDR.

Measured on a flat `E04030` frame tagged BT.709:

| | |
|---|---|
| FFmpeg told to use BT.709 | `DC3E2C` |
| FFmpeg told to use BT.601 on purpose | `CE2F2E` |
| the plug-in, 1.2.0 and earlier | `D02F30` |
| the plug-in, 1.2.1 | `DD3F2D` |

Landing on the deliberately-wrong answer was the diagnosis. The single unit of
difference now is the round trip through the codec, not the matrix. In practice
the old error was worth about 15 units of 255 on red and green — visible on
saturated colours and on skin.

The description is taken in order: what the frame says, what the container says,
then a guess from the frame height. The middle step is not decoration — SVT-AV1
does not write the colour description into the stream at all, so its files
always arrive marked "unspecified", and the first attempt at this fix changed
nothing whatsoever on them.

Full and limited range are now honoured the same way. What is still not handled
is the transfer curve: HDR arrives with the correct BT.2020 matrix but still in
PQ or HLG, and turning that into SDR is not something this plug-in attempts.

A check was added so this cannot come back quietly, and it was verified by
removing the fix: the check fails with "off by 15" and passes again once the fix
is restored.

## The log was costing a third of the decoding

Every line used to open the file, write, and close it again — deliberately, so a
crash could not lose the log. The intention was right, the price was not:

| | per line |
|---|---|
| open, write, close | 117 µs |
| held open, flushed | 5 µs |
| held open, not flushed | 0.4 µs |

Lines are not rare: at least two for every frame delivered, against 650 µs of
actual decoding for a 1440p frame. The file is now held open and flushed after
every line — which keeps exactly the crash-safety the old arrangement was for,
at twenty-four times less.

## Damaged files

A new tool feeds the decoding core truncations, noise, flipped bits, holes in
the middle, and headers with garbage behind them. One thing is checked: that the
process reaches the end. Refusing to open such a file is perfectly legitimate;
taking the host down is not — and that is not hypothetical, since a frame with
no dimensions once reached the scaler, which answers that with `av_assert0` and
the end of the process rather than an error.

The damage follows fixed rules from a given seed, so a failure reproduces word
for word. Six runs across three container types are now part of every push.

The very first run did find a crash — in the new tool itself, which had sized a
buffer for 8-bit output and then asked for 16-bit. That counts as a result too:
it proves the thing can catch what it is looking for. Twenty-one runs across six
formats and seven seeds have since found nothing in the decoder.

## FFmpeg no longer loads from DllMain

The plug-in used to load its FFmpeg libraries while Windows was still loading the
plug-in itself. That is explicitly against the rules: the loader lock is held at
that moment, and a library being loaded wants the same lock for its own
initialisation. It worked in all five Adobe applications, which is luck rather
than a guarantee.

Loading now happens on the first request Premiere actually sends. The only thing
left in `DllMain` is reading the plug-in's own path, which touches nothing.

## Smaller corrections

- A string-copy helper overflowed its buffer if it was ever given a zero-length
  destination. Both callers pass a fixed size, so it was unreachable — and now it
  is impossible.
- The frame request read the host's first pixel format without checking that the
  host had sent any.
- The row length the host reports is now checked against the frame width before
  anything is written into it.

## Known limitations

- HDR: the matrix is honoured, the PQ/HLG transfer curve is not converted.
- No alpha channel for AV1 — it is stored differently from VP9's.
- No timecode from the file, no deinterlacing.
- Long recordings have not been tested; runs so far are minutes, not hours.
- Windows only.
- Intel and AMD hardware decoding paths exist but have never been run.

## Install

Download `AetherSetup-1.2.1.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version, including one under the old name; no need to uninstall first.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
SHA256  bb16f94a3a66d47e3837c64115f83e658e31f5b944b509794e6204071f36cf8b
```
