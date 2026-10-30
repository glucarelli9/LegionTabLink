[Setup]
AppName=GL ExtraScreen
AppVersion=1.3
AppPublisher=Gianfranco Lucarelli
AppPublisherURL=https://github.com/glucarelli9/LegionTabLink
AppSupportURL=https://github.com/glucarelli9/LegionTabLink/issues
AppContact=glucarelli9@gmail.com
DefaultDirName={autopf}\GLExtraScreen
DefaultGroupName=GL ExtraScreen
OutputDir=output
OutputBaseFilename=GLExtraScreen-Setup
SetupIconFile=bundle\GLExtraScreen.ico
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
Source: "bundle\GLExtraScreen.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "bundle\adb.exe"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\AdbWinApi.dll"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\AdbWinUsbApi.dll"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\libwinpthread-1.dll"; DestDir: "{app}"; Components: adb; Flags: ignoreversion
Source: "bundle\ParsecVDisplay-v0.45-setup.exe"; DestDir: "{tmp}"; Components: vdd; Flags: deleteafterinstall
Source: "bundle\GLExtraScreen.apk"; DestDir: "{app}"; Components: apk; Flags: ignoreversion

[Icons]
Name: "{autodesktop}\GL ExtraScreen"; Filename: "{app}\GLExtraScreen-Server.exe"; IconFilename: "{app}\GLExtraScreen.ico"; Tasks: desktopicon; Comment: "Start GL ExtraScreen Server"
Name: "{group}\GL ExtraScreen"; Filename: "{app}\GLExtraScreen-Server.exe"; IconFilename: "{app}\GLExtraScreen.ico"
Name: "{group}\Uninstall GL ExtraScreen"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "GLExtraScreen"; ValueData: """{app}\GLExtraScreen-Server.exe"""; Tasks: autostart; Flags: uninsdeletevalue
; Create the ParsecVDD custom modes key with Permissions so the server
; can write custom resolutions at runtime without admin rights.
Root: HKLM; Subkey: "SOFTWARE\Parsec\vdd"; Permissions: users-modify; Flags: uninsdeletekeyifempty

[Run]
Filename: "{tmp}\ParsecVDisplay-v0.45-setup.exe"; Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"; StatusMsg: "Installing Virtual Display Driver..."; Components: vdd; Flags: waituntilterminated; Check: not IsVDDInstalled
Filename: "{app}\GLExtraScreen-Server.exe"; Description: "Launch GL ExtraScreen now"; Flags: nowait postinstall skipifsilent unchecked

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM GLExtraScreen-Server.exe"; Flags: runhidden; RunOnceId: "KillServer"
Filename: "taskkill.exe"; Parameters: "/F /IM ParsecVDisplay.exe"; Flags: runhidden; RunOnceId: "KillParsec"
Filename: "{app}\adb.exe"; Parameters: "kill-server"; Flags: runhidden; RunOnceId: "KillAdb"
Filename: "{pf}\ParsecVDisplay\unins000.exe"; Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"; Flags: runhidden skipifdoesntexist; RunOnceId: "UninstallVDD64"
Filename: "{pf32}\ParsecVDisplay\unins000.exe"; Parameters: "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"; Flags: runhidden skipifdoesntexist; RunOnceId: "UninstallVDD32"

[Messages]
WelcomeLabel1=Welcome to GL ExtraScreen
WelcomeLabel2=Turn your Android tablet into a second monitor over USB.%n%nBy Gianfranco Lucarelli%nglucarelli9@gmail.com%n%nThis will install the server, a virtual display driver, and ADB.
FinishedHeadingLabel=GL ExtraScreen is ready!
FinishedLabel=Launch GL ExtraScreen from the desktop shortcut.%nThe server window will guide you through the setup.

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
  if CurStep = ssInstall then
    Exec('taskkill.exe', '/F /IM GLExtraScreen-Server.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

  if CurStep = ssPostInstall then
  begin
    AdbPath := ExpandConstant('{app}\adb.exe');
    ApkPath := ExpandConstant('{app}\GLExtraScreen.apk');
    if FileExists(ApkPath) and FileExists(AdbPath) then
    begin
      Exec(AdbPath, 'start-server', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
      Exec(AdbPath, 'install -r "' + ApkPath + '"', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
      // An older build signed with a different key cannot be updated in place
      if ResultCode <> 0 then
      begin
        Exec(AdbPath, 'uninstall com.legion.tablink', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
        Exec(AdbPath, 'install "' + ApkPath + '"', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
      end;
    end;
  end;
end;
