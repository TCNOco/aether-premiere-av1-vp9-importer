# Aether 1.2.5

Frames now go to Premiere in the format the decoder already produced, instead
of being converted to RGB first. On a graphics card that is the difference
between 622 ms and 169 ms for the same 120 frames. Memory is down by two
thirds on a busy timeline, and a bug that reported the wrong audio track's
properties is fixed.

## Frames go over as they are

Premiere can take YUV 4:2:0 directly. It was never asked.

Everything the plug-in delivered used to be converted to RGB first, and that
conversion was **three quarters of the work** — decoding a 1440p frame took
0.08 ms, converting it took 3.94 ms. The conversion was ours, it was
avoidable, and it is now gone wherever the host accepts the native format.

Measured through the fake host, 120 frames of 1440p, the asynchronous path
(which is the one Premiere actually uses):

| path | host idle | host busy 3 ms | host busy 12 ms |
|---|---:|---:|---:|
| graphics card, native YUV | **169.4 ms** | 414.9 | 1498.5 |
| processor, native YUV | 211.3 | **412.7** | **1485.3** |
| processor, BGRA | 231.7 | 464.8 | 1544.1 |
| graphics card, BGRA | 622.0 | 865.1 | 1960.8 |

Confirmed in a real Premiere 26.0, not only on the bench — the plug-in log
reads `async frame 0 delivered (2560x1440, planar YUV)`.

**The default decoder has not changed, and that is a separate finding.** The
reason it was set to the processor in 1.2.0 — "the graphics card is six times
slower" — is no longer true: that 622 became 169. But look along the rows. The
moment the host is doing anything between frames, and it always is, the two
come within one percent of each other. The setting is no longer a trap, which
is the real change; it is not a reason to flip the default.

Three kinds of file keep the old path, deliberately: 10-bit (Premiere only has
two-plane formats for those), files with alpha (YUV has nowhere to put it),
and 8-bit BT.2020 (Premiere has no constant for it, and silently calling it
BT.709 would be a lie about the colour). Reduced-size preview frames also come
across as BGRA — the host asks for them that way.

Turn it off with `yuv = off` in the settings file or `AETHER_YUV=0`.

## Memory on a busy timeline

The frame cache budget was worked out per clip and had no overall ceiling.
Premiere keeps a decoder open for every clip on the timeline, so the number
multiplied. Peak working set, ten 1440p clips:

| | before | after |
|---|---:|---:|
| ten clips | **4371 MB** | **1402 MB** |

Two changes got there. The cache now has a ceiling for the whole process,
512 MB, divided between the live decoders; and each decoder no longer opens
with as many threads as the machine has cores, which was as many threads as
the machine has cores *per clip*.

Threads were divided rather than capped, because the measurement said capping
would cost more than it saved: a single thread already decodes 1440p at 248
frames per second, four times faster than playback needs. Full speed matters
for export, not for the timeline. So one clip still gets every core, and ten
clips share them.

Set `AETHER_CACHE_MB` to change the ceiling, or to 0 to remove it.

## Audio tracks were described by the first one

Premiere discovers streams by asking about them one after another on the same
handle. The plug-in checked "is audio open?" without checking **which track**
was open, so from the second question onward it answered with track 0's
channel count, sample rate and length.

On a recording where every track has the same shape — an OBS capture, for
instance — this is invisible. On a file whose tracks differ it was wrong, and
the test media now includes such a file: stereo for ten seconds, mono for six.

## The log can be read while Premiere is running

It could not before. The log was opened for exclusive use, so for as long as
Premiere was open the one file you would want to attach to a bug report could
not be read, copied, or moved — which is to say it was unavailable exactly
when it was needed.

It is also no longer drowned. Conforming a five-minute audio track asks for it
in 3520 pieces, and the log wrote a line for every one; four tracks made
fourteen thousand lines per import, and a real failure was somewhere inside
them. Now the beginning is written in full, then one line per two hundred
requests — and every failure, always, with the track, the request number and
the position.

## Also

- **Writing past the end of an audio buffer is now impossible.** The bounds
  held under every assumption that could be checked, but an assumption that
  fails inside Premiere does not produce an import error, it produces a closed
  Premiere.
- **Opening a decoder is under a lock.** Premiere calls one importer instance
  from several threads at once — conforming on one, playback on another — and
  the lazy open was a check-then-act with nothing guarding it.
- **MKV and WebM were verified inside Premiere.** The README has promised them
  since 1.0.0; until now they had only ever been read by the core.
- The build server no longer follows moving versions. The runner image, the
  FFmpeg build and the actions are pinned; three runs went red without a line
  of code changing, and that is a check reporting the weather.

## Checks

Thirteen files through the core, sixteen deliberately damaged files, five host
profiles in the fake host, and four new checks: that the planes carry the same
picture as BGRA, that audio survives a conform racing playback, that a stream
is described the same way however it was reached, and that reading the same
audio twice agrees relative to the signal rather than to a fixed number.

Verified in Premiere Pro 26.0 on Windows 11.

## Install

Download `AetherSetup-1.2.5.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

There is also **`Aether-1.2.5-portable.zip`**: `Aether` goes into
`…\Adobe\Common\Plug-ins\7.0\MediaCore\`, and `com.nehade.aether` into
`…\Common Files\Adobe\CEP\extensions\` if you want the panel. `INSTALL.txt` in
the archive spells it out.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.2.5.exe        SHA256  fc7a4a31379f23003597eccd130ab42f520c391605be4e4eb141804587413319
Aether-1.2.5-portable.zip    SHA256  54264140bef262959af18f47e6a7195af988df16d88c2b6727b75d22ce07d489
```

The README has a section on what else there is to go on besides these numbers.
