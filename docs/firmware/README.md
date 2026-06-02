# Mimic X Firmware Update

Mimic X アダプタ (CH32X035 搭載) のファームウェアを Web ブラウザから書き込むためのページです。

公開 URL: <https://kunichiko.github.io/MimicX-firmware/firmware/>

## 使い方

1. Chromium 系ブラウザ（Chrome, Edge）で [ファームウェア更新ページ](https://kunichiko.github.io/MimicX-firmware/firmware/) を**先に**開いておく
2. Mimic X アダプタの BOOT ボタンを押しながら USB ケーブルを接続する
3. **すぐに**「アダプタに接続」を押し、ブラウザのダイアログで「QinHeng Electronics 製のデバイス」を選択する
4. 基板に合うバリアント（joystick / x68k_keyboard / combined）とファームウェアバージョンを選択して「書き込み」を押す
5. 書き込み完了後、USB ケーブルを抜き差しして通常モードで起動する

> **BOOT モードの 5 秒タイムアウトについて**
>
> CH32X035 内蔵の USB DFU ブートローダーは、ホストから ISP コマンドを受信しないまま約 5 秒経過すると自動的に通常起動へ抜ける仕様です。ブラウザのページを開く前に USB を挿してしまうと、「アダプタに接続」を押す頃には既に通常モードに移行していてアダプタが見つかりません。必ず先にページを開き、USB 接続直後に「アダプタに接続」を押してください。間に合わなかった場合は USB を一度抜き、BOOT ボタンを押しながら挿し直してリトライしてください（CH32X035G8U6 には RESET 端子が出ていないため、RESET ボタンによる復帰はできません）。

> **Windows をお使いの方へ**
>
> Mimic X アダプタを BOOT モードで Chrome / Edge に認識させるには、事前に **WinUSB ドライバ**を手動でインストールしておく必要があります。手順は [WinUSB ドライバのインストール手順](https://kunichiko.ohnaka.jp/products/mimicx/install-winusb) を参照してください。macOS / Linux / ChromeOS / Android では追加のドライバインストールは不要です。

## バリアント

| バリアント | 用途 | ビルド env |
|-----------|------|-----------|
| `joystick` | ATARI / メガドライブ 6 ボタン | `pio run -e joystick` |
| `x68k_keyboard` | X68000 キーボード (キーボード + マウス) | `pio run -e x68k_keyboard` |
| `combined` | 全機能 (joystick + x68k_keyboard + x68k_mouse) を 1 MCU に同居 | `pio run -e combined` |

## 動作環境

- **ブラウザ**: Chrome, Edge（WebUSB 対応ブラウザが必要。Safari / Firefox は非対応）
- **OS**: Windows, macOS, Linux, ChromeOS, Android

## 新しいファームウェアを追加する手順

タグ push で GitHub Actions が以下をすべて自動で行うので、手動コピー /
`index.html` の編集は不要:

1. 3 env (`joystick` / `x68k_keyboard` / `combined`) を PlatformIO でビルド
2. `vX.Y.Z` の GitHub Release を作成し、3 つの `mimicx_<env>_v<X.Y.Z>.bin` を
   アセットとして添付。注釈付きタグ (`git tag -a -m "..."`) のメッセージが
   Release の body にそのまま流れる
3. 同じ bin を `docs/firmware/firmwares/` 配下に commit/push し、GitHub Pages
   に同一オリジン配備 (Web フラッシャーから CORS 制約なしで fetch できる)

リリース手順:

```sh
git checkout main && git pull

# 注釈付きタグ (-a -m) のメッセージが Release body + Web フラッシャーの
# 「リリースノート」ボックスにそのまま表示される。Markdown / 改行 OK。
git tag -a v0.8.0 -m "v0.8.0: タイトル

詳細な変更内容:
- 箇条書きはそのまま転記される
- 改行も保持される"

git push origin v0.8.0
```

Web フラッシャーのバージョン一覧 (`index.html`) はページ load 時に
GitHub Releases API を叩いて動的生成しているので、新しいタグを push すれば
特別な作業なしで反映される (GitHub Pages の再デプロイ完了後)。

軽量タグ (`-a -m` なし) を push した場合は `generate_release_notes: true` に
フォールバックして自動生成された Release body が使われる。Web フラッシャー
からも見えるので、正式リリースは注釈付きタグで打つこと推奨。

## 技術情報

- [WebUSB API](https://developer.mozilla.org/en-US/docs/Web/API/WebUSB_API) を使用して WCH ISP プロトコルで CH32X035 に書き込みます
- `ch32flasher.js` は [chprog](https://github.com/wagiminator/MCU-Flash-Tools)（MIT License）のプロトコル実装を JavaScript で再実装したものです（[MinyasX](https://github.com/kunichiko/X68k-MinyasX/) の実装を流用）
