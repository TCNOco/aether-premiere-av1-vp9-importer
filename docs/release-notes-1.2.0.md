# AV1 / VP9 Importer for Premiere Pro 1.2.0

Two bugs that made footage come out wrong rather than fail loudly, **transparency**,
**audio-only files**, **Premiere Pro 2026**, and an installer that no longer guesses
where Adobe keeps its plug-ins.

## A timeline frame is not a source frame

The plug-in worked out which frame to show with arithmetic: timestamp times frame
rate. That is correct exactly as long as the frame rate never varies — and a screen
recorder varies it constantly, writing ten frames in one second and sixty in the
next while the container header still states a single number.

On such a file the result was not a subtle drift. Half the requests in the first
half of a ten-second clip came back with **the very first frame of the recording** —
including at the five-second mark:

```
timeline 0.250 s   ->  frame from 0.300 s      picture ahead of the timeline
timeline 0.500 s   ->  frame from 0.000 s      the first frame of the file
timeline 5.000 s   ->  frame from 0.000 s      the same one, five seconds later
```

The wrong number was also used as the key into the frame cache, so the cache went on
confidently serving frames from the wrong part of the file no matter what the
decoder did.

The frame is now **found by time**: the last frame whose timestamp is no later than
the moment the host asked for. Knowing a frame is the last one means looking at the
one after it — the per-frame duration stored in the container is no help, Matroska
fills it in from the header and reports the same value for every frame however far
apart they really are.

That lookahead is not free, and the measurement disagreed with the intuition. "One
extra frame" cost **3% on random seeks**, because a threaded decoder does not hand
back frames one at a time. So the lookahead is skipped whenever a frame lands exactly
on the requested instant, which is every frame of a constant-rate file. Three runs
each, same file, same machine:

| Random seek | Runs | Mean |
|---|---|---|
| 1.1.0 | 448.2 / 449.8 / 449.7 | 449.2 ms |
| lookahead always | 464.5 / 463.4 / 460.4 | 462.8 ms |
| **1.2.0** | 451.0 / 451.8 / 447.0 | **449.9 ms** |

Sequential reading and backwards stepping did not move: 1507 fps against 1546, which
is inside the spread of the individual runs, and 15.27 ms per frame against 15.17.

## Files that do not start at zero

The other half of the same problem. Not every file begins at zero — a recording off a
transport stream, or anything muxed with `-copyts`, starts wherever it starts, and
each stream has its own beginning.

The plug-in did not look at that at all, and such a file came apart completely: every
frame request returned the first frame of the file, and the audio was silence from
end to end. No error appeared anywhere; the clip simply behaved like a still image
with no sound.

Frames and audio samples are now both counted from **one common clip zero**, the
container's own start. Counting each stream from its own beginning would have been
the other mistake: it flattens the real offset between picture and sound that the
file's author put there — AAC has one of its own, about 24 ms.

Sync is now measured rather than eyeballed. Two of the generated test files carry a
**flash in the picture and a click in the sound at the same instant**; the distance
between them on the way out is the desync, and a run fails if they end up more than
two frames apart.

```
sync.mp4         flash at 2.000 s, click at 2.020 s — 20 ms apart
sync_offset.mp4  flash at 2.033 s, click at 2.043 s — 10 ms apart
```

Both shifted by the same 24 ms of clip zero, and both still together.

## Transparency

VP9 in WebM can carry an alpha channel, and it hides in two places at once. It is not
in the video stream — the stream's pixel format says plain `yuv420p` — but attached
alongside each Matroska block, announced only by an `alpha_mode` tag on the track. And
of FFmpeg's decoders only `libvpx-vp9` returns it: the native `vp9` decoder drops it
silently, and every hardware decoder drops it too.

So the tag is read before a decoder is chosen, and a file that declares transparency
is forced onto the software path. Decoding it slower is better than losing it without
a word.

## Files with no video at all

A Matroska or WebM holding only audio now opens. Premiere cannot open those
containers at all, so without the plug-in that audio is simply unreachable.

The same rule as everywhere else decides it: an audio-only MP4 or M4A is **refused**,
because Premiere opens those perfectly well itself. A plug-in that grabs more than it
should is worse than one that grabs less — it breaks footage that used to work.

## Premiere Pro 2026

Confirmed working on **26.0.0.72**, with no change to the plug-in. The same build now
runs in Premiere Pro CC 2019, 2025 and 2026, After Effects and Media Encoder.

That is what asking the host for whichever suite versions it actually has buys: 2019
hands back version 7 of the frame-cache suite where 2025 gives 8, and 2026 needed
nothing new.

The README now carries one compatibility table covering applications, containers,
codecs, bit depth, transparency, timing, audio and hardware — including the rows that
say "not tried", because those are worth knowing too.

## The installer

It used to have the plug-in folder written into it as a fixed path. Adobe records
that address in the registry itself, so the installer now asks:

```
HKLM\SOFTWARE\Adobe\Premiere Pro\CurrentVersion\Plug-InsDir
HKLM\SOFTWARE\Adobe\<application>\<version>\CommonPluginInstallPath
```

The fixed path remains as a fallback. This is not cosmetic — with Adobe installed
somewhere other than the system drive, the old installer put the plug-in where
nothing would look for it.

A page before the install now lists the Adobe applications found on the machine, so
it is clear in advance which of them will pick the plug-in up. Those are found by
scanning folders rather than the registry, because Media Encoder writes no registry
key at all. Finding nothing is not an error either: the plug-in can be installed
first and is picked up when an application appears.

The same list goes into the setup log, which is the first thing worth having in a
report that says the plug-in never showed up.

Smaller things: the language is chosen on the first screen instead of being guessed,
the modal that appeared before the wizard is gone (Restart Manager closes running
applications anyway), and the installer, the entry in *Apps & features* and the
settings window all have an icon now.

## Under the hood

Core checks run on every push, on GitHub's Windows runners. Nothing is downloaded:
the test media is generated on the spot by FFmpeg from synthetic sources — including
the variable-rate file and the two flash-and-click sync files. Ten files, eight of
which must be accepted and pass every check, two of which must be **refused**.

The plug-in itself cannot be built there — it needs the Adobe SDK, which may not be
redistributed — so what is checked is the layer deliberately kept free of any Adobe
dependency: most of the code, and nearly all of the risk.

## Known limitations

- HDR: the picture is decoded, but transfer metadata is not passed on.
- No alpha channel for AV1 — it is stored differently from VP9's.
- No timecode from the file, no deinterlacing.
- Long recordings have not been tested; runs so far are minutes, not hours.
- Windows only.
- Intel and AMD hardware decoding paths exist but have never been run.

## Install

Download `AV1Importer-Setup-1.2.0.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

The installer is not code-signed, so Windows will warn about an unknown publisher.
Verify the download instead:

```
SHA256  e8dee0e218a275f73154f957b6bd407335058fbd6390d03b95134c058fe8b722
```
