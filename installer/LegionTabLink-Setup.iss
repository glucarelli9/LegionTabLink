[Setup]
AppName=GL ExtraScreen
AppVersion=1.0
AppPublisher=GL ExtraScreen
AppPublisherURL=https://github.com/glucarelli9/LegionTabLink
DefaultDirName={autopf}\GLExtraScreen
DefaultGroupName=GL ExtraScreen
OutputDir=output
OutputBaseFilename=GLExtraScreen-Setup
SetupIconFile=bundle\LegionTabLink.ico
UninstallDisplayIcon={app}\GLExtraScreen-Server.exe
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
WizardStyle=modern
DisableProgramGroupPage=yes
UninstallDisplayName=GL ExtraScreen
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "autostart"; Description: "Launch GL ExtraScreen when Windows starts"; GroupDescription: "Startup:"

[Types]
Name: "full"; Description: "Full installation (Server + Virtual Display Driver)"
Name: "server"; Description: "Server only (Virtual Display Driver already installed)"

[Components]
Name: "server"; Description: "GL ExtraScreen Server"; Types: full server; Flags: fixed
Name: "adb"; Description: "Android Debug Bridge (ADB)"; Types: full server; Flags: fixed
Name: "vdd"; Description: "Parsec Virtual Display Driver"; Types: full
Name: "apk"; Description: "GL ExtraScreen APK (auto-installed on tablet)"; Types: full server

[Files]
Source: "bundle\GLExtraScreen-Server.exe"; DestDir: "{app}"; Components: server; Flags: ignoreversion
Source: "bundle\LegionTabLink.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "bundle\adb.exe"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\AdbWinApi.dll"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\AdbWinUsbApi.dll"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\libwinpthread-1.dll"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\ParsecVDisplay-v0.45-setup.exe"; DestDir: "{tmp}"; Components: vdd; Flags: deleteafterinstall
Source: "bundle\GLExtraScreen.apk"; DestDir: "{app}"; Components: apk; Flags: ignoreversion

[Icons]
Name: "{autodesktop}\GL ExtraScreen"; Filename: "{app}\GLExtraScreen-Server.exe"; IconFilename: "{app}\LegionTabLink.ico"; Tasks: desktopicon; Comment: "Start GL ExtraScreen Server"
Name: "{group}\GL ExtraScreen"; Filename: "{app}\GLExtraScreen-Server.exe"; IconFilename: "{app}\LegionTabLink.ico"
Name: "{group}\Uninstall GL ExtraScreen"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "GLExtraScreen"; ValueData: """{app}\GLExtraScreen-Server.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{tmp}\ParsecVDisplay-v0.45-setup.exe"; Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"; StatusMsg: "Installing Virtual Display Driver..."; Components: vdd; Flags: waituntilterminated; Check: not IsVDDInstalled
Filename: "{app}\GLExtraScreen-Server.exe"; Description: "Launch GL ExtraScreen now"; Flags: nowait postinstall skipifsilent unchecked

[UninstallRun]
Filename: "{app}\adb.exe"; Parameters: "kill-server"; Flags: runhidden

[Messages]
WelcomeLabel1=Welcome to GL ExtraScreen
WelcomeLabel2=This will install GL ExtraScreen on your computer.%n%nGL ExtraScreen turns your Android tablet into a wireless second monitor.%n%nWhat gets installed:%n  - GL ExtraScreen Server (with desktop shortcut)%n  - Virtual Display Driver (creates a virtual monitor)%n  - Android Debug Bridge (for tablet communication)%n%nThe app will be automatically installed on your tablet if connected via USB.

[Code]
function IsVDDInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{pf}\ParsecVDisplay\ParsecVDisplay.exe'))
         or FileExists(ExpandConstant('{pf32}\ParsecVDisplay\ParsecVDisplay.exe'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ApkPath: String;
  AdbPath: String;
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    AdbPath := ExpandConstant('{app}\adb.exe');
    ApkPath := ExpandConstant('{app}\GLExtraScreen.apk');
    if FileExists(ApkPath) and FileExists(AdbPath) then
    begin
      Exec(AdbPath, 'start-server', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec(AdbPath, 'install -r "' + ApkPath + '"', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;
  end;
end;
