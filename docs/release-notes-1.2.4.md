# Aether 1.2.4

Hardware decoding now works on AMD. Getting there meant removing the decoder
AMD ships for the purpose, because it takes the process down.

## AMD

There is a vendor decoder, `av1_amf`. It cannot be used: in this FFmpeg build
it **kills the process**. Not "fails to start" — it initialises, decodes frames
correctly, and then dies with an access violation (`0xC0000005`). Five runs in
a row, identical for `av1_amf` and `vp9_amf`, on thirty frames and on one.
Tested on the integrated Radeon of a Ryzen 9800X3D, driver 32.0.21045.1000.

That mattered more than it sounds. `av1_amf` was **third in our preference
list**. On a machine with an NVIDIA card it is unreachable — the D3D11 device
goes to NVIDIA and AMF will not initialise on somebody else's device — but on
an AMD-only machine it would have been reached, and the crash would have
happened inside Premiere. That looks like "Premiere just closed itself", with
nothing anywhere.

What does work is **D3D11VA**, the general Windows path for hardware decoding:
an ordinary FFmpeg decoder parses the stream and the graphics card does the
heavy lifting through the driver. Browsers and players use the same mechanism,
and it is one path for AMD, Intel and NVIDIA alike, where vendor decoders need
one each and every one has its own ailments.

It sits **after** the vendor decoders in the preference list, so nothing
changes on NVIDIA: that path is still `av1_cuvid`, exactly as measured before.

The whole check suite passes on the integrated Radeon — AV1 8-bit and 10-bit,
VP9, variable frame rate, audio sync, colour, HDR. The one file that does not
is the one with alpha, and that is our own rule rather than an AMD problem:
hardware decoders lose the alpha channel, so those files deliberately go to the
CPU. Force hardware on that file and it fails on NVIDIA in exactly the same way.

## Speed is still an argument against the GPU

2560×1440, AV1, per frame:

| path | fps | decode | transfer from GPU memory | colour |
|---|---:|---:|---:|---:|
| CPU, dav1d | **585** | 1.02 ms | — | 0.69 ms |
| NVIDIA, av1_cuvid | 125 | 0.20 ms | 3.58 ms | 4.22 ms |
| NVIDIA, D3D11VA | 106 | 0.18 ms | 5.00 ms | 4.26 ms |
| integrated AMD, D3D11VA | 73 | 0.21 ms | **9.12 ms** | 4.29 ms |

The decode itself is no slower on the integrated chip than on the discrete
card: 0.21 ms against 0.20. What eats everything is moving the frame into
system memory — nine milliseconds. The integrated GPU shares memory with the
CPU and it helps not at all.

So the point of the hardware path is not speed but leaving the CPU alone. When
it is busy with a render or an export, 73 frames a second at almost no CPU cost
can be worth more than 585 at full. The default is unchanged: automatic
selection still prefers the CPU on machines with eight threads or more.

## Two switches for support

```
AETHER_DECODER=d3d11va         which way to decode, or a decoder by name
AETHER_D3D11_ADAPTER=1         which graphics adapter to use
```

The second matters more than it looks: on a machine with two cards only one
counts as primary, and there is no other way to reach the other one. Without it
the integrated Radeon could not have been tested at all.

Neither falls back quietly. The chosen path either works or the file does not
open — a silent fallback would mean the test shows something other than what
was tested.

## Known limitations

- Intel (`av1_qsv`) is still untested — no hardware here. It is now more likely
  to work, since Intel would be picked up by the same D3D11VA path as AMD, but
  likely is not tested.
- Discrete AMD cards are untested; the path is the same as the integrated one.
- No alpha channel for AV1 — it is stored differently from VP9's.
- No timecode from the file, no deinterlacing.
- The panel uses CEP, which Adobe has deprecated in favour of UXP.
- Windows only.

## Install

Download `AetherSetup-1.2.4.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

There is also **`Aether-1.2.4-portable.zip`**: `Aether` goes into
`…\Adobe\Common\Plug-ins\7.0\MediaCore\`, and `com.nehade.aether` into
`…\Common Files\Adobe\CEP\extensions\` if you want the panel. `INSTALL.txt` in
the archive spells it out.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.2.4.exe        SHA256  faa0769518d26169bb670f04429e11ea850d4350ea99f03d1b65ec0f743af6dc
Aether-1.2.4-portable.zip    SHA256  6090b6552caf2fadd92871d70c787789f7a8b9cccbaab41c7367498da08d7ced
```

The README has a section on what else there is to go on besides these numbers.
