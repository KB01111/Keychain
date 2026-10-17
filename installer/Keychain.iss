; installer/Keychain.iss - Keychain 1.0.0 per-user (nonadministrative) installer [R17].
; Build via scripts/package.ps1 (ISCC /DSourceRoot=<portableDir> /DAppVersion=1.0.0)
; or directly:  ISCC /DSourceRoot=..\dist\Keychain-1.0.0-win-x64 Keychain.iss

#ifndef SourceRoot
#define SourceRoot "..\dist\Keychain-1.0.0-win-x64"
#endif
#ifndef AppVersion
#define AppVersion "1.0.0"
#endif

[Setup]
AppId={{8B7A2C1D-9F3E-4A5B-B6C8-1D2E3F4A5B6C}
AppName=Keychain
AppVersion={#AppVersion}
AppPublisher=Keychain Project
UninstallDisplayName=Keychain {#AppVersion}
DefaultDirName={localappdata}\Programs\Keychain
DefaultGroupName=Keychain
; Per-user install: never request elevation [R17]
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
MinVersion=10.0.22000
DisableProgramGroupPage=yes
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=Keychain-Setup-{#AppVersion}-x64

[Files]
; Native launcher and its runtime dependencies
Source: "{#SourceRoot}\Keychain.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; Precompiled DX12 shaders (required at startup)
Source: "{#SourceRoot}\assets\shaders\*.cso"; DestDir: "{app}\assets\shaders"; Flags: ignoreversion
; Settings app incl. app-local Windows App SDK / .NET self-contained runtime
Source: "{#SourceRoot}\settings\*"; DestDir: "{app}\settings"; Flags: ignoreversion recursesubdirs createallsubdirs
; Third-party license notices (nlohmann/json, doctest)
Source: "..\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Keychain"; Filename: "{app}\Keychain.exe"; WorkingDir: "{app}"; Comment: "Keychain radial launcher (starts in the tray area)"
Name: "{group}\Keychain Settings"; Filename: "{app}\settings\Keychain.Settings.exe"; WorkingDir: "{app}\settings"; Comment: "Configure Keychain actions, profiles, and appearance"

[Run]
Filename: "{app}\Keychain.exe"; Description: "Launch Keychain now"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent unchecked

[UninstallRun]
; Make sure a running launcher is stopped before files are removed.
Filename: "{cmd}"; Parameters: "/C taskkill /IM Keychain.exe /F"; Flags: runhidden; RunOnceId: "StopLauncher"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    // Remove the per-user startup registration (the launcher may have added it).
    RegDeleteValue(HKEY_CURRENT_USER,
      'Software\Microsoft\Windows\CurrentVersion\Run', 'Keychain');

    // Preserve user configuration by default; delete only on explicit selection.
    // SuppressibleMsgBox: hidden during /SILENT and /VERYSILENT (answers IDNO,
    // i.e. silent uninstalls preserve the user's configuration).
    if SuppressibleMsgBox(
        'Also delete your Keychain settings and profiles?' + #13#10 +
        '(%LOCALAPPDATA%\Keychain will be removed)' + #13#10#13#10 +
        'Choose No to keep your configuration for a future install.',
        mbConfirmation, MB_YESNO, IDNO) = IDYES then
    begin
      DelTree(ExpandConstant('{localappdata}\Keychain'), True, True, True);
    end;
  end;
end;
