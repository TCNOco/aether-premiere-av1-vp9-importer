# Third-party notices / Сторонние компоненты

## FFmpeg

This product uses libraries from the FFmpeg project (`avcodec`, `avformat`,
`avutil`, `swresample`, `swscale`) under the **GNU Lesser General Public
License version 3** (LGPL v3). FFmpeg is not owned by the authors of this
plug-in, and no FFmpeg developer endorses it.

The binaries shipped with the installer are **unmodified** builds by BtbN,
variant `win64-lgpl-shared`, version n8.1:

- Builds: <https://github.com/BtbN/FFmpeg-Builds/releases>
- FFmpeg source: <https://github.com/FFmpeg/FFmpeg>
- Build scripts: <https://github.com/BtbN/FFmpeg-Builds>

The full text of the licence is shipped alongside the plug-in as
`LICENSE-ffmpeg.txt` and is also available at <https://www.gnu.org/licenses/lgpl-3.0.html>.

The plug-in links to these libraries **dynamically**. They are ordinary DLL
files in the plug-in folder, so you may replace them with your own build of the
same FFmpeg versions — that is the relinking freedom LGPL requires. The plug-in
loads them by file name from its own directory:

```
avutil-60.dll  swresample-6.dll  swscale-9.dll  avcodec-62.dll  avformat-62.dll
```

---

Плагин использует библиотеки проекта FFmpeg по лицензии **LGPL v3**. Двоичные
файлы в установщике — **неизменённая** сборка BtbN (`win64-lgpl-shared`, n8.1),
ссылки на исходники и скрипты сборки выше. Полный текст лицензии кладётся рядом
с плагином как `LICENSE-ffmpeg.txt`.

Связывание динамическое: библиотеки лежат обычными DLL в папке плагина, и вы
вправе заменить их собственной сборкой тех же версий FFmpeg — именно эту свободу
и требует LGPL.

## Adobe Premiere Pro C++ SDK

The plug-in is built against the Adobe Premiere Pro C++ SDK. The SDK itself is
**not** included in this repository and is not redistributed — obtain it from
<https://developer.adobe.com/premiere-pro/> under Adobe's own terms.

Плагин собирается с Adobe Premiere Pro C++ SDK. Сам SDK в репозиторий **не
входит** и не распространяется — его нужно скачать у Adobe отдельно.

Adobe, Premiere Pro and Media Encoder are trademarks of Adobe Inc. This project
is not affiliated with or endorsed by Adobe.
