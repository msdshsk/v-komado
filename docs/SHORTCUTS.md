# ショートカット設定

管理画面の「ショートカット設定」、またはビューアの「表示 → ショートカット設定」から開く。管理画面からは仮想ディスプレイがOFFでも設定できる。ポータブル版は `SharedVD.exe --shortcuts` でも開ける。

変更できるのは「メイン画面へカーソルを戻す」「ウィンドウを選んで移動」の2つ。初期値はF6、Shift+F6。入力欄にフォーカスを移し、好きな組み合わせを押して保存する。「使用する」を外すと、その操作のグローバルキーを無効にできる。どちらも無効でも、表示メニューや通常のマウス移動は使える。

Ctrl／Alt／Shiftとキーの組み合わせ、またはF1～F24の単独キーに対応する。文字など通常キーにはCtrlまたはAltが必要。F12はWindowsのデバッガー用、Windowsキーの組み合わせはOS用の予約があるため対象外。Alt+F4など基本的なシステム操作も除外する。Fnキー自体の扱いはキーボードのハードウェア設定に依存する。

設定中は既存のグローバルキーを一時解除し、元のキーも入力できる。保存すると即時反映。キャンセルや×なら元の設定を再登録する。設定はユーザー別の `%LOCALAPPDATA%\SharedVD\hotkeys.conf` に保存し、次回起動に復元する。保存失敗時は以前のファイルを残す。読めない設定は初期値へ戻し、設定画面に説明を表示する。

2操作の重複、他のグローバルキーとの登録競合は保存時に検出する。ただし、PhotoshopやVS Codeがウィンドウ内だけで処理するキーとの重複は検出できない。登録に成功したキーはそれらより優先されることがあるので、普段使うアプリに合わせて変更する。

アプリ全般で未使用と保証できるFキーはない。PhotoshopはF5～F9、VS CodeはF5・F6・F9などを使う。マクロキーボードでF13～F24を出力できる場合も候補になるが、これも空きを保証しない。

参照：[RegisterHotKey](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey)、[VS Codeの既定キー](https://code.visualstudio.com/docs/reference/default-keybindings)、[Photoshopのキー一覧](https://helpx.adobe.com/content/dam/help/en/photoshop/using/default-keyboard-shortcuts/photoshop-keyboard-shortcuts.pdf)。

`tests/hotkeys_tests.cpp` では保存・読込・無効化・重複・グローバル競合・保存失敗・破損ファイル・設定画面のキャンセルと初期値復元を検査する。ユーザー設定の代わりに一時的なテスト用ファイルを使う。ビューアの回帰テストも `--hotkey-config` でユーザー設定から分離する。
