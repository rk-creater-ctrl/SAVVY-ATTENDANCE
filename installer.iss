#define MyAppName "Savvy Attendance System"
#define MyAppVersion "2.0.0"
#define MyAppPublisher "Savvy"
#define MyAppExeName "SavvyAttendanceQt.exe"

[Setup]
AppId={{E65D6F50-13D4-4AE9-8B81-54E35BC46927}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName=C:\SavvyAttendanceSystem
DisableDirPage=no
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=dist
OutputBaseFilename=SavvyAttendanceSetup
SetupIconFile=installer_staging\app_icon.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#MyAppExeName}
AlwaysShowComponentsList=no
DisableWelcomePage=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Dirs]
Name: "{app}"; Permissions: users-full
Name: "{app}\dataset"; Permissions: users-full
Name: "{app}\reports"; Permissions: users-full
Name: "{app}\unknown_faces"; Permissions: users-full

[Files]
; Microsoft Visual C++ Redistributable (x64)
Source: "vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

; Database - preserve existing records if already installed, never delete on uninstall
Source: "installer_staging\attendance.db"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall; Permissions: users-full

; Labels - preserve existing school labels if already present, never delete on uninstall
Source: "installer_staging\labels.txt"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall; Permissions: users-full

; All application binaries, Qt plugins, deep learning models, sound, and resources
Source: "installer_staging\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion; Excludes: "attendance.db,labels.txt"

[Icons]
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app_icon.ico"
Name: "{autodesktop}\Savvy Photos & Dataset"; Filename: "{app}\dataset"; IconFilename: "{app}\app_icon.ico"
Name: "{autodesktop}\Savvy Attendance Reports"; Filename: "{app}\reports"; IconFilename: "{app}\app_icon.ico"
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app_icon.ico"
Name: "{group}\Photos & Dataset"; Filename: "{app}\dataset"
Name: "{group}\Attendance Reports"; Filename: "{app}\reports"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/passive /norestart"; Check: VCRedistNeedsInstall; StatusMsg: "Installing Microsoft Visual C++ 2015-2022 Redistributable (x64)..."; Flags: waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
function VCRedistNeedsInstall: Boolean;
var
  installed: Cardinal;
begin
  Result := not (RegQueryDWordValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', installed) and (installed = 1));
end;
