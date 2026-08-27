# Aether 1.3.3

A hardening release. Premiere can close a file while another thread is still
asking for a frame or for audio. That used to free the decoder under a live
call and take the host down. It no longer does.

## File lifetime

`imGetSourceVideo`, `imImportAudio7` and the other readers now pin the decoder
for the length of the call. `imCloseFile` marks the file closing, waits until
those pins drop, then deletes. `imQuietFile` waits the same way but keeps the
object, so the file can be opened again.

The async importer has the same shape. Host calls are checked against a table
of live objects by pointer identity, without dereferencing first. `aiClose`
removes the object from that table, waits for in-flight calls, then frees it.
The fake host covers close-during-GetFrame.

The enabled importer now asks Adobe to keep the plug-in loaded. Unloading the
`.prm` while an async worker is still running is how a closed Premiere looks
when nothing is wrong with the file.

## Kill switch and a damaged install

A disabled importer and an incomplete FFmpeg install now answer the same way:
`imInit` is cacheable at priority 0, files come back as `imBadFile`, and
`imQuietFile` / `imCloseFile` still run if private data exists. A missing DLL
must look like a plug-in that handed the file to the next importer, not like
Premiere closing itself.

## Decoder and cache

`Info()`, `IsOpen()` and decoder stats are copied under the decoder lock.
Callers that need audio layout ask again after `OpenAudio` — a snapshot taken
before that would report zero channels. Audio-only streams no longer count as a
video decoder in the thread and RAM budget. Frame duration in Premiere ticks saturates instead of overflowing a
signed multiply. Host frame sizes outside 1…8192 are refused before a buffer
is written.

The preview-cache fingerprint now hashes 256 KiB from the head, the middle and
the tail of the file. Leftover `protocol_whitelist` / `format_whitelist`
options after `avformat_open_input` fail closed: if this FFmpeg build did not
consume them, the whitelist is not in force. `LoadLibraryExW` checks the path
of the module it actually got — another MediaCore plug-in that already loaded
`avcodec-62.dll` from elsewhere is refused, not mixed.

The Premiere panel kills a hung diagnose process after 60 seconds.

## Checks

`release-check.bat` on this tree: core media set, damaged-file passes,
`plugin_test` (normal, disabled, incomplete runtime), and representative
`host_test` including concurrent async close.

## Install

Download `AetherSetup-1.3.3.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

There is also **`Aether-1.3.3-portable.zip`**: `Aether` goes into
`...\Adobe\Common\Plug-ins\7.0\MediaCore\`, and `com.nehade.aether` into
`...\Common Files\Adobe\CEP\extensions\` if you want the panel. `INSTALL.txt` in
the archive spells it out.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.3.3.exe        SHA256  edf187332759fff073f5bd7bc4da2bbceb7b771a0740bcc61010f286e3aa5220
Aether-1.3.3-portable.zip    SHA256  96632eab326315428f5955b34d73f3cd97de80c9a7fc22c5610424436d4b3e47
```

The README has a section on what else there is to go on besides these numbers.
