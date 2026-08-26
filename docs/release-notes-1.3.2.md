# Aether 1.3.2

Settings you can actually use, a kill switch that unloads FFmpeg, an honest RAM
limit, optional disk cache for reduced previews, and native 10-bit planes on
Premiere 26.

## HDR / 10-bit as P010

10-bit footage is offered as native P010 first. On Premiere 26 `CreatePPix`
returns a writable buffer, `GetPixels` works, and `GetRowBytes` can still
report zero — the stride is then `width * 2`, checked against `GetSize`.
`CreateCustomPPix` for the stock BIPLANAR format returns `InvalidParms` and is
not used.

If the host refuses a buffer, that request falls back to BGRA 16-bit and P010
is not offered again until Premiere restarts. Checked live on HDR footage:
the log shows `10u planes (P010)`.

Turn it off with `yuv10 = off` or `AETHER_YUV10=0`.

## Host hardening

Audio with more than 64 channels is refused. `swr_convert` already truncated
to 64, while `GetAudio` walked the full count and could read empty buffers.

An exception from `xImportEntry` or the async entry no longer terminates the
Adobe process.

The RAM cache no longer keeps a floor of four frames, which broke the global
cap on several 8K clips at once. Transfer `UNSPECIFIED` is no longer rewritten
as BT.709 — the host gets ITU code 2.

`release-check.bat` now hashes the shipped FFmpeg DLLs against
`tools/ffmpeg-dll.sha256`. Public CI still cannot run `plugin_test` /
`host_test`: the Adobe SDK may not be redistributed.

## Settings, kill switch, RAM ceiling

`Aether.exe` and the Premiere panel read and write one C++ settings model.
The panel no longer parses `settings.ini` itself.

- **Aether enabled.** After a restart, Adobe sees priority 0, every file is
  handed back, and FFmpeg is not loaded.
- **RAM frame cache.** 0 through 4 GiB, default 512 MiB. This caps decoded
  frames Aether keeps, not Premiere as a whole. Bytes are reserved with a
  process-wide CAS; a single frame larger than the budget is not inserted.
- **`cache_memory_mb=0` / `AETHER_CACHE_MB=0` now mean off.** Older builds
  treated zero as unlimited.

## Reduced preview cache (off by default)

Lossless BGRA8 Draft/Low frames up to 2 MiB can be stored under
`%LOCALAPPDATA%\Aether\preview-cache\v1`. Full-size playback, Good quality,
native YUV/P010, audio and export never use it. The window and the panel have
a Cache page: toggle, limit, usage, clear.

It stays **off** until a cold/warm project measurement shows a win without a
playback regression. A live Premiere 26 session with 1440p AV1 clips requested
full-size BGRA for bin posters, so the disk stayed empty — that is the filter
working, not a failed write.

## Checks

Core media set and damaged-file passes, settings and `.aepv` unit tests, the
disabled-importer `plugin_test`, representative `host_test`, and a live load
in Premiere Pro 26.0 on Windows 11.

## Install

Download `AetherSetup-1.3.2.exe`, close Premiere Pro, After Effects and Media
Encoder, run it. Administrator rights are required. Installs over any earlier
version; no need to uninstall first.

There is also **`Aether-1.3.2-portable.zip`**: `Aether` goes into
`...\Adobe\Common\Plug-ins\7.0\MediaCore\`, and `com.nehade.aether` into
`...\Common Files\Adobe\CEP\extensions\` if you want the panel. `INSTALL.txt` in
the archive spells it out.

The installer is not code-signed, so Windows will warn about an unknown
publisher. Verify the download instead:

```
AetherSetup-1.3.2.exe        SHA256  c45e4fb2fc6edd33737e6a90dab7e85789f2ac8c75da2053f6eecd3dd01ec7e8
Aether-1.3.2-portable.zip    SHA256  8d55d2e782130a1100206753b5b2839cbcbd75c84fcaf922a6baac6a578c6ef0
```

The README has a section on what else there is to go on besides these numbers.
