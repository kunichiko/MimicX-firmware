# MimicX-firmware

**製品名: Mimic X**

スマートフォンからUSB-MIDI経由で受信した指令をもとに、レトロPCのHIDデバイス（キーボード・マウス・ジョイスティック）を模倣するマイコンファームウェア。

「Mimic X」は様々な HID デバイスに変身する (Mimic = 模倣する) ことから命名。

## 対応デバイス

- ATARI仕様ジョイスティック
- メガドライブ 6 ボタンファイティングパッド
- X68000キーボード
- （今後追加予定）

## ビルド方法

[PlatformIO](https://platformio.org/) ベースのプロジェクト。事前に PlatformIO
Core を導入してください (`pip install platformio` / `brew install platformio`
/ `uv tool install platformio` / VS Code の "PlatformIO IDE" 拡張のいずれか)。

`platformio.ini` にボード別の env が定義されています:

| env | 用途 |
|---|---|
| `joystick` | ATARI / MD6 ジョイスティック (atari-joystick 基板) |
| `x68k_keyboard` | X68000 キーボード基板 (本体側へキーボード + マウスポート対応) |
| `combined` | 全機能 (joystick + x68k_keyboard + x68k_mouse) を 1 個の MCU に同居 |
| `joystick-debug` | デバッグビルド (-Og + WCH-LinkE GDB サーバ) |

### ビルド

```sh
pio run -e joystick           # ATARI/MD6 ジョイスティック
pio run -e x68k_keyboard      # X68000 キーボード (キーボード + マウス)
pio run -e combined           # 全機能同居版
```

初回は Community-PIO-CH32V プラットフォーム / ch32v003fun フレームワーク /
riscv toolchain が自動取得されます (数分かかります)。成果物は
`.pio/build/<env>/firmware.{bin,hex,elf}` に出力されます。

### 書き込み

**A. WCH-LinkE (SWD 経由)**

```sh
pio run -e joystick -t upload   # platformio.ini の upload_protocol=minichlink
```

**B. USB DFU bootloader 経由**

リセット時に UDP (PC17) を HIGH にすると WCH USB DFU bootloader で起動します
(atari-joystick 基板の **BOOT ボタン**を押しながら USB を挿す)。書き込みは
`wchisp` または下記 WebUSB フラッシャから:

```sh
wchisp flash .pio/build/joystick/firmware.bin
```

### デバッグ

```sh
pio debug -e joystick-debug
```

WCH-LinkE 接続時に minichlink GDB サーバ経由でブレーク・ステップ実行できます。

### クリーンビルド

```sh
pio run -e joystick -t clean   # env 単位
rm -rf .pio                    # 完全クリア (プラットフォーム再取得)
```

## リリース手順

タグ push (`v*.*.*`) を起点に GitHub Actions の `Firmware Release` ワークフロー
(`.github/workflows/firmware-release.yml`) が以下を自動実行する:

| ジョブ | 役割 |
|---|---|
| `Build firmware` | matrix で 3 env (`joystick` / `x68k_keyboard` / `combined`) を並列ビルド。`mimicx_<env>_v<X.Y.Z>.bin` を artifact 出力 |
| `Create GitHub Release` | `vX.Y.Z` の GitHub Release を作成し 3 つの bin を添付。注釈付きタグ (`git tag -a -m "..."`) のメッセージをそのまま Release body として使う (注釈なしなら自動生成にフォールバック) |
| `Deploy bins to GitHub Pages` | 同じ 3 つの bin を `docs/firmware/firmwares/` に commit/push。Web フラッシャー (GitHub Pages) から同一オリジンで fetch できるようにする |

実際のリリース作業:

```sh
git checkout main && git pull

# 必要なら src/main.c の FW_VERSION_* (MAJOR / MINOR / PATCH) も更新して commit
# (Web フラッシャーには現れないが、ホスト app の IDENTIFY 応答で表示される値)

# 注釈付きタグを打つ。-m のメッセージが Release body と Web フラッシャーの
# 「リリースノート」ボックスにそのまま表示される (Markdown / 改行 OK)
git tag -a v0.8.0 -m "v0.8.0: タイトル

変更内容を Markdown で記述する。
- 箇条書きはそのまま転記される"

git push origin v0.8.0
```

Web フラッシャー (`docs/firmware/`) は GitHub Releases API で版とノートを動的取得
するので、新タグを push すれば手動編集なしで反映される (GitHub Pages の再デプロイ
完了後)。pull request / `workflow_dispatch` ではビルド検証のみで Release は作成
されない (上記 2 ジョブ目以降がスキップされる)。

## 関連リポジトリ

- [MimicX-protocol](https://github.com/kunichiko/MimicX-protocol) - MIDI通信プロトコルライブラリ
- [MimicX-app](https://github.com/kunichiko/MimicX-app) - Flutterアプリ
- [MimicX-hardware](https://github.com/kunichiko/MimicX-hardware) - 基板設計データ

## ファームウェア更新

CH32X035 のファームウェアは Web ブラウザ (Chrome / Edge) から更新できます:

<https://kunichiko.github.io/MimicX-firmware/firmware/>

詳細: [docs/firmware/README.md](docs/firmware/README.md)
