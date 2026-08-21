# Aether 1.2.2

Two things this release is actually about: HDR footage now keeps its colours,
and frames are handed to the host on a second thread. Plus a small thing that
turns out to matter more than it sounds — the plug-in finally tells you which
version it is.

## HDR is declared, not flattened

Until now a BT.2020 PQ or HLG clip arrived in Premiere looking washed out: grey
where black should be, olive where yellow should be, brick where red should be.
The pixels were right; nobody had told the host what curve they were on, so it
read them as ordinary Rec.709 and drew them accordingly.

The fix is not tone mapping. Turning HDR into SDR is a creative decision and an
importer has no business making it for the editor — Premiere does it itself,
according to the sequence settings. What was missing was the plug-in saying what
it hands over, and that is what 1.2.2 adds, through `imGetIndColorSpace`:

| file | primaries | transfer |
|---|---|---|
| ordinary HD | 1 (BT.709) | 1 (BT.709) |
| BT.2020 PQ | 9 (BT.2020) | 16 (PQ) |
| BT.2020 HLG | 9 (BT.2020) | 18 (HLG) |

Those are ITU codes — the same numbers the stream itself carries, so nothing is
translated on the way. The matrix is declared **identity**: after the conversion
to RGB there is no matrix left and nothing for the host to undo.

Where a file carries no primaries they are now derived from the matrix rather
than guessed from the frame height. Declaring BT.601 primaries for a file with a
BT.709 matrix is nonsense, and that is what the first attempt did. The transfer
curve is never guessed at all — the difference between ordinary gamma and PQ is
the difference between a normal picture and a washed out one, and there is
nothing to infer it from.

Log curves travel the same path. Premiere knows the codes for S-Log, V-Log and
C-Log, and a file carrying them will have them passed on. That could not be
tested — no such footage was at hand.

Verified in Premiere Pro on a Rec.709 sequence: BT.2020 PQ and HLG bars come
through with their colours, where earlier versions produced the washed out
picture described above.

## Frames on a second thread

Premiere can ask an importer to work asynchronously: request a frame now, come
back for it later, and get on with something else meanwhile. 1.2.2 implements
that (`imCreateAsyncImporter`) — a separate importer with its own decoder, its
own worker thread and its own small queue.

The honest measurement, taken under a fake host kept deliberately busy between
frames, the way a real one is:

| decoder | gain |
|---|---|
| GPU (`av1_cuvid` on an RTX 5080) | 5–12% |
| CPU (`dav1d`) | nothing measurable |

That asymmetry is the whole story. On the GPU path the decode overlaps with the
host's own work; on the CPU path the decoder was already saturating the cores
and there is nothing left to overlap with.

A first attempt at this measurement showed the async path 3–12% *slower*, which
was the benchmark measuring its own memory allocator. A later one showed a 10%
speedup that turned out to be warm-up bias and vanished when the measurement
order was reversed. The numbers above are what survived both.

If it causes trouble it can be switched off without reinstalling: put
`async = off` in `%LOCALAPPDATA%\Aether\settings.ini`, or set `AETHER_ASYNC=0`
in the environment. The ordinary path stays in place and unchanged.

## The plug-in now says which version it is

Right-click `Aether.prm` → Properties → Details now shows `1.2.2`. Before this
there was nothing there at all, and the only way to tell one build from another
was the size in bytes — which cost an evening of this release's development,
testing a fix that was not actually installed.

The number lives in exactly one file, `src/AV1Version.h`. The installer no
longer keeps its own copy: it reads the version out of the compiled plug-in, so
the two cannot drift apart quietly.

## Test material for HDR

`tools\make-hdr-check.ps1` builds a small set for checking HDR by eye, since
that is the only way to check it: the same picture written five different ways,
including a **brightness ladder** — one clip carrying the same bars at 100, 203,
400 and 1000 nits, three seconds each.

The ladder exists because PQ carries brightness in absolute nits, so "convert
SDR to PQ" is not one operation but a family of them: one has to decide how many
nits white is worth. A hundred, as on an SDR studio monitor? Or 203, as BT.2408
prescribes for diffuse white in HDR? The host holds that answer. Whichever
segment matches the SDR reference is the assumption it makes.

The generator measures its own output rather than asserting it is correct. The
first version of this set silently produced four identical files: it used
zscale's `npl` option, which does nothing for PQ at all — zimg applies it only to
HLG. Tags were right, the picture looked plausible, ffprobe was satisfied, and
"1000 nit" did not differ from "100 nit" by a single code value. Only checking
against the SMPTE 2084 formula showed it. That check is now part of the script:

```
  100 nit   want ~ 509   got  529   ok
  203 nit   want ~ 573   got  593   ok
  400 nit   want ~ 639   got  656   ok
 1000 nit   want ~ 720   got  744   ok
```

## A test that had been lying

`plugin_test` checked that the FFmpeg libraries were loaded immediately after
the plug-in itself. That expectation was correct until 1.2.1, when loading them
was deliberately moved out of `DllMain` — calling `LoadLibrary` while the loader
lock is held is a documented way to deadlock a host. The check was never updated
and had been reporting a failure ever since.

It now verifies the actual intent, in both halves: that nothing is loaded while
the module is still loading, and that everything is loaded once the first real
request arrives.

## Known limitations

- No alpha channel for AV1 — it is stored differently from VP9's.
- No timecode from the file, no deinterlacing.
- Log curves (S-Log, V-Log, C-Log) are passed on but have never been seen in
  practice.
- Intel and AMD hardware decoding paths exist but have never been run.
- Windows only.

## Install

Download `AetherSetup-1.2.2.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version, including one under the old name; no need to uninstall first.

There is also **`Aether-1.2.2-portable.zip`** for anyone who would rather copy a
folder than run an unsigned installer from a stranger — a reasonable position.
Same files, no installer, nothing written to the registry; drop the `Aether`
folder into `…\Adobe\Common\Plug-ins\7.0\MediaCore\` and read `INSTALL.txt` in
the archive. It is reproducible: the same build produces a byte-identical zip,
so the checksum below is something you can verify rather than trust.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.2.2.exe        SHA256  c8c9048511d18a94680e43c849b5fa4dbf60a5304276748599d9728f09e8748a
Aether-1.2.2-portable.zip    SHA256  2cddce4fb6b07265d0ca0524b4d40acb3b50cb73825f0bd6f587692ae39ba4e1
```

The README has a section on what else there is to go on besides these numbers.
