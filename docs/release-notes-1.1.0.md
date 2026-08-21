# AV1 / VP9 Importer for Premiere Pro 1.1.0

The first release since 1.0.1, and it carries a lot: **VP9 alongside AV1**, Premiere
Pro support going back to **2019**, confirmed operation in **After Effects and Media
Encoder**, **10-bit** footage that arrives as 10-bit instead of being flattened to 8,
and a **settings window** for choosing the decoder.

The licence has also changed from MIT to **Apache 2.0**.

## VP9

Premiere cannot decode VP9 either, and VP9 usually arrives in WebM — a container
Premiere cannot even open. Both halves are handled by the plug-in now: it demuxes the
container itself and decodes the stream. That matters mostly for footage from the
web, where VP9 is what you are usually given.

Codec support is a table rather than a single hard-coded check, so adding the next
one is a few lines. The list stays deliberately short on purpose: the plug-in takes
priority over Premiere's own importer, and every extra codec means intercepting files
Premiere was opening perfectly well without it. Anything not on the list is handed
straight back.

## Premiere Pro 2019 and later

Until now this was only ever tried on 25.x. Two things stood in the way, and both
turned out to be about the surroundings rather than the API.

**The plug-in did not load at all in Premiere Pro CC 2019.** No message, no log
entry, and the same *"unsupported video compression type av01"* the plug-in exists
to remove. Premiere 2019 keeps its own `msvcp140.dll` — a Visual Studio 2015 build —
in its program folder, and Windows resolves DLLs by name against what is already in
the process. The plug-in was handed that old runtime and failed to initialise. It is
now linked with a static runtime and depends on no `msvcp140` at all, which also
means it no longer needs the Visual C++ redistributable on the machine.

**Frame caching was asked for by a version number that old hosts do not have.** The
suite was requested at version 8 and nothing else; a host that knows only version 7
hands back nothing, which looks exactly like the feature not existing. The plug-in
now asks for the newest version the host actually has and works without the suite
entirely if there is none. Premiere 2019 exercises this for real — it gives version
7 where 2025 gives 8.

Tested at both ends: **13.1.5** and **25.x**. 2020 through 2024 are untested but sit
between two versions that are known good, and the importer interface only moved from
21 to 24 across that whole span.

## After Effects and Media Encoder

The installer puts the plug-in in the folder the Adobe applications share, so all of
them see it. That was an assumption until it was checked; now the log names the host:

| | |
|---|---|
| After Effects 25.3.2 | import, four separate audio tracks, preview at 42 fps on 1440p |
| Media Encoder 25.6.4 | a full export: 1265 frames, no errors, 71 fps |

Rendering out of After Effects itself has still not been tried.

## 10-bit

A 10-bit source is now offered to the host as `BGRA_4444_16u`, with the 8-bit format
kept second so a host can fall back. Before this, 10-bit AV1 was decoded and then
flattened to 8 bits, which shows up as banding on gradients and skies.

The catch was not the code. In Premiere's 16-bit formats white sits at **32768**,
not 65535 — the convention After Effects and Photoshop use — so the output has to be
brought down from what FFmpeg produces. Getting that wrong would not crash anything;
it would quietly make every 10-bit clip twice as bright.

The wider output costs real time, and it is the colour conversion that pays:

| 2560x1440@60 | Total | Colour |
|---|---|---|
| 8-bit out | 4.8 ms — 208 fps | 3.9 ms |
| 16-bit out | 16 ms — 62 fps | 15.2 ms |

Only 0.3 ms of that belongs to the range conversion; the rest is FFmpeg's own work.
**Nothing changed for 8-bit files** — same single format offered, and the same 205
frames per second measured before and after.

## Choosing the decoder

There is now an **AV1 Importer — settings** entry in the Start menu. Three choices:
automatic, CPU, or GPU.

The default is the CPU, and that is a measurement rather than a preference. On a
2560x1440@60 file `libdav1d` runs at 800 fps against 130 for `av1_cuvid`, because
Premiere needs frames in system memory and the GPU path has to pay for a copy out of
video memory that the CPU path never makes. On a weak CPU the balance flips, which
is what the automatic setting is for.

It is a separate window rather than a dialog inside Premiere on purpose: the setting
matters most when a graphics driver fault stops Premiere from starting, and inside
Premiere it would be out of reach exactly then.

## Licensing

The project's own code moves from MIT to **Apache License 2.0**, with a `NOTICE`
file shipped alongside. Apache 2.0 grants patent rights explicitly instead of
leaving them to be inferred, and withdraws them from anyone who attacks the project
with patents. For a codec plug-in that is a live question — see the patent note in
`THIRD-PARTY-NOTICES.md`. Nothing MIT permitted has been taken away.

FFmpeg is still LGPL v3, unmodified, dynamically linked. The Adobe SDK is still not
redistributed in any form.

## Under the hood

Two new checks that exist because both caught real bugs:

- `host_test` drives the built plug-in from a fake host that can pretend to be
  older — one that knows only version 7 of the frame-cache suite, one from the
  Premiere 13.0 era, one that refuses the suite altogether. Every profile has to
  deliver identical pixels. Reverting the version fix makes two of the five fail.
- `plugin_test` reads the import table out of the `.prm` and fails if `MSVCP*` or
  `VCRUNTIME*` appear there. The machine that builds the plug-in is the one machine
  where that bug cannot be observed.

## Known limitations

- HDR: the picture is decoded, but transfer metadata is not passed on.
- No alpha channel.
- Long recordings have not been tested; runs so far are minutes, not hours.
- Windows only.
- Intel and AMD hardware decoding paths exist but have never been run.

## Install

Download `AV1Importer-Setup-1.1.0.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

The installer is not code-signed, so Windows will warn about an unknown publisher.
Verify the download instead:

```
SHA256  47538f93c9bf30e17195e7610b84a8003ee2bc28dc0c0c86cf9760fe1bc2ed8d
```
