# 公開と更新

- [GitHub Pages](https://msdshsk.github.io/v-komado/): 紹介・操作案内。`site/` のHTMLとCSSを使用する。
- [GitHub Releases](https://github.com/msdshsk/v-komado/releases): バージョンごとのセットアップEXE、ポータブルZIP、ソースZIP、SHA256を配布する。
- `main` へのpushでWindowsのビルドとXMLテストを実行する。Pagesは `site/`、アイコン、Pagesワークフローの変更時に自動更新する。

## 紹介ページの更新

`site/index.html`、`site/style.css` を編集して `main` にpushする。`.github/workflows/pages.yml` が必要なファイルとアイコンだけをPagesへ配信する。ローカルの `.tmp/`、`.tasks/`、ビルド出力は公開対象に含めない。

ページ内のバージョン表記と直接ダウンロードリンクは、アプリのリリース時に更新する。GitHub Pagesの設定は「GitHub Actions」を使用する。

## アプリのリリース

1. バージョン表記、`CHANGELOG.md`、配布物の説明書を更新する。インストーラーのAppIdや既存設定の保存先は、名称変更だけを理由に変更しない。
2. `scripts/Build-Setup.ps1` と `scripts/Package-Viewer.ps1` で配布物を作り、対象版のビルド・テスト・実機操作を確認する。
3. 変更をコミットしてpushし、GitHub Actionsの結果を確認する。配布EXEの元になったソースが、タグの指すコミットと一致していることを確認する。
4. バージョンタグ（例: `v1.0.0`）を作成しpushする。`git archive` でそのタグのソースZIPを作る。
5. EXE・ポータブルZIP・ソースZIPのSHA256を `SHA256SUMS.txt` に保存する。公開用チェックサムにはファイル名のみを使用し、ローカルの絶対パスは含めない。
6. Releasesの下書きを作成して配布物を添付する。変更点・動作環境・更新方法・制限を記載し、添付ファイルを確認して公開する。
7. Pagesのダウンロード先を更新し、公開URLとダウンロードを確認する。

リリースの公開は手動。`main` へのpushだけでは新しいアプリ版を公開しない。公開済みのタグや配布ファイルを差し替える場合は、利用者が変更を識別できる新しいバージョンを用意する。
