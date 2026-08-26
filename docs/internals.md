# How Aether works

Implementation notes: the parts of the Premiere importer API that are not
written down anywhere, and the bugs that came of them.

None of this is needed to use the plug-in — see the
[README](../README.md) for that. It is here because every section below is
a day somebody lost, written down so that nobody loses it twice.

`src/AV1Decoder.*` is the decoding core. It deliberately knows nothing about the
Adobe SDK, so it can be built and exercised by a console program, which removes
most of the debugging from the slow build-install-restart-Premiere loop.
`src/AV1Importer.*` is the SDK layer and only translates Premiere's requests into
calls on the core.
`src/PreviewCache.*` is the Adobe-independent persistent cache for reduced BGRA8
previews.

## What is in here

- **Talking to Premiere**
  - Three conditions Premiere never tells you about
  - Adobe's 16-bit is not the obvious 16-bit
  - A timeline frame is not a source frame
- **Colour**
  - Colour uses the matrix the file names, not a guess
  - HDR is declared, not flattened
- **Alpha**
  - Alpha hides in two places at once
- **Speed and hardware**
  - Asynchronous delivery, and who it actually helps
  - Hardware decoding on AMD does not go the way it looks like it should
  - The frame cache
- **Loading and building**
  - Why the C++ runtime is linked statically
  - Why FFmpeg is delay-loaded
- **Around the plug-in**
  - The panel has to be signed
  - The log
- **Testing without Premiere**

## Talking to Premiere

### Three conditions Premiere never tells you about

Each one fails identically: the file will not open, exactly as if no plug-in were
installed. There is no message and no log entry.

1. **Priority 100 or higher.** Below that, Premiere handles MP4 itself and never
   reaches the plug-in. Only 100+ overrides Adobe's own importers.
2. **Answer `imGetSupports7/8`** with `malSupports7/8`. This is the handshake about
   which interface version the plug-in speaks. Without it Premiere reads the file
   info and walks away, never asking for a pixel format or a frame.
3. **`subType` must be `imUncompressed`**, not `av01`. The plug-in does the
   decoding and hands over ready pixels; saying `av01` sends Premiere off to look
   for a decoder it does not have, and it reports exactly the error you were
   trying to fix.

### Adobe's 16-bit is not the obvious 16-bit

In Premiere's `_16u` pixel formats white sits at **32768**, not at 65535 — the same
convention After Effects and Photoshop use, stated on page 64 of the SDK guide.
FFmpeg produces the full 0–65535 range, so every component has to be brought down
afterwards. `(v + 1) >> 1` does it exactly at both ends: 65535 becomes 32768 and 0
stays 0.

Getting this wrong would not crash anything. It would just make every 10-bit clip
twice as bright and clipped, which is the kind of bug that survives a long time.

`decoder_test` checks both halves on a 10-bit file: that the output carries more
than 256 distinct values per channel — otherwise it is 8-bit data in a wider buffer
— and that nothing exceeds 32768.

### A timeline frame is not a source frame

Premiere asks for frame number N of a timeline that runs at a fixed rate. A
source does not have to oblige. A screen recorder writes ten frames in one second
and sixty in the next, while the container header still states a single number,
and the two counts have nothing to do with each other: our test file holds 350
source frames across ten seconds that a 60 fps timeline divides into 601.

So the frame to show is not computed, it is **looked up by time**: the last frame
whose timestamp is no later than N divided by the rate. Knowing that a frame is
the last one requires seeing the one after it — the frame duration stored in the
container is no help, Matroska fills it in from the header and states the same
value for every frame no matter how far apart they really are.

Reading one frame ahead is not free, and the measurement says where: sequential
reading and backwards stepping do not change, but every seek used to pay for one
extra decode — 3% on random jumps, because a threaded decoder does not hand back
frames one at a time. The way out is that most files do not need the lookahead at
all: when a frame lands exactly on the requested instant, nothing later can be a
better answer, and it is delivered immediately. Constant frame rate therefore
pays nothing, and variable frame rate pays one frame.

The same section fixed the other half of the problem. Not every file begins at
zero: a recording off a transport stream, or anything muxed with `-copyts`,
starts wherever it starts. Frames and audio samples are now both counted from one
common clip zero — the container's own start — so a genuine offset between
picture and sound is preserved rather than flattened. Before that, such a file
came apart completely: every request returned the very first frame, and the sound
was silence.

Both are checked by measurement rather than by eye. Two of the generated test
files carry a flash in the picture and a click in the sound placed at the same
instant; the distance between them on the way out is the desync, and a run fails
if they end up more than two frames apart.

## Colour

### Colour uses the matrix the file names, not a guess

Luma and chroma become RGB through a matrix, and there is more than one: BT.601
for standard definition, BT.709 for HD, BT.2020 for wide gamut. Picking the
wrong one misses the colour on all of the footage at once.

Which is exactly what happened here. Left to itself `swscale` takes BT.601, so
everything shot in BT.709 — which is nearly all HD — came out wrong. On a flat
`E04030` frame:

| | |
|---|---|
| ffmpeg told to use BT.709 | `DC3E2C` |
| ffmpeg told to use BT.601 on purpose | `CE2F2E` |
| the plug-in, before | `D02F30` |
| the plug-in, after | `DD3F2D` |

Landing on the deliberately wrong answer was the diagnosis. The single unit of
difference afterwards is the round trip through the codec, not the matrix.

Then the other half turned up. A frame does not always know its own colour:
SVT-AV1 does not write the description into the stream, so the frame says
"unspecified" — and on such a file the first attempt at this fix changed
nothing at all. The description is therefore taken in order: what the frame
says, what the container says, and only then a guess from the frame height
(576 lines or fewer means standard definition and BT.601).

The matrix is handed to the scaler on every frame rather than once. That is
deliberate: `sws_getCachedContext` is free to rebuild the context without
telling us, and any saving here would mean a frame quietly converted with the
wrong one. It costs nothing measurable — colour conversion stayed at 0.62–0.65
ms per 1440p frame, the same as before.

What this does not fix is the transfer curve. HDR now arrives with the right
matrix but still in PQ or HLG, and turning that into SDR is not something this
plug-in attempts.

### HDR is declared, not flattened

Turning HDR into SDR is a creative decision, and an importer has no business
making it for the editor. Premiere does that itself, according to the sequence
settings; our job is to say honestly what is in the pixels we hand over.

What is in them is full-range RGB with the **original** transfer curve and the
**original** primaries: the only thing applied is the luma-chroma matrix. That
is what gets declared through `imGetIndColorSpace`, as the same ITU codes the
stream itself carries:

| file | primaries | transfer |
|---|---|---|
| ordinary HD | 1 (BT.709) | 1 (BT.709) |
| BT.2020 PQ | 9 (BT.2020) | 16 (PQ) |
| BT.2020 HLG | 9 (BT.2020) | 18 (HLG) |

The matrix is declared **identity**: after the conversion to RGB there is no
matrix left, and nothing for the host to undo.

Where the file carries no primaries, they are derived from the matrix rather
than guessed separately from the frame height — declaring BT.601 primaries for
a file with a BT.709 matrix is nonsense, and that is exactly what the first
version did. The transfer curve is never guessed: if the file does not name
one, the ITU code for unspecified (2) is passed through. Inventing BT.709
here used to tag HDR-without-metadata as SDR. The difference between ordinary
gamma and PQ is the difference between a normal picture and a washed out one,
and there is nothing to infer it from.

Log curves travel the same way: Premiere knows the codes for S-Log, V-Log and
C-Log, and if a file carries them they are passed on. That could not be tested
— no such footage was at hand.

**What this does not promise.** That the declaration is correct has been
checked; that Premiere then does the right thing with it is something only
Premiere can show. Judging that takes a pair of eyes, so the material to judge
by is built by a script of its own:

```
tools\make-hdr-check.ps1      four files carrying the same picture
```

Only the encoding differs: ordinary SDR, the same frame in BT.2020 PQ, the same
in HLG, and the same again as a brightness ladder.

The ladder is the important one. PQ carries brightness in **absolute nits**, so
"convert SDR to PQ" is not one operation but a family of them: one has to decide
how many nits white is worth. A hundred, as on an SDR studio monitor? Or 203, as
BT.2408 prescribes for diffuse white in HDR? The host holds that answer, not us,
and guessing is pointless. So one file carries four segments of the same picture
converted differently; whichever segment matches the reference is the assumption
the host makes.

The script takes nothing on trust here — and measures two separate things.

First, whether the brightness asked for actually landed. What is expected is not
"looks about right" but the specific codes from the SMPTE 2084 formula.

```
  100 nit   want ~ 509   got  529   ok
  203 nit   want ~ 573   got  593   ok
  400 nit   want ~ 639   got  656   ok
 1000 nit   want ~ 720   got  744   ok
```

That check is not decoration. The first version of this set used the `npl`
option, which does nothing for PQ at all — zimg applies it only to HLG. Every
file came out identical, with correct tags and a plausible look: "1000 nit" did
not differ from "100 nit" in any way. Neither the eye nor ffprobe showed it;
only measuring the code did. A plain gain in linear light works instead.

Second, whether a converted file really must match the reference. Here the
conversion is run backwards.

```
02-pq-100nit.mkv   understood  37.9   ignored  15.6
04-hlg.mkv         understood  39.1   ignored  17.4
```

Around 38 dB is "the eye sees no difference"; around 16 dB is the washed out
picture. Twenty decibels apart — hard to miss. Two frames are written alongside
for comparison: `expected.png` and `if-ignored.png`.

No HDR display is needed: on a Rec.709 sequence the host flattens HDR to SDR
itself, and it is that result one should be looking at.

## Alpha

### Alpha hides in two places at once

A VP9 file with transparency will quietly arrive fully opaque unless two separate
things go right, and neither of them announces itself.

**The alpha is not in the video stream.** Matroska carries it as an extra block
attached to each frame and marks the track with an `alpha_mode` tag. The stream's
own pixel format still reads `yuv420p`, so asking the format whether there is
transparency gives the wrong answer. The tag is the only honest source.

**Only one of the two VP9 decoders returns it.** FFmpeg's native `vp9` decoder
drops the alpha silently; `libvpx-vp9` returns `yuva420p`. Hardware decoders drop
it too — `vp9_cuvid` was measured returning a solid opaque channel. So a file with
transparency is forced onto the CPU path even when the GPU is set as preferred.
Losing the alpha without a word is worse than decoding more slowly.

`decoder_test` checks the result rather than the intent: if the file declares
alpha, the delivered frames must contain more than one alpha value.

## Speed and hardware

### Asynchronous delivery, and who it actually helps

The plain way, Premiere asks for a frame and waits. Adobe provides a second,
asynchronous interface: the host orders frames ahead and collects them later,
while decoding happens as it gets on with its own work — compositing, effects,
display.

Not everything can be done ahead. The SDK allows scheduling exactly the work
that does not depend on the output size and format, and the host only names
those when it asks for the frame. Decoding can be scheduled; colour conversion
cannot.

Which produces a result that only measurement could have found. The fake host
was taught to keep the CPU busy between frames, the way a real one is, and the
two paths were compared on a 2560×1440 recording:

| host work between frames | CPU (`libdav1d`) | GPU (`av1_cuvid`) |
|---|---|---|
| 1 ms | −0.3% | **−7.8%** |
| 3 ms | −0.5% | **−12.2%** |
| 6 ms | −2.1% | **−9.5%** |
| 12 ms | −0.6% | **−5.4%** |

On the CPU, decoding costs half a millisecond and the dominant cost — colour
conversion — cannot be moved anywhere, so there is nothing to win. On the GPU,
decoding plus the copy out of video memory costs 3.5 ms, and all of that leaves
the host's thread.

Checked by reversing the order of the measurements: running the async path
first, so that warm-up works against it, leaves the win intact. That matters —
the first measurement in a series always flatters the last one.

By the rules of the SDK a separate importer **must not** hold a link to the
standard one: their lifetimes are decoupled and the standard one may close
first. So it has its own decoder and its own file handle. Its frame cache is
trimmed to a few frames ahead; the full budget would double the memory per clip
for no gain.

Turned off with `async = off` in the settings file, or `AETHER_ASYNC=0`.

What the tests found: `aiGetFrame` waited for the frame to appear among the
ready ones — and slept forever if the request was cancelled or flushed in the
meantime, because then nobody would ever produce it. That came out of a stress
scenario where four threads order, collect and cancel frames at once while a
close arrives in the middle of a running call. The SDK explicitly permits that,
and closing frees the importer's state — without a count of calls in flight it
would be a use-after-free inside Premiere.

### Hardware decoding on AMD does not go the way it looks like it should

AMD has a decoder of its own, `av1_amf`. It cannot be used: in this FFmpeg
build it **takes the process down**. Not "fails to start" — it initialises,
decodes frames correctly, and then dies with an access violation
(`0xC0000005`). Checked on 2026-08-22 on the integrated Radeon of a 9800X3D,
driver 32.0.21045.1000: five runs in a row, identical for `av1_amf` and
`vp9_amf`, on thirty frames and on one. Inside Premiere that would look like
"Premiere just closed itself", with nothing anywhere.

What does work is **D3D11VA**, the general Windows path for hardware decoding.
It is built differently: an ordinary FFmpeg decoder parses the stream and the
graphics card does the heavy lifting through the driver. Browsers and players
use the same mechanism, and it is one path for everybody — AMD, Intel,
NVIDIA — where vendor decoders need one each, every one with its own ailments.

Order of preference: vendor decoders first (`av1_cuvid`, `av1_qsv`), then
D3D11VA. Nothing changes on NVIDIA, and AMD and Intel get picked up by the
thing that ought to pick them up.

Speed remains an argument against the GPU rather than for it. 2560×1440, AV1,
per frame:

| path | fps | decode | transfer from GPU memory | colour |
|---|---:|---:|---:|---:|
| CPU, dav1d | **585** | 1.02 ms | — | 0.69 ms |
| NVIDIA, av1_cuvid | 125 | 0.20 ms | 3.58 ms | 4.22 ms |
| NVIDIA, D3D11VA | 106 | 0.18 ms | 5.00 ms | 4.26 ms |
| integrated AMD, D3D11VA | 73 | 0.21 ms | **9.12 ms** | 4.29 ms |

The decode itself is no slower on the integrated chip than on the discrete
NVIDIA card: 0.21 ms against 0.20. What eats everything is moving the frame
into system memory — nine milliseconds. The integrated GPU sits on the same
memory as the CPU and it helps not at all: the trip through D3D11 still costs
twice what it does on the discrete card.

So the point of the hardware path is not speed but leaving the CPU alone: when
it is busy with a render or an export, 73 frames a second at almost no CPU cost
can be worth more than 585 at full.

Any path can be tried without rebuilding anything:

```
set AETHER_DECODER=d3d11va         which way to decode
set AETHER_D3D11_ADAPTER=1         which graphics adapter to use
```

The second matters more than it looks: on a machine with two cards only one is
considered primary, and there is no other way to reach the other one. The
search does NOT continue afterwards — the chosen path either works or the file
does not open: falling back quietly would mean the test shows something other
than what was tested.

### The frame cache

Measurement, not intuition, decided this. Reading forward costs 4 ms per frame;
stepping *backwards* cost 71 ms. An inter-frame codec has to return to the nearest
keyframe and decode forward from there, and OBS writes a keyframe every second,
so every step back decoded half a second of video and threw away the result that
would be needed a moment later.

The RAM cache is therefore filled **only while moving backwards**. Reading forward
is already sequential, and caching there would waste memory for nothing. Frames
are moved out of GPU memory first. Its default process-wide ceiling is 512 MiB;
every insertion reserves bytes atomically, so one oversized frame or several
decoders cannot cross it. A zero limit disables this cache.

A separate persistent cache sits only in `GetFrameBGRA`: reduced BGRA8 requests
at Draft/Low quality, at most 2 MiB, and never sequential playback. Its key hashes
the source identity (path, size, mtime, file ID and first/last 64 KiB), timeline
frame, output size, decoder/backend, FFmpeg and Aether versions. The `.aepv`
record is tight top-down BGRA plus a strict header and CRC32. Writes go through a
bounded worker queue and atomic rename; any I/O or validation failure is a miss,
never an import failure. LRU cleanup returns the directory to 90% of its configured
limit.

## Loading and building

### Why the C++ runtime is linked statically

Premiere Pro CC 2019 keeps its own `msvcp140.dll` — version 14.00.24210, built with
Visual Studio 2015 — in its program folder. Windows resolves DLLs by name against
what is already loaded in the process, so a plug-in built with a newer Visual Studio
gets handed that old runtime and fails to initialise. `LoadLibrary` returns 1114,
and Premiere reacts exactly as if the plug-in were not installed: no message, no log
entry, and the same "unsupported compression type av01" you were trying to fix.

Linking the runtime statically (`/MT`) removes the dependency instead of fighting
over it. It is safe here because no runtime object ever crosses the plug-in
boundary: Premiere is spoken to through its own structures, FFmpeg through a C API,
and the FFmpeg DLLs carry their own runtime anyway.

`plugin_test` reads the import table out of the built `.prm` and fails if
`MSVCP*` or `VCRUNTIME*` reappear there — the machine that builds the plug-in is
the one machine where this bug cannot be observed.

### Why FFmpeg is delay-loaded

Windows looks for dependent DLLs next to the *executable*, meaning next to
Premiere, and never next to the DLL being loaded. With ordinary linking Premiere
would simply fail to load the plug-in, without a word. So `AV1Runtime.cpp` loads
FFmpeg from the plug-in's own folder at startup, and the libraries themselves are
linked with `/DELAYLOAD`, so by the time a real call happens they are already in
the process.

Two non-obvious build requirements follow:

- **whole-program optimisation must be off.** With it the linker silently ignores
  `/DELAYLOAD`;
- **the import libraries are regenerated** into `ffmpeg/lib-msvc`, because the ones
  shipped with FFmpeg are produced by a different toolchain and `/DELAYLOAD` does
  not work with them either (LNK4199):

```
lib /def:ffmpeg\lib\avcodec-62.def /name:avcodec-62.dll /out:ffmpeg\lib-msvc\avcodec.lib /machine:x64
```

## Around the plug-in

### The panel has to be signed

Premiere does not show unsigned extensions. Silently: no message, no menu
entry, nothing in a log. The only way around it is developer mode
(`PlayerDebugMode`), which an ordinary user does not have and should not need.

A self-signed certificate is accepted by Adobe — unlike Windows SmartScreen,
where self-signing is worth nothing — and it costs nothing:

```
installer\make-cert.ps1     certificate and signing tool
installer\make-panel.ps1    signing and verification
```

Timestamping is not optional. Without it the signature dies with the
certificate, and one day the panel would silently stop appearing for
**everybody** — no update, no reason, no message.

The developer's own machine lies in the comforting direction here: developer
mode is usually on, and the panel shows up even unsigned. That is why
`make-panel.ps1` runs `-verify` — the one check that does not depend on the
settings of the machine doing the build.

### The log

`%LOCALAPPDATA%\Aether\log.txt`, rewritten each time Premiere starts. It
records every selector Premiere sends, by name, **including the ones the plug-in
rejects**. The cause of failure hid among exactly those. Inside Premiere every
importer failure looks the same, so this log is the main debugging tool.

The file is held open and flushed after every line. It used to be opened and
closed again for each line — for the same crash-safety — and that turned out to
cost far more than expected:

| | per line |
|---|---|
| open, write, close | 117 µs |
| held open, flushed | 5 µs |
| held open, not flushed | 0.4 µs |

Lines are not rare: at least two for every frame delivered, against 650 µs of
actual decoding for a 1440p frame. The log was adding roughly a third to the
decoder's own work. Flushing stayed: it is what gives the crash-safety the
whole arrangement was for, and it costs twenty-four times less than reopening
the file.

Successful `imGetSourceVideo` / async frame calls are not logged one by one.
On `imShutdown` the importer writes one aggregated host-request profile instead:
reduced vs full size, pixel format, render quality, sequential vs jump, and the
most common asked sizes. That is the measurement surface for whether live
Premiere thumbnails actually match the disk-cache predicates.

## Testing without Premiere

`host_test` is the interesting one. It impersonates Premiere and drives the built
`.prm` through open, info, frames and audio, then does it again pretending to be
an older host: one that only knows version 7 of the frame-cache suite, one from the
Premiere 13.0 era, one that refuses the cache suite altogether. Each run must
deliver the same pixels.

That covers the failure this project actually had. A host does not hand out a suite
version it has never heard of, so asking for the newest one and nothing else leaves
the pointer null on every older Premiere — which looks exactly like the feature not
existing. Reverting the fix makes two of the five profiles fail, so the check is
not decorative.

What it cannot show: whether an older Premiere sends different selectors in a
different order, whether older SDK headers laid the structs out differently, or how
it reads the `IMPT` resource. Only a real installation answers those.

`decoder_test` ends with correctness checks and exits non-zero if any fails: a
cached frame against a freshly decoded one, buffer bounds when Premiere asks for a
reduced size, and whether the same audio range reads identically twice. The last
one found a real bug the first time it ran.

Two scripts turn that into something a build server can run:

```
tools\make-test-media.ps1     generates the media with FFmpeg - nothing is downloaded
tools\run-core-checks.ps1     runs the core against it and judges the result
```

The judging matters as much as the decoding. Five files must be accepted and pass
every check; two must be **refused** — an audio-only MP4 and an H.264 file, both of
which Premiere opens perfectly well on its own. A plug-in that grabs more than it
should is worse than one that grabs less.

A third tool feeds the same core **damaged** files:

```
tools\fuzz_test.cpp           truncations, noise, flipped bits, holes
```

One thing is checked: that the process reaches the end. Refusing to open such a
file is perfectly legitimate; taking the host down is not, and that is not
hypothetical — a frame with no dimensions once reached swscale, which answers
that not with an error but with `av_assert0` and the end of the process. Inside
Premiere it looked like "Premiere just closed itself", with nothing anywhere.

The damage follows fixed rules from a given seed, so "died on case 11"
reproduces word for word. The very first run did find a crash — in the tool
itself, which had sized a buffer for 8-bit and then asked for 16. That counts
as a result too: it proves the thing can catch what it is looking for.

That pair runs on every push, on GitHub's Windows runners. The plug-in itself
cannot be built there: it needs the Adobe SDK, which may not be redistributed, so
no build server can have a copy. What is checked is the layer deliberately kept
free of any Adobe dependency — most of the code, and nearly all of the risk.
