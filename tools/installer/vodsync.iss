; vodsync - Windows installer (Inno Setup 6.3 or newer).
;
; Build it with:  python tools/build.py installer
; which reads the version out of src/vs_version.h and passes it in as AppVer,
; so this file never carries a version of its own.
;
; Three decisions worth knowing before reading the script:
;
; 1. PER-USER, NO ADMIN. PrivilegesRequired=lowest, so {autopf} resolves to
;    {localappdata}\Programs and the whole install happens without a UAC
;    prompt. That is not only politeness: it is what lets the app keep
;    working. vs_state_dir writes beside the exe when that folder is
;    writable, and falls back to %LOCALAPPDATA%\vodsync when it is not. A
;    per-user install folder IS writable, so an installed copy behaves
;    exactly like the portable one - the saved channels, the session and the
;    chat logs sit next to vodsync.exe. An admin install into Program Files
;    would instead take the fallback. Both work; one has fewer differences
;    to remember, and `vodsync.exe --where` prints which you got.
;
; 2. NO CRT REDISTRIBUTABLE. vcruntime140.dll is installed beside the exe
;    (app-local deployment). That is why there is no [Run] entry chaining
;    vc_redist.x64.exe, which would have needed admin and undone point 1.
;    The rest of the C runtime is part of Windows 10 and 11.
;
; 3. CHAT REPLAY IS A COMPONENT, because it is 66 MB of TwitchDownloaderCLI
;    and because it needs python, which this installer does not ship and
;    cannot sensibly bundle - vs_chat_sync.py and vs_chat_prep.py are
;    scripts, not binaries. Playback needs none of it.

#define AppName    "vodsync"
#define Root        AddBackslash(SourcePath) + "..\.."
#define BuildDir    Root + "\build"
#define ExePath     BuildDir + "\vodsync.exe"
#define TdlDir      Root + "\third_party\twitchdownloader"
#define IcoPath     Root + "\src\vodsync.ico"
#define YtDlpPath   BuildDir + "\yt-dlp.exe"

; Nothing here is built by this script; it only packs what is already there.
#if !FileExists(ExePath)
  #error vodsync.exe not found - run "python tools/build.py app" first
#endif
#if !FileExists(BuildDir + "\libmpv-2.dll")
  #error libmpv-2.dll not found next to the exe - the build copies it there
#endif
#if !FileExists(BuildDir + "\vcruntime140.dll")
  #error vcruntime140.dll not found next to the exe - the build copies it there
#endif

; build.py passes this. The fallback is for running iscc by hand.
#ifndef AppVer
  #define AppVer GetStringFileInfo(ExePath, "FileVersion")
#endif

[Setup]
; Never change AppId: it is how Windows recognises an upgrade of this app
; rather than a second copy of it.
AppId={{FC781880-DC07-44D1-BDFB-1E47C0E0C46D}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppName} {#AppVer}
AppPublisher={#AppName}
VersionInfoVersion={#AppVer}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The exe is normally in daily use, so a reinstall over a running copy is the
; common case, not the awkward one. Restart Manager closes it rather than
; failing on a locked libmpv-2.dll; it is not restarted, because [Run] offers
; that at the end and starting it twice would be worse.
CloseApplications=yes
RestartApplications=no
OutputDir={#BuildDir}\installer
OutputBaseFilename={#AppName}-{#AppVer}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName} {#AppVer}
UninstallDisplayIcon={app}\vodsync.exe
#if FileExists(IcoPath)
SetupIconFile={#IcoPath}
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";    Description: "Everything"
Name: "compact"; Description: "Playback only (no chat replay)"
Name: "custom";  Description: "Custom"; Flags: iscustom

[Components]
Name: "core"; Description: "{#AppName}"; Types: full compact custom; Flags: fixed
Name: "chat"; Description: "Chat replay and export (needs python installed separately)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#ExePath}";                       DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#BuildDir}\libmpv-2.dll";         DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#BuildDir}\vcruntime140.dll";     DestDir: "{app}"; Components: core
; yt-dlp is vendored in third_party (fetched by hand, checksum-verified
; against the release's SHA2-256SUMS) and copied into build\ by build.py, the
; way libmpv-2.dll is. Still conditional: without it the app falls back to
; "python -m yt_dlp" and would need python for playback, not just for chat.
#if FileExists(YtDlpPath)
Source: "{#YtDlpPath}";                     DestDir: "{app}"; Components: core; Flags: ignoreversion
#endif
; vs_chat_sync.py looks for TwitchDownloaderCLI in its own parent directory,
; among other places, so {app} is where this belongs - not {app}\tools.
Source: "{#TdlDir}\TwitchDownloaderCLI.exe"; DestDir: "{app}";       Components: chat; Flags: ignoreversion
Source: "{#Root}\tools\vs_chat_sync.py";     DestDir: "{app}\tools"; Components: chat; Flags: ignoreversion
Source: "{#Root}\tools\vs_chat_prep.py";     DestDir: "{app}\tools"; Components: chat; Flags: ignoreversion
; Both scripts import this as a sibling module - it is the one definition of
; the chat log format. Leave it out and an installed copy fails every export
; with ModuleNotFoundError while the build it was made from works fine.
Source: "{#Root}\tools\vs_chat_fmt.py";      DestDir: "{app}\tools"; Components: chat; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";       Filename: "{app}\vodsync.exe"
Name: "{group}\Where it keeps its files"; Filename: "{app}\vodsync.exe"; Parameters: "--where"; Comment: "Print the state folder and the helper commands"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\vodsync.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\vodsync.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
// What the app wrote while it ran is not what the installer put there, so
// Inno will not remove it and it is simply left behind. Offer, once, and
// default to No: these are the saved channels, the sessions and the exported
// chat logs, and someone uninstalling in order to reinstall wants them.

function AskAbout(Dir: String): Boolean;
begin
  Result := MsgBox('Also delete your saved channels, sessions and chat logs?'
                   + #13#10#13#10 + Dir + #13#10#13#10
                   + 'Choose No to keep them for a later install.',
                   mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
end;

// Named patterns rather than the whole folder. The install folder is the
// user's choice on the directory page - it could be one they already keep
// other things in - so deleting a tree there would be a fine way to destroy
// files that were never ours. %LOCALAPPDATA%\vodsync is a folder the app
// creates and owns, and that one can go entirely.
procedure DeleteStateFiles(Dir: String);
begin
  DelTree(Dir + '\vodsync.channels', False, True, False);
  DelTree(Dir + '\vodsync.autosave', False, True, False);
  DelTree(Dir + '\*.session',        False, True, False);
  DelTree(Dir + '\chat_*.tsv',       False, True, False);
  DelTree(Dir + '\*.vodsync.tsv',    False, True, False);
  // The cached emote images. A directory the app creates and owns, so
  // unlike the install folder it can go whole - and unlike the files above
  // it would otherwise be the one thing an uninstall left behind.
  DelTree(Dir + '\emotes', True, True, True);
end;

function HasStateFiles(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\vodsync.channels')
         or FileExists(Dir + '\vodsync.autosave')
         or DirExists(Dir + '\emotes');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppDir, UserDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  // A silent uninstall has nobody to ask, so it must not delete anything.
  //
  // MB_DEFBUTTON2 above only decides which button is focused for a human.
  // It does NOT decide the answer /SUPPRESSMSGBOXES gives: that answers a
  // MB_YESNO box with Yes. So without this guard, "vodsync-setup.exe
  // /VERYSILENT" style automation - or anything else that uninstalls
  // without a window - would wipe the saved channels and the session with
  // no one having agreed to it. Verified by watching it happen.
  if UninstallSilent() then
    exit;
  // Beside the exe after a per-user install; the per-user folder after one
  // that landed somewhere read-only. In practice only one of the two exists,
  // so this asks at most once.
  AppDir  := ExpandConstant('{app}');
  UserDir := ExpandConstant('{localappdata}\vodsync');
  if HasStateFiles(AppDir) and AskAbout(AppDir) then
    DeleteStateFiles(AppDir);
  if DirExists(UserDir) and AskAbout(UserDir) then
    DelTree(UserDir, True, True, True);
end;
