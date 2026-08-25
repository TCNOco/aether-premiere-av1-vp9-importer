# Aether 1.3.0

A reliability release. Almost nothing here is a new feature; what changed is
how the plug-in behaves when something is wrong — a broken install, two threads
arriving at once, a filename that is not in the Latin alphabet, a recording ten
hours long. Four of the fixes are visible in ordinary use: settings stop
disappearing, a damaged install no longer takes Premiere down with it,
scrubbing at reduced quality is a third faster, and the panel works from the
keyboard.

## Settings stopped disappearing

Changing the decoder rewrote `settings.ini` from scratch. Anything else in it —
`async = off`, `yuv = off`, a comment written by hand from an issue thread — was
gone.

Now one line is replaced and the rest is left alone, duplicate keys are
collapsed so two readers cannot disagree about which one wins, and the file is
written beside itself and moved into place, so an interrupted write cannot
leave a half-empty file behind.

## A broken install no longer closes Premiere

The FFmpeg libraries are delay-loaded. If they are missing, nothing happens
until the first frame is decoded — and then the loader raises an exception that
ends the whole Adobe process. What the user sees is Premiere vanishing, with
nothing in any log to connect it to a plug-in.

The libraries are now checked once, before anything else runs. If they cannot be
loaded the importer disables itself and says so. A damaged install looks like a
plug-in that does not work, which is what it is.

They are looked for beside the plug-in first and by the ordinary search path
second. Only when both fail is the runtime called incomplete — the delay loader
searches the same way, so this tests the thing that actually matters.

## Two threads, one error message

Video and audio are decoded independently, on purpose: conforming must not
queue behind a stream of frames. They each have their own lock. The **text of
the last error** had neither — it was one string, written from under both.

Two threads assigning to one `std::string` is not "last one wins". A short value
lives inside the object and a long one on the heap, and on the boundary between
them one thread frees the buffer the other is filling. From outside that is heap
corruption inside Premiere: it closes by itself, with nothing in the log.

The error text now has its own lock, and audio has its own message. That second
part is not tidiness — conforming failures used to be logged with whatever
error video had produced a millisecond earlier in another thread.

## The log is readable whatever the file is called

The path was printed with `%S`, which MSVC converts through the C locale, and
that locale knows ASCII only. Measured:

```
1 ascii  [C:\Users\hadeg\file.mp4] end     <- 39 characters, fine
2 cyrill [F:\                              <- stops, returns -1
```

It stopped at the first non-Latin character and did not even write the newline,
so the next entry was glued onto the stump. That line — which file was asked
for — is the one thing worth having when a file will not import, and for anyone
whose footage is not in Latin folders it was never there.

It is now written as UTF-8, and the home directory is replaced with
`%USERPROFILE%`: the path stays useful, the user name does not travel with it.

No check could have caught this, because every test file in the project was
named in ASCII, and the test programs took their arguments through a narrow
`main` — which on Windows arrives in the system code page, not UTF-8. They were
built so that this road could not be walked. They use `wmain` now, and the
media set has a file under a Cyrillic path with a space in it.

## Scrubbing at reduced quality

Premiere sends the requested render quality with every frame. The plug-in had
never read it, so a draft scrub and a final export scaled the frame with the
same care.

Measured on 1440p, 120 frames, four runs in both orders, best taken:

| output size | bicubic | fast bilinear | |
|---|---:|---:|---:|
| 1280x720 | 326.3 ms | 227.8 ms | **-30.2%** |
| 640x360 | 269.8 ms | 211.6 ms | **-21.6%** |
| 2560x1440 (as in the file) | 214.6 ms | 215.0 ms | +0.2% |

The last row matters more than the first two: at full size there is nothing to
scale and the flag means nothing at all. The finished frame is untouched by
this; only the case Premiere sends `Draft` for pays anything.

## Also fixed

- **Clips longer than about ten hours.** At 59.94 fps the duration field
  overflowed past 2 145 000 frames and went negative. It now saturates and says
  so in the log.
- **The asynchronous importer holds its own function suites.** It was copying
  pointers from the ordinary importer, which releases those suites when it
  closes — and the SDK says explicitly that the two have separate lifetimes.
- **Suites are released only if they were acquired.** A host that refused one
  would have had its reference count driven below zero.
- **Opening a file is locked per instance,** not once for the whole plug-in.
  Forty-five files used to open one after another, each holding the lock through
  a full container probe.
- **An audio track's length comes from the fully parsed context.** Taken from
  the header alone it was about 25 ms short on a real recording — all of it at
  the end of the track.
- **Audio never writes more channels than the host allocated.**
- **A path too long for the buffer is refused and logged** instead of being cut
  silently and opened as whatever the cut path pointed at.
- **The host's list of frame formats is read to the end,** not just its first
  entry, which is what the SDK's "in order of preference" asks for.
- **Rotation is read and logged.** The frame is not rotated: the importer SDK
  has no field to declare rotation, so a vertical clip still lands sideways —
  but the log now says why instead of leaving it a mystery.

## Ten-bit frames: written, and switched off

Ten-bit files still go through BGRA64, and that is the most expensive path in
the plug-in: 15.95 ms per 1440p frame against 3.94 for eight-bit.

Delivering them as two planes instead would cost 0.42 ms — measured end to end
through the fake host at 1052 ms against 159 for 120 frames. The code is
written, checked, and **disabled by default**, because Premiere 26 accepts the
format and then gives no buffer to write into: `GetPixels` returns nothing for
a two-plane frame, `GetYUV420PlanarBuffers` covers only eight-bit three-plane
ones, and the SDK has no third way. Turned on, every frame fails and the
preview freezes.

It is kept rather than deleted so it can be tried on another version with one
line — `yuv10 = on` in the settings file — instead of a rebuild.

## Panel and build

- The panel announces its tabs and status to a screen reader and can be driven
  from the keyboard.
- The FFmpeg download and Adobe's signing tool are pinned by SHA256, and the
  signing tool by commit as well: it used to be fetched from a moving branch.
- The core is built and run under AddressSanitizer on every push.
- `release-check.bat` is a strict pre-release gate that, unlike the ordinary
  build script, is not allowed to skip the checks that need the Adobe SDK.
- New checks: six on the settings file, five on the duration boundaries, one on
  a non-Latin path, one comparing ten-bit planes against BGRA.

## Checks

Fourteen files through the core, ninety-six deliberately damaged ones, five host
profiles in the fake host, and the plug-in driven through all of them.

Verified in Premiere Pro 26.0 on Windows 11.

## Install

Download `AetherSetup-1.3.0.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

There is also **`Aether-1.3.0-portable.zip`**: `Aether` goes into
`…\Adobe\Common\Plug-ins\7.0\MediaCore\`, and `com.nehade.aether` into
`…\Common Files\Adobe\CEP\extensions\` if you want the panel. `INSTALL.txt` in
the archive spells it out.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.3.0.exe        SHA256  4780981e49516e19eba0da780a345732f90d005a664949e0ea3feb78f0249ee6
Aether-1.3.0-portable.zip    SHA256  d24d69a54bac6aaf7bb515141a46005e302128fac5d20f6c154ad4da418942db
```

The README has a section on what else there is to go on besides these numbers.
