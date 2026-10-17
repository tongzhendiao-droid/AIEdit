; AIEdit's installer, made with Inno Setup 6 from the folder `cmake --install` fills:
;
;   cmake --install build --prefix dist\AIEdit
;   iscc /DAppVersion=0.1.0 /DSourceDir=dist\AIEdit /DOutputDir=dist packaging\windows\aiedit.iss
;
; It installs for the current user without administrator rights, or for all users if chosen, and
; can add "Open with AIEdit" to the right-click menu of files and folders and offer AIEdit in
; "Open with" and in Settings > Default apps. Chinese is offered when /DChineseIsl names Inno
; Setup's unofficial ChineseSimplified.isl, or Inno Setup has it among its languages.

#ifndef AppVersion
  #error Pass /DAppVersion=<version>
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<the folder cmake --install filled>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef ChineseIsl
  #if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
    #define ChineseIsl "compiler:Languages\ChineseSimplified.isl"
  #endif
#endif

[Setup]
AppId={{0ED43B20-BD2D-45CC-9325-46906448B837}
AppName=AIEdit
AppVersion={#AppVersion}
AppPublisher=AIEdit
AppPublisherURL=https://github.com/tongzhendiao-droid/AIEdit
AppSupportURL=https://github.com/tongzhendiao-droid/AIEdit/issues
DefaultDirName={autopf}\AIEdit
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=aiedit.ico
UninstallDisplayIcon={app}\bin\aiedit.exe
OutputDir={#OutputDir}
OutputBaseFilename=AIEdit-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
#ifdef ChineseIsl
Name: "chinesesimplified"; MessagesFile: "{#ChineseIsl}"
#endif

[CustomMessages]
OpenWith=Open with AIEdit
Integration=Windows integration:
ContextMenuTask=Add "Open with AIEdit" to the right-click menu of files and folders
OpenWithTask=Offer AIEdit in "Open with" for text, log, data and binary files
#ifdef ChineseIsl
chinesesimplified.OpenWith=用 AIEdit 打开
chinesesimplified.Integration=系统集成：
chinesesimplified.ContextMenuTask=在文件和文件夹的右键菜单中加入“用 AIEdit 打开”
chinesesimplified.OpenWithTask=在“打开方式”中列出 AIEdit（文本、日志、数据和二进制文件）
#endif

[Tasks]
Name: "contextmenu"; Description: "{cm:ContextMenuTask}"; GroupDescription: "{cm:Integration}"
Name: "openwith"; Description: "{cm:OpenWithTask}"; GroupDescription: "{cm:Integration}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\AIEdit"; Filename: "{app}\bin\aiedit.exe"
Name: "{autodesktop}\AIEdit"; Filename: "{app}\bin\aiedit.exe"; Tasks: desktopicon

[Registry]
; The right-click menu of any file, of a folder (which opens in the folder panel), and of the
; background of a folder window. On Windows 11 it is under "Show more options".
Root: HKA; Subkey: "Software\Classes\*\shell\AIEdit"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenWith}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\*\shell\AIEdit"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\bin\aiedit.exe,0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\*\shell\AIEdit\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\aiedit.exe"" ""%1"""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\shell\AIEdit"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenWith}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\shell\AIEdit"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\bin\aiedit.exe,0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\shell\AIEdit\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\aiedit.exe"" ""%1"""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\AIEdit"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenWith}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\AIEdit"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\bin\aiedit.exe,0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\AIEdit\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\aiedit.exe"" ""%V"""; Tasks: contextmenu

; "Open with": the program, and a file type of its own (AIEdit.File) that the extensions in
; [Code] list under OpenWithProgids. Defaults stay as they are; Settings > Default apps can make
; AIEdit the default for those extensions.
Root: HKA; Subkey: "Software\Classes\Applications\aiedit.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "AIEdit"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\aiedit.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\aiedit.exe"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\AIEdit.File"; ValueType: string; ValueName: ""; ValueData: "AIEdit"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\AIEdit.File\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\aiedit.exe,0"; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\AIEdit.File\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\aiedit.exe"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\AIEdit\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "AIEdit"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\AIEdit\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "A fast viewer for text and binary files, with AI to ask"; Tasks: openwith
Root: HKA; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "AIEdit"; ValueData: "Software\AIEdit\Capabilities"; Flags: uninsdeletevalue; Tasks: openwith

[Run]
Filename: "{app}\bin\aiedit.exe"; Description: "{cm:LaunchProgram,AIEdit}"; Flags: nowait postinstall skipifsilent

[Code]
const
  // What "Open with" offers AIEdit for: text, logs, data, sources, and binaries for the hex view.
  Extensions = '.txt .text .log .out .md .markdown .json .jsonl .xml .yaml .yml .toml .ini .cfg .conf .properties .csv .tsv .srt .nfo .diff .patch .sql .sh .bat .cmd .ps1 .py .c .cc .cpp .cxx .h .hh .hpp .cs .java .js .ts .go .rs .html .htm .css .cmake .bin .dat .dmp .exe .dll .sys .obj .o .so .elf .img';

function TypesRoot: Integer;
begin
  if IsAdminInstallMode then
    Result := HKEY_LOCAL_MACHINE
  else
    Result := HKEY_CURRENT_USER;
end;

// Lists AIEdit for each extension, or takes it off again.
procedure ListForExtensions(List: Boolean);
var
  Rest, Extension, Key: String;
  Space: Integer;
begin
  Rest := Extensions + ' ';
  while Rest <> '' do
  begin
    Space := Pos(' ', Rest);
    Extension := Copy(Rest, 1, Space - 1);
    Delete(Rest, 1, Space);
    if Extension <> '' then
    begin
      Key := 'Software\Classes\' + Extension + '\OpenWithProgids';
      if List then
      begin
        RegWriteStringValue(TypesRoot, Key, 'AIEdit.File', '');
        RegWriteStringValue(TypesRoot, 'Software\Classes\Applications\aiedit.exe\SupportedTypes', Extension, '');
        RegWriteStringValue(TypesRoot, 'Software\AIEdit\Capabilities\FileAssociations', Extension, 'AIEdit.File');
      end
      else
      begin
        RegDeleteValue(TypesRoot, Key, 'AIEdit.File');
        RegDeleteKeyIfEmpty(TypesRoot, Key);
        RegDeleteKeyIfEmpty(TypesRoot, 'Software\Classes\' + Extension);
      end;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('openwith') then
    ListForExtensions(True);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // The rest goes with the keys [Registry] made.
  if CurUninstallStep = usPostUninstall then
    ListForExtensions(False);
end;
