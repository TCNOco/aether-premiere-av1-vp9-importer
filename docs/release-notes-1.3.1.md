# Aether 1.3.1

A hardening release. No new features â€” the plug-in is safer to load, stricter
about what it opens, and quieter on AAC audio inside Matroska.

## FFmpeg only from the plug-in folder

The libraries are loaded by full path next to `Aether.prm`. Searching by name
(PATH, the Premiere folder, the current directory) is gone: that search is how
a wrong DLL would execute inside Adobe. `build.bat` copies the five DLLs next
to the `.prm` so a local build matches what the installer ships.

A damaged install still disables the importer instead of taking Premiere down.

## Only our containers

The demuxer is chosen by content, not by the file name. A file ending in
`.mp4` can be an ffmpeg concat playlist that points at a neighbour. Opening
now uses `protocol_whitelist=file` and a short `format_whitelist` (mp4 / mov /
mkv / webm and the usual relatives). A disguised concat file is refused; the
core checks cover that case.

Frame edges above 8192 are refused before the decoder opens. A broken
container without EOF can no longer spin the host thread forever.

## AAC crackle in Matroska

Matroska stores audio timestamps on a 1 ms grid. An AAC frame is 1024 samples
(~21.33 ms). On every boundary the plug-in saw a false overlap or gap of a
handful of samples â€” Premiere heard that as crackle. WebM with Opus was fine:
its frame is exactly 20 ms.

Continuous reads now stitch those boundaries; after a seek the grid is not
rewritten, so scrubbing stays correct. Checked in Premiere Pro 26.

## Host and settings

- All five frame buffers are checked on allocation; a failed write disposes
  the host PPix instead of leaking it.
- Input frame-format structs from Premiere are no longer written into.
- Stride checks use 64-bit arithmetic.
- Files open with `FILE_SHARE_WRITE | FILE_SHARE_DELETE` so a recording that is
  still being written is not rejected as â€œunsupportedâ€.
- `settings.ini` is read once per process (environment variables still win on
  every call). The CEP panel writes it the same way C++ does: temporary file,
  then replace.
- The log is closed on `imShutdown`, not from `DllMain`.

## Checks

Core media set (including the concat disguise), damaged-file passes, the fake
host on the premiere-test clips, and a live check of the three Matroska AAC
files in Premiere Pro 26.0 on Windows 11.

## Install

Download `AetherSetup-1.3.1.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

There is also **`Aether-1.3.1-portable.zip`**: `Aether` goes into
`â€¦\Adobe\Common\Plug-ins\7.0\MediaCore\`, and `com.nehade.aether` into
`â€¦\Common Files\Adobe\CEP\extensions\` if you want the panel. `INSTALL.txt` in
the archive spells it out.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.3.1.exe        SHA256  2669bf62d8de06563f708796c88d76965b0b4ba7917d78670274f35e18d098b2
Aether-1.3.1-portable.zip    SHA256  70427cb079bf52d414a1af6ed2ded0b50328d7d80d3200475a7c19f295c126df
```

The README has a section on what else there is to go on besides these numbers.
