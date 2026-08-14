#define AppName "Files4Me"
#ifndef AppVersion
#define AppVersion "1.0-release"
#endif
#ifndef AppNumericVersion
#define AppNumericVersion "1.0.0.0"
#endif
#define AppExeName "Files4Me.exe"

[Setup]
AppId={{E763A42A-B65C-40CE-8893-0A534906575B}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=adamng04 and Files4Me contributors
AppCopyright=Copyright (c) 2026 adamng04 and Files4Me contributors
VersionInfoVersion={#AppNumericVersion}
VersionInfoDescription=Files4Me {#AppVersion} installer
DefaultDirName={localappdata}\Programs\Files4Me
DefaultGroupName=Files4Me
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
LicenseFile=..\LICENSE
SetupIconFile=..\assets\Files4Me.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName=Files4Me {#AppVersion}
OutputDir=..\dist\installer
OutputBaseFilename=Files4Me-{#AppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dynamic
CloseApplications=yes
RestartApplications=no
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "defaultmanager"; Description: "Make Files4Me the default file exploring app"; GroupDescription: "Windows integration:"; Flags: unchecked

[Files]
Source: "..\dist\Files4Me.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "installed.marker"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\assets\MATERIAL-ICONS-LICENSE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Files4Me"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\Files4Me"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\Directory\shell\Files4Me"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Open in Files4Me"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\shell\Files4Me"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExeName},0"
Root: HKCU; Subkey: "Software\Classes\Directory\shell\Files4Me\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\Drive\shell\Files4Me"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Open in Files4Me"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Drive\shell\Files4Me"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExeName},0"
Root: HKCU; Subkey: "Software\Classes\Drive\shell\Files4Me\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\Files4Me"; ValueType: string; ValueName: "MUIVerb"; ValueData: "Open folder in Files4Me"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\Files4Me"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#AppExeName},0"
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\Files4Me\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%V"""

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch Files4Me"; Flags: nowait postinstall skipifsilent

[Code]
const
  BackupKey = 'Software\Files4Me\Installer';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{E763A42A-B65C-40CE-8893-0A534906575B}_is1';

function InitializeSetup: Boolean;
var
  InstalledVersion: String;
begin
  Result := True;
  if RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', InstalledVersion) then
  begin
    if CompareText(InstalledVersion, '{#AppVersion}') = 0 then
      Result := MsgBox('Files4Me {#AppVersion} is already installed.' + #13#10 + #13#10 +
        'Reinstall the latest version?', mbConfirmation, MB_YESNO) = IDYES
    else
      Result := MsgBox('Files4Me ' + InstalledVersion + ' is installed.' + #13#10 + #13#10 +
        'Update Files4Me to {#AppVersion}?', mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

procedure SaveAndSetDefault(const ClassName, BackupName: String);
var
  Previous: String;
  BackupExists: Cardinal;
begin
  if RegQueryDWordValue(HKCU, BackupKey, BackupName + 'Exists', BackupExists) then
  begin
    RegWriteStringValue(HKCU, 'Software\Classes\' + ClassName + '\shell', '', 'Files4Me');
    exit;
  end;
  if RegQueryStringValue(HKCU, 'Software\Classes\' + ClassName + '\shell', '', Previous) then
  begin
    RegWriteDWordValue(HKCU, BackupKey, BackupName + 'Exists', 1);
    RegWriteStringValue(HKCU, BackupKey, BackupName, Previous);
  end
  else
    RegWriteDWordValue(HKCU, BackupKey, BackupName + 'Exists', 0);
  RegWriteStringValue(HKCU, 'Software\Classes\' + ClassName + '\shell', '', 'Files4Me');
end;

procedure RestoreDefault(const ClassName, BackupName: String);
var
  Current, Previous: String;
  Existed: Cardinal;
begin
  if RegQueryStringValue(HKCU, 'Software\Classes\' + ClassName + '\shell', '', Current) and
     (CompareText(Current, 'Files4Me') = 0) then
  begin
    if RegQueryDWordValue(HKCU, BackupKey, BackupName + 'Exists', Existed) and (Existed = 1) and
       RegQueryStringValue(HKCU, BackupKey, BackupName, Previous) then
      RegWriteStringValue(HKCU, 'Software\Classes\' + ClassName + '\shell', '', Previous)
    else
      RegDeleteValue(HKCU, 'Software\Classes\' + ClassName + '\shell', '');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('defaultmanager') then
  begin
    SaveAndSetDefault('Directory', 'PreviousDirectoryVerb');
    SaveAndSetDefault('Drive', 'PreviousDriveVerb');
  end;
  if (CurStep = ssPostInstall) and not WizardIsTaskSelected('defaultmanager') then
  begin
    RestoreDefault('Directory', 'PreviousDirectoryVerb');
    RestoreDefault('Drive', 'PreviousDriveVerb');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RestoreDefault('Directory', 'PreviousDirectoryVerb');
    RestoreDefault('Drive', 'PreviousDriveVerb');
    RegDeleteKeyIncludingSubkeys(HKCU, BackupKey);
  end;
end;
