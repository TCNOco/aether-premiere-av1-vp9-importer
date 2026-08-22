; Установщик Aether — импортёра AV1 и VP9 для приложений Adobe.
;
; Собирается компилятором Inno Setup:
;   ISCC.exe installer\Aether.iss
; (или через installer\build-installer.bat, который сам найдёт компилятор)
;
; Путь установки не даётся на выбор намеренно: приложения Adobe ищут плагины
; только в своей общей папке, а рядом с .prm обязаны лежать библиотеки ffmpeg —
; плагин ищет их именно там. Свободный выбор папки означал бы неработающую
; установку у половины пользователей.
;
; Зато сама папка не прибита гвоздями: её адрес Adobe пишет в реестр, и мы его
; оттуда читаем. Жёсткий путь остаётся запасным вариантом.
;
; Картинки и значок рисуются кодом — installer\make-art.py, — чтобы их можно
; было пересобрать, а не хранить как двоичные файлы неизвестного происхождения.

#define AppName        "Aether - AV1 / VP9 Importer for Adobe"

; Версия НЕ пишется здесь, а читается из собранного плагина.
;
; Своя копия номера рано или поздно разошлась бы с настоящей, и разошлась бы
; молча: установщик уверял бы одно, свойства файла показывали другое. Плагин
; несёт свою версию в ресурсе (см. src\AV1Version.h) — оттуда и берём, так
; расходиться нечему. Нет собранного плагина — нет и установщика, и это
; правильно: собирать установщик вокруг пустого места незачем.
#define SourcePlugin   "..\build\Release\Aether.prm"
#define AppVersion     GetStringFileInfo(SourcePlugin, PRODUCT_VERSION)
#define AppPublisher   "neoHaDe"
#define AppURL         "https://github.com/neoHaDe/aether-premiere-av1-vp9-importer"
#define PluginDir      "Aether"

; Как папка называлась раньше. Нужна не для истории: установка под прежним
; именем осталась бы лежать рядом, и два импортёра спорили бы за один и тот же
; av01 — а кто победит, зависит от порядка обхода папки.
#define OldPluginDir   "AV1 Importer"

[Setup]
AppId={{7C4A1E92-AV01-4C31-9E7B-1D2F3A4B5C60}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Aether - AV1 and VP9 decoder plug-in for Adobe

; Плагины Adobe живут в общей папке, туда нужны права администратора
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Папку берём из реестра Adobe, см. GetMediaCore ниже
DefaultDirName={code:GetMediaCore}\{#PluginDir}
DisableDirPage=yes

; Папка сменила имя вместе с плагином, поэтому запомненная от прошлой
; установки здесь только помешала бы
UsePreviousAppDir=no
DefaultGroupName={#AppName}
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\aether.ico

; Выбор языка первым окном, всегда — а не только когда система не совпала
; ни с одним из них. Русский и английский тут равноправны, и угадывать за
; человека, на каком ему читать, не надо
ShowLanguageDialog=yes

; Экран приветствия включён намеренно: на нём видно, что это за плагин и для
; чего он, а без него установщик начинался сразу с текста лицензии
DisableWelcomePage=no

; Restart Manager сам обнаружит, что приложение Adobe держит файлы плагина,
; и предложит закрыть его — вместо невнятной ошибки «файл занят»
CloseApplications=yes
RestartApplications=no

OutputDir=..\dist
OutputBaseFilename=AetherSetup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=aether.ico
WizardImageFile=wizard-large.bmp
WizardSmallImageFile=wizard-small.bmp
LicenseFile=..\LICENSE

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
ru.SettingsShortcut=Aether
en.SettingsShortcut=Aether
ru.SettingsComment=Настройки плагина и диагностика
en.SettingsComment=Plug-in settings and diagnostics
ru.OpenSettings=Проверить установку
en.OpenSettings=Check the installation

ru.FoundCaption=Приложения Adobe
ru.FoundDesc=Куда попадёт плагин и кто его увидит
ru.FoundIntro=Плагин ставится в общую папку Adobe, поэтому его подхватывают все приложения сразу — отдельная установка в каждое не нужна.
ru.FoundList=На этом компьютере найдены:
ru.FoundNone=Приложений Adobe на этом компьютере не найдено.%n%nПлагин всё равно можно установить: он подхватится сам, когда вы поставите Premiere Pro, After Effects или Media Encoder.
ru.FoundClose=Эти приложения нужно закрыть. Если они открыты, установщик предложит закрыть их сам.
ru.FoundWhere=Папка установки:

en.FoundCaption=Adobe applications
en.FoundDesc=Where the plug-in goes and which applications will see it
en.FoundIntro=The plug-in is installed into the folder Adobe applications share, so all of them pick it up at once — there is nothing to install per application.
en.FoundList=Found on this computer:
en.FoundNone=No Adobe applications were found on this computer.%n%nYou can still install the plug-in: it will be picked up as soon as you install Premiere Pro, After Effects or Media Encoder.
en.FoundClose=These applications have to be closed. If any of them is open, the installer will offer to close it.
en.FoundWhere=Installing into:

[InstallDelete]
; Прежняя установка целиком. Файлы там останутся собственностью старой записи
; в «Программах и компонентах», но запись мы перезаписываем своей (AppId тот
; же), так что удалить их больше некому.
Type: filesandordirs; Name: "{code:GetMediaCore}\{#OldPluginDir}"

; Панель: при обновлении подпись меняется, и остатки прежней версии могут
; поспорить с новой за один и тот же идентификатор.
Type: filesandordirs; Name: "{code:GetCepExtensions}\com.nehade.aether"

; Прежнее имя программы: до 1.2.3 окно называлось AetherSettings.exe.
; Оставить его — значит оставить в папке файл, который никто не обновляет
; и который однажды покажет настройки версии двухлетней давности.
Type: files; Name: "{app}\AetherSettings.exe"

[Icons]
Name: "{autoprograms}\{cm:SettingsShortcut}"; Filename: "{app}\Aether.exe"; Comment: "{cm:SettingsComment}"

[Files]
Source: "{#SourcePlugin}"; DestDir: "{app}"; Flags: ignoreversion

; Окно плагина: настройки и диагностика. Отдельная программа, а не панель
; внутри Premiere — она нужна и тогда, когда Premiere из-за драйвера не
; запускается, и через полгода, когда установщика давно нет.
Source: "..\build\Release\Aether.exe"; DestDir: "{app}"; Flags: ignoreversion

; Пробные клипы для диагностики. Кладутся рядом, а не распаковываются во
; временную папку: нечего чистить и не на что ругаться антивирусу.
; Каждый по паре секунд — проверяется путь распаковки, а не разрешение.
Source: "..\build\media\samples\*"; DestDir: "{app}\samples"; Flags: ignoreversion

; Движок диагностики без окна. Его зовёт панель внутри Premiere: она на HTML
; и распаковывать видео не умеет никак, поэтому проверку делает та же
; программа, что и окно, — расходиться им негде.
Source: "..\build\Release\AetherDiagnose.exe"; DestDir: "{app}"; Flags: ignoreversion

; Панель для меню «Расширения». Ставится в ОБЩУЮ папку расширений Adobe,
; а не в профиль пользователя, и это осознанно: установщик работает от
; администратора, и «папка текущего пользователя» под повышением прав
; означает не то, что кажется. Общая папка одна на всех и вопросов не
; вызывает.
;
; Панель ПОДПИСАНА (installer\make-panel.ps1). Без подписи Premiere её не
; покажет — молча, без единого сообщения; обойти это можно только режимом
; разработчика, которого у обычного человека нет.
Source: "..\build\panel\com.nehade.aether\*"; DestDir: "{code:GetCepExtensions}\com.nehade.aether"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

; Значок нужен и после установки: на него смотрит «Установка и удаление программ»
Source: "aether.ico"; DestDir: "{app}"; Flags: ignoreversion

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
Source: "..\NOTICE";                  DestDir: "{app}"; DestName: "NOTICE.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\ffmpeg\LICENSE.txt";      DestDir: "{app}"; DestName: "LICENSE-ffmpeg.txt"; Flags: ignoreversion

[Run]
; Диагностика запускается ПОСЛЕ установки и от обычного пользователя.
; Проверять до установки нечего: главные вопросы — подхватил ли Premiere
; плагин и распаковывается ли файл — все про уже установленное. К тому же
; установщик работает от администратора, а Premiere никогда, и проверка
; под повышенными правами прошла бы там, где настоящая работа не проходит.
Filename: "{app}\Aether.exe"; Parameters: "--diagnose"; Description: "{cm:OpenSettings}"; Flags: postinstall nowait skipifsilent

[Code]

const
  FallbackMediaCore = '\Adobe\Common\Plug-ins\7.0\MediaCore';

var
  CachedMediaCore: String;
  FoundPage: TWizardPage;
  FoundMemo: TNewMemo;

// Убрать завершающую наклонную черту: After Effects пишет путь с ней,
// Premiere без неё, а склеивать потом одинаково
function TrimSlash(const S: String): String;
begin
  Result := S;
  while (Length(Result) > 3) and (Result[Length(Result)] = '\') do
    Result := Copy(Result, 1, Length(Result) - 1);
end;

// Спросить у Adobe, где лежат общие плагины, вместо того чтобы угадывать.
// Все приложения пишут этот адрес себе в реестр при установке, и он один
// и тот же для всех — потому плагин и подхватывается всеми сразу.
function DetectMediaCore(): String;
var
  Versions: TArrayOfString;
  Apps: TArrayOfString;
  Path: String;
  i, a: Integer;
begin
  Result := '';

  if RegQueryStringValue(HKLM64, 'SOFTWARE\Adobe\Premiere Pro\CurrentVersion',
                         'Plug-InsDir', Path) then
  begin
    Path := TrimSlash(Path);
    if DirExists(Path) then
    begin
      Result := Path;
      Exit;
    end;
  end;

  // Иначе перебираем версии всех приложений: любая из них знает нужный адрес
  SetArrayLength(Apps, 2);
  Apps[0] := 'Premiere Pro';
  Apps[1] := 'After Effects';
  for a := 0 to GetArrayLength(Apps) - 1 do
  begin
    if not RegGetSubkeyNames(HKLM64, 'SOFTWARE\Adobe\' + Apps[a], Versions) then
      Continue;
    for i := 0 to GetArrayLength(Versions) - 1 do
    begin
      if RegQueryStringValue(HKLM64, 'SOFTWARE\Adobe\' + Apps[a] + '\' + Versions[i],
                             'CommonPluginInstallPath', Path) then
      begin
        Path := TrimSlash(Path);
        if DirExists(Path) then
        begin
          Result := Path;
          Exit;
        end;
      end;
    end;
  end;
end;

function GetMediaCore(Param: String): String;
begin
  if CachedMediaCore = '' then
  begin
    CachedMediaCore := DetectMediaCore();
    if CachedMediaCore = '' then
      CachedMediaCore := ExpandConstant('{commonpf}') + FallbackMediaCore;
  end;
  Result := CachedMediaCore;
end;

// Общая папка расширений CEP — та, из которой Premiere берёт панели.
//
// Берём общую, а не пользовательскую, и это не мелочь. Установщик работает
// от администратора, а «папка текущего пользователя» под повышением прав
// означает не то, что кажется: панель легко уедет в профиль администратора,
// и человек её не увидит. Общая папка одна на всех и таких вопросов не
// задаёт.
//
// Путь у неё в Program Files (x86) даже на 64-битной системе — так его
// заложила Adobe, и от разрядности он не зависит.
function GetCepExtensions(Param: String): String;
begin
  Result := ExpandConstant('{commoncf32}') + '\Adobe\CEP\extensions';
end;

// Корень Adobe — четыре уровня вверх от MediaCore. Считаем от найденной папки,
// а не от {commonpf}: если Adobe стоит не на системном диске, жёсткий путь
// смотрел бы не туда, а этот — туда же, куда и сам плагин.
function AdobeRoot(): String;
var
  P: String;
  i: Integer;
begin
  P := GetMediaCore('');
  for i := 1 to 4 do
    P := ExtractFileDir(P);
  Result := P;
end;

// Список приложений Adobe по папкам, а не по реестру. Реестр знает только про
// Premiere Pro и After Effects — Media Encoder не заводит там ключа вовсе,
// а по папкам видно всё, включая версии, которых ещё не существует.
procedure CollectApps(var Names: TArrayOfString);
var
  Root: String;
  Rec: TFindRec;
  Masks: TArrayOfString;
  m, n: Integer;
begin
  SetArrayLength(Names, 0);
  Root := AdobeRoot();
  if not DirExists(Root) then
    Exit;

  SetArrayLength(Masks, 3);
  Masks[0] := 'Adobe Premiere Pro*';
  Masks[1] := 'Adobe After Effects*';
  Masks[2] := 'Adobe Media Encoder*';
  for m := 0 to GetArrayLength(Masks) - 1 do
  begin
    if FindFirst(Root + '\' + Masks[m], Rec) then
    begin
      try
        repeat
          if (Rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          begin
            n := GetArrayLength(Names);
            SetArrayLength(Names, n + 1);
            Names[n] := Rec.Name;
          end;
        until not FindNext(Rec);
      finally
        FindClose(Rec);
      end;
    end;
  end;
end;

procedure InitializeWizard();
var
  Names: TArrayOfString;
  Text: String;
  i: Integer;
begin
  CollectApps(Names);

  Text := ExpandConstant('{cm:FoundIntro}') + #13#10#13#10;

  if GetArrayLength(Names) > 0 then
  begin
    Text := Text + ExpandConstant('{cm:FoundList}') + #13#10;
    for i := 0 to GetArrayLength(Names) - 1 do
      Text := Text + '    ' + Names[i] + #13#10;
    Text := Text + #13#10 + ExpandConstant('{cm:FoundClose}') + #13#10;
  end
  else
    Text := Text + ExpandConstant('{cm:FoundNone}') + #13#10;

  // Путь собираем сами, а не через {app}: на этом этапе мастер его ещё
  // не обязан знать, а показать его надо именно здесь
  Text := Text + #13#10 + ExpandConstant('{cm:FoundWhere}') + #13#10
               + '    ' + GetMediaCore('') + '\{#PluginDir}';

  // В журнал установки: по нему видно, что именно нашлось на чужой машине,
  // а это первый вопрос в любом отчёте «плагин не появился»
  Log('MediaCore: ' + GetMediaCore(''));
  if GetArrayLength(Names) = 0 then
    Log('found: nothing')
  else
    for i := 0 to GetArrayLength(Names) - 1 do
      Log('found: ' + Names[i]);

  // Своя страница с обычным полем, а не CreateOutputMsgMemoPage: тот кладёт
  // текст в RichEdit через разбор RTF, и кириллица приезжает вопросительными
  // знаками, а случайные символы разбираются как команды разметки и красят
  // строки в разные цвета. Проверено на русской версии этой самой страницы.
  FoundPage := CreateCustomPage(wpLicense,
    ExpandConstant('{cm:FoundCaption}'),
    ExpandConstant('{cm:FoundDesc}'));

  FoundMemo := TNewMemo.Create(FoundPage);
  FoundMemo.Parent     := FoundPage.Surface;
  FoundMemo.Left       := 0;
  FoundMemo.Top        := 0;
  FoundMemo.Width      := FoundPage.SurfaceWidth;
  FoundMemo.Height     := FoundPage.SurfaceHeight;
  FoundMemo.ScrollBars := ssVertical;
  FoundMemo.ReadOnly   := True;
  FoundMemo.TabStop    := False;
  FoundMemo.Text       := Text;
end;
