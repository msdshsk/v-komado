#ifndef Root
  #error Root is required
#endif
[Setup]
; Keep the original identity and directory to upgrade SharedVD in place.
AppId={{2D2B6B84-29EF-4D46-B60F-099928D881F7}
AppName=VKomado
AppVersion=1.2.1
AppPublisher=VKomado Project
DefaultDirName={autopf}\SharedVD
DisableDirPage=yes
UsePreviousAppDir=no
DefaultGroupName=VKomado
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
PrivilegesRequired=admin
WizardStyle=modern
OutputDir={#Root}\artifacts\releases
OutputBaseFilename=VKomado-1.2.1-Setup-win-x64
Compression=lzma2
SolidCompression=yes
UninstallDisplayIcon={app}\VKomado.Control.exe
SetupIconFile={#Root}\assets\sharedvd.ico
CloseApplications=yes
RestartApplications=no
AppMutex=Local\SharedVD.Control
InfoBeforeFile={#Root}\packaging\SETUP-NOTES.txt

[Languages]
Name: japanese; MessagesFile: compiler:Languages\Japanese.isl

[Files]
Source: {#Root}\artifacts\manager-build\Release\VKomado.Control.exe; DestDir: {app}; Flags: ignoreversion
Source: {#Root}\artifacts\manager-build\Release\VKomado.Device.exe; DestDir: {app}; Flags: ignoreversion
Source: {#Root}\artifacts\viewer-build\Release\lgwl_viewer.exe; DestDir: {app}; DestName: VKomado.Viewer.exe; Flags: ignoreversion
Source: {#Root}\artifacts\downloads\driver-25.7.23\VirtualDisplayDriver\MttVDD.inf; DestDir: {app}\Driver; Flags: ignoreversion
Source: {#Root}\artifacts\downloads\driver-25.7.23\VirtualDisplayDriver\MttVDD.dll; DestDir: {app}\Driver; Flags: ignoreversion
Source: {#Root}\artifacts\downloads\driver-25.7.23\VirtualDisplayDriver\mttvdd.cat; DestDir: {app}\Driver; Flags: ignoreversion
Source: {#Root}\config\vdd-distribution.xml; DestDir: {app}\Driver; DestName: vdd_settings.xml; Flags: onlyifdoesntexist uninsneveruninstall
Source: {#Root}\third_party\licenses\VDD-MIT.txt; DestDir: {app}\Licenses; DestName: VDD-MIT.txt
Source: {#Root}\third_party\licenses\Loopback-MIT.txt; DestDir: {app}\Licenses; DestName: Loopback-MIT.txt
Source: {#Root}\LICENSE; DestDir: {app}; DestName: LICENSE.txt
Source: {#Root}\THIRD-PARTY-NOTICES.md; DestDir: {app}
Source: {#Root}\scripts\Enable-ResolutionPresets.cmd; DestDir: {app}\Tools
Source: {#Root}\scripts\Enable-ResolutionPresets.ps1; DestDir: {app}\Tools
Source: {#Root}\scripts\ResolutionPresets.psm1; DestDir: {app}\Tools
Source: {#Root}\packaging\SETUP-NOTES.txt; DestDir: {app}; DestName: README.txt
Source: {#Root}\artifacts\manager-build\Release\VKomado.Device.exe; Flags: dontcopy

[Icons]
Name: {autoprograms}\VKomado; Filename: {app}\VKomado.Control.exe

[Run]
Filename: {app}\VKomado.Control.exe; Description: VKomadoを開く; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
var
  DriverNeedsRestart: Boolean;

procedure RemoveLegacyFile(const Path: String);
begin
  if FileExists(Path) and not DeleteFile(Path) then
    Log('Could not remove legacy file: ' + Path);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  if CheckForMutexes('Local\SharedVdLoopbackViewer') then begin
    Result := 'SharedVD / VKomadoのビューアを終了してから、もう一度実行してください。';
    Exit;
  end;
  ExtractTemporaryFile('VKomado.Device.exe');
  if not Exec(ExpandConstant('{tmp}\VKomado.Device.exe'), '--preflight --ui-errors', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := '既存の仮想ディスプレイを確認できませんでした。'
  else if Code <> 0 then
    Result := '既存のVDDまたは未完了の導入を確認してください。設定は上書きしていません。'
  else
    Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssPostInstall then begin
    if not Exec(ExpandConstant('{app}\VKomado.Device.exe'), '--install --ui-errors', '', SW_HIDE, ewWaitUntilTerminated, Code) then
      RaiseException('ドライバーのセットアップを開始できませんでした。');
    if Code = 3010 then DriverNeedsRestart := True
    else if Code <> 0 then
      RaiseException('ドライバーのセットアップが完了しませんでした。VKomadoのアンインストールで導入途中の状態を整理できます。');
    { Remove only obsolete app files after the new helper accepted the existing state. }
    RemoveLegacyFile(ExpandConstant('{app}\SharedVD.Control.exe'));
    RemoveLegacyFile(ExpandConstant('{app}\SharedVD.Device.exe'));
    RemoveLegacyFile(ExpandConstant('{app}\SharedVD.Viewer.exe'));
    RemoveLegacyFile(ExpandConstant('{autoprograms}\SharedVD.lnk'));
  end;
end;

function NeedRestart: Boolean;
begin
  Result := DriverNeedsRestart;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  { usUninstall runs after confirmation and before file removal. Inno 7.1
    propagates exceptions at this stage: files remain if driver cleanup fails. }
  if CurUninstallStep = usUninstall then begin
    if not Exec(ExpandConstant('{app}\VKomado.Device.exe'), '--uninstall --ui-errors', '', SW_HIDE, ewWaitUntilTerminated, Code) then
      RaiseException('退避・削除処理を開始できないため、アンインストールを中止しました。');
    if Code <> 0 then
      RaiseException('退避またはドライバー削除が完了しないため、アプリと復旧用ファイルを残しました。');
    { The distribution settings belong to this app, unlike any existing VDD settings. }
    if FileExists(ExpandConstant('{app}\Driver\vdd_settings.xml')) and
       not DeleteFile(ExpandConstant('{app}\Driver\vdd_settings.xml')) then
      RaiseException('設定ファイルを削除できません。ファイルを閉じてから再試行してください。');
  end;
end;
