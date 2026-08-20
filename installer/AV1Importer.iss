; Установщик плагина AV1 Importer для Adobe Premiere Pro.
;
; Собирается компилятором Inno Setup:
;   ISCC.exe installer\AV1Importer.iss
; (или через installer\build-installer.bat, который сам найдёт компилятор)
;
; Путь установки не даётся на выбор намеренно: Premiere ищет плагины только
; в MediaCore, и папка рядом с .prm обязана содержать библиотеки ffmpeg —
; плагин ищет их именно там. Свободный выбор папки означал бы неработающую
; установку у половины пользователей.

#define AppName        "AV1 Importer for Premiere Pro"
#define AppVersion     "1.0.1"
#define AppPublisher   "neoHaDe"
#define AppURL         "https://github.com/neoHaDe/premiere-av1-importer"
#define PluginDir      "AV1 Importer"

[Setup]
AppId={{7C4A1E92-AV01-4C31-9E7B-1D2F3A4B5C60}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
VersionInfoVersion={#AppVersion}
VersionInfoDescription=AV1 decoder plug-in for Adobe Premiere Pro

; Плагины Adobe живут в общей папке, туда нужны права администратора
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

DefaultDirName={commonpf}\Adobe\Common\Plug-ins\7.0\MediaCore\{#PluginDir}
DisableDirPage=yes
DisableProgramGroupPage=yes
DefaultGroupName={#AppName}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\AV1Importer.prm

; Restart Manager сам обнаружит, что Premiere держит файлы плагина,
; и предложит закрыть его — вместо невнятной ошибки «файл занят»
CloseApplications=yes
RestartApplications=no

OutputDir=..\dist
OutputBaseFilename=AV1Importer-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
ru.NoPremiere=Не найдена папка плагинов Adobe:%n%n%1%n%nПохоже, Adobe Premiere Pro не установлен. Продолжить всё равно?
en.NoPremiere=Adobe plug-in folder not found:%n%n%1%n%nAdobe Premiere Pro does not appear to be installed. Continue anyway?
ru.CloseFirst=Перед установкой закройте Adobe Premiere Pro и Media Encoder.
en.CloseFirst=Please close Adobe Premiere Pro and Media Encoder before installing.

[Files]
Source: "..\build\Release\AV1Importer.prm"; DestDir: "{app}"; Flags: ignoreversion

; Библиотеки ffmpeg обязаны лежать рядом с плагином: Windows ищет их возле
; исполняемого файла (то есть возле Premiere), поэтому плагин загружает их
; сам по полному пути из своей папки
Source: "..\ffmpeg\bin\avcodec-62.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\ffmpeg\bin\avformat-62.dll";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\ffmpeg\bin\avutil-60.dll";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\ffmpeg\bin\swresample-6.dll";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\ffmpeg\bin\swscale-9.dll";     DestDir: "{app}"; Flags: ignoreversion

; Лицензии кладём рядом с плагином: раздача библиотек ffmpeg под LGPL
; обязывает приложить текст лицензии к самим файлам, а не только к репозиторию
Source: "..\LICENSE";                 DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\ffmpeg\LICENSE.txt";      DestDir: "{app}"; DestName: "LICENSE-ffmpeg.txt"; Flags: ignoreversion

[Code]

function InitializeSetup(): Boolean;
var
  MediaCore: String;
begin
  Result := True;

  // В тихом режиме окна не показываем: установщик должен отработать без
  // единого клика, иначе автоматическая установка встанет насмерть
  if not WizardSilent() then
    MsgBox(ExpandConstant('{cm:CloseFirst}'), mbInformation, MB_OK);

  MediaCore := ExpandConstant('{commonpf}\Adobe\Common\Plug-ins\7.0\MediaCore');
  if not DirExists(MediaCore) and not WizardSilent() then
  begin
    Result := MsgBox(FmtMessage(ExpandConstant('{cm:NoPremiere}'), [MediaCore]),
                     mbConfirmation, MB_YESNO) = IDYES;
  end;
end;
