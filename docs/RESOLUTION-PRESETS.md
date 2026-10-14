# 既存環境で解像度を追加する

VKomadoの管理画面を閉じ、インストール先の `Tools/Enable-ResolutionPresets.cmd`（ソースからの場合は `scripts/Enable-ResolutionPresets.cmd`）を実行し、Windowsの管理者確認を承認する。仮想画面のウィンドウは既存の退避処理でメイン画面へ戻してからドライバーをOFFにする。退避に失敗すると設定変更を中止する。

1920×1080を残し、1280×720、2560×1440、3840×2160を追加する。すべて60Hz。GPU、HDR、既存の解像度・リフレッシュレートなど他の設定は維持する。設定ファイルの隣に `.before-resolution-presets-日時-GUID.bak` のバックアップを保存する。

反映後、Windowsの「設定 → システム → ディスプレイ」で仮想画面を選び、「ディスプレイの解像度」を変更する。VKomadoからビューアを開き直す。成功メッセージを確認したら管理者PowerShellの画面を閉じてよい。アプリ内の解像度選択と次回ON時の設定維持は1.2.0では未対応。OFF→ONで1080pへ戻った場合はWindowsの設定から再選択する。

実行前の確認だけなら、通常のPowerShellで次を実行する。ファイル・画面構成・ウィンドウ配置は変更しない。

```powershell
.\scripts\Enable-ResolutionPresets.ps1 -Preview
```

スクリプトは導入記録と対象VDDのハードウェアIDを確認し、VDDPATHがある場合はその参照先を使用する。対応する配置先は `C:\VirtualDisplayDriver` または `%ProgramFiles%\SharedVD\Driver`。1モニターの既存XML設定に限り処理する。ドライバーのインストール・アンインストールやプライマリ画面の変更は行わない。

再読み込み後、Windowsが4種類のモードを列挙することを検証する。失敗した場合は元のXMLとON状態の復旧を試み、失敗箇所とバックアップの場所を表示する。操作前からOFFだった場合は成功後もOFFに戻す。自動再起動はしない。

`tests/resolution_presets_tests.ps1` は専用のXMLと一時ファイルを使い、解像度追加・既存設定保持・重複防止・不正XML拒否・ファイル置換・バックアップ・復旧を検査する。実機のVDD設定は変更しない。
