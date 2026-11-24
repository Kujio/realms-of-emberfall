; Inno Setup script for the Windows client (Phase 8). Built by the release workflow on a Windows runner:
;   iscc /DAppVersion=0.4.0 /DSourceExe=..\..\client\build\EmberfallClient.exe deploy\windows\emberfall.iss
; Output: deploy\windows\Output\EmberfallSetup-<version>.exe. The portable EmberfallClient.exe is always
; shipped next to it; the installer only adds Start menu / desktop shortcuts and an uninstaller.
; Unsigned builds trigger Windows SmartScreen ("More info" -> "Run anyway"); see docs/BUILD.md.

#ifndef AppVersion
  #define AppVersion "0.4.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\..\client\build\EmberfallClient.exe"
#endif

[Setup]
AppId={{6E1B6B5C-2F0A-4C8E-9C51-6B2A9F0E7A11}
AppName=Realms of Emberfall
AppVersion={#AppVersion}
AppVerName=Realms of Emberfall {#AppVersion}
AppPublisher=Realms of Emberfall
DefaultDirName={autopf}\Realms of Emberfall
DefaultGroupName=Realms of Emberfall
DisableProgramGroupPage=yes
; Per-user install by default (no administrator prompt); the user may pick all users instead.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=EmberfallSetup-{#AppVersion}
SetupIconFile=..\..\client\icon.ico
UninstallDisplayIcon={app}\EmberfallClient.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName=Realms of Emberfall

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\..\ASSETS.md"; DestDir: "{app}"; DestName: "ASSETS.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\Realms of Emberfall"; Filename: "{app}\EmberfallClient.exe"
Name: "{group}\Uninstall Realms of Emberfall"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Realms of Emberfall"; Filename: "{app}\EmberfallClient.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\EmberfallClient.exe"; Description: "{cm:LaunchProgram,Realms of Emberfall}"; Flags: nowait postinstall skipifsilent
