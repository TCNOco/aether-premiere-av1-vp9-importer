# Third-party notices / Сторонние компоненты

The MIT licence in [LICENSE](LICENSE) covers **only the source code in this
repository**. It does not, and cannot, cover the third-party components below.

Лицензия MIT в [LICENSE](LICENSE) распространяется **только на исходный код этого
репозитория** и не покрывает перечисленные ниже сторонние компоненты.

---

## Adobe Premiere Pro C++ SDK

The plug-in is built against the Adobe Premiere Pro C++ SDK and uses only its
public, documented API. It does not modify Adobe software.

**No part of the SDK is included in this repository or in any release.** Headers,
sample code, utilities and every other SDK component are absent by design — see
`.gitignore`. To build from source you must obtain the SDK yourself from
<https://developer.adobe.com/premiere-pro/> and accept Adobe's own Developer
Terms; nothing in this project grants you any right to it.

What the release distributes is the compiled plug-in — object code that is our own
work — which is what Adobe's developer terms contemplate for plug-ins.

Adobe, Premiere Pro and Media Encoder are trademarks of Adobe Inc. This project is
independent: not affiliated with, sponsored by, or endorsed by Adobe.

Плагин собирается с Adobe Premiere Pro C++ SDK, использует только его публичный
API и не изменяет программы Adobe. **Никакая часть SDK не входит ни в репозиторий,
ни в релизы** — ни заголовки, ни примеры кода, ни утилиты. Для сборки SDK нужно
получить у Adobe самостоятельно и принять её условия; этот проект никаких прав на
SDK не даёт. В релизе распространяется скомпилированный плагин — наш собственный
код в машинном виде.

## FFmpeg

The plug-in uses libraries from the FFmpeg project (`avcodec`, `avformat`,
`avutil`, `swresample`, `swscale`) under the **GNU Lesser General Public License
version 3** (LGPL v3). FFmpeg is not owned by the authors of this plug-in, and no
FFmpeg developer endorses it.

The binaries shipped with the installer are **unmodified** builds by BtbN, variant
`win64-lgpl-shared`, version n8.1:

- Builds: <https://github.com/BtbN/FFmpeg-Builds/releases>
- FFmpeg source: <https://github.com/FFmpeg/FFmpeg>
- Build scripts: <https://github.com/BtbN/FFmpeg-Builds>

The full licence text ships alongside the plug-in as `LICENSE-ffmpeg.txt` and is
also at <https://www.gnu.org/licenses/lgpl-3.0.html>.

Linking is **dynamic**. The libraries are ordinary DLL files in the plug-in folder,
so you may replace them with your own build of the same FFmpeg versions — that is
the relinking freedom LGPL requires. The plug-in loads them by name from its own
directory:

```
avutil-60.dll  swresample-6.dll  swscale-9.dll  avcodec-62.dll  avformat-62.dll
```

Плагин использует библиотеки FFmpeg по лицензии **LGPL v3**. Двоичные файлы —
**неизменённая** сборка BtbN (`win64-lgpl-shared`, n8.1); ссылки на исходники и
скрипты сборки выше. Текст лицензии кладётся рядом с плагином как
`LICENSE-ffmpeg.txt`. Связывание динамическое: библиотеки лежат обычными DLL в
папке плагина, и вы вправе заменить их собственной сборкой тех же версий FFmpeg —
именно эту свободу и требует LGPL.

## AV1 decoders

Decoding is done by whichever of these is available, in this order:

| Decoder | Provided by | Licence |
|---|---|---|
| `av1_cuvid`, `av1_qsv`, `av1_amf` | the GPU vendor's driver (NVIDIA / Intel / AMD) | vendor's own terms |
| `libdav1d` | [dav1d](https://code.videolan.org/videolan/dav1d) by VideoLAN, inside the FFmpeg DLLs | BSD-2-Clause |
| `av1` | FFmpeg's own decoder | LGPL v3, as above |

The FFmpeg DLLs also contain the `libaom` and `SVT-AV1` **encoders** (BSD, with the
AOMedia Patent License 1.0). The plug-in never calls them: it only decodes and has
no encoding path at all.

## AV1 and patents

AV1 is published by the Alliance for Open Media as a royalty-free format under the
[AOMedia Patent License 1.0](https://aomedia.org/license/patent-license/).

Be aware of the limits of that, because "royalty-free" is not an absolute
guarantee:

- The licence covers only the Necessary Claims of AOMedia members. Patents held by
  non-members are outside it.
- Third-party claims do happen. In 2026 Dolby sued Snap over AV1, arguing that its
  patents are used and are not covered by the royalty-free model.

For an open-source plug-in the practical exposure is far lower than for a large
commercial product, but it is not zero — particularly for heavy commercial use.
**None of this is legal advice.** If you ship this in a commercial product, get
your own.

AV1 объявлен Alliance for Open Media бесплатным от отчислений по их Patent License
1.0, но это покрывает только патенты участников AOMedia — претензии третьих сторон
возможны. В 2026 году Dolby подала иск против Snap именно по AV1, что показывает:
«royalty-free» не абсолютная гарантия. Для открытого плагина риск заметно ниже, чем
для крупного коммерческого продукта, но не нулевой. Это не юридическая
консультация.
