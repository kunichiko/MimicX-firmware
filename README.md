# MimicX-firmware

**製品名: Mimic X**

スマートフォンから MIDI 経由で受信した指令をもとに、レトロPCのHIDデバイス（キーボード・マウス・ジョイスティック）を模倣するマイコンファームウェア。

「Mimic X」は様々な HID デバイスに変身する (Mimic = 模倣する) ことから命名。

## ターゲット

将来いろいろなマイコンに展開できるよう、ファーム実体は `platforms/` 配下に
マイコン別フォルダで分割し、MCU 非依存の共有コードを `platforms/common/` にまとめている。

| ターゲット | ディレクトリ | トランスポート | ビルド系 |
|---|---|---|---|
| CH32X035 (有線) | [`platforms/ch32x035/`](platforms/ch32x035/) | USB-MIDI | PlatformIO (`pio run -e ...`) |
| ESP32-WROOM-32 (無線) | [`platforms/esp32/`](platforms/esp32/) | BLE-MIDI | ESP-IDF (`idf.py`) |
| Seeed XIAO ESP32-C6 (無線) | [`platforms/esp32c6/`](platforms/esp32c6/) | BLE-MIDI | ESP-IDF (`idf.py`, RISC-V) |
| 共有コード (MCU 非依存) | [`platforms/common/`](platforms/common/) | — | 各ビルドから参照 |

`platforms/common/mimicx_hid/` に `hid_function.h` (vtable インターフェース) と
`hid_dispatcher.c/.h` が入る。ESP32 版は現在 BLE 疎通スケルトンの段階。詳細は
[`platforms/esp32/README.md`](platforms/esp32/README.md) を参照。

## 対応デバイス

- ATARI仕様ジョイスティック
- メガドライブ 6 ボタンファイティングパッド
- X68000キーボード
- （今後追加予定）

## ビルド方法

[PlatformIO](https://platformio.org/) ベースのプロジェクト。事前に PlatformIO
Core を導入してください (`pip install platformio` / `brew install platformio`
/ `uv tool install platformio` / VS Code の "PlatformIO IDE" 拡張のいずれか)。

CH32 版の PlatformIO プロジェクトは `platforms/ch32x035/` にある。`pio` はその
ディレクトリで実行する (または `pio run -d platforms/ch32x035 ...`)。
`platforms/ch32x035/platformio.ini` にボード別の env が定義されています:

| env | 用途 |
|---|---|
| `joystick` | ATARI / MD6 ジョイスティック (atari-joystick 基板) |
| `x68k_keyboard` | X68000 キーボード基板 (本体側へキーボード + マウスポート対応) |
| `combined` | 全機能 (joystick + x68k_keyboard + x68k_mouse) を 1 個の MCU に同居 |
| `joystick-debug` | デバッグビルド (-Og + WCH-LinkE GDB サーバ) |

### ビルド

```sh
cd platforms/ch32x035
pio run -e joystick           # ATARI/MD6 ジョイスティック
pio run -e x68k_keyboard      # X68000 キーボード (キーボード + マウス)
pio run -e combined           # 全機能同居版
```

初回は Community-PIO-CH32V プラットフォーム / ch32v003fun フレームワーク /
riscv toolchain が自動取得されます (数分かかります)。成果物は
`platforms/ch32x035/.pio/build/<env>/firmware.{bin,hex,elf}` に出力されます。

### 書き込み

**A. WCH-LinkE (SWD 経由)**

```sh
cd platforms/ch32x035
pio run -e joystick -t upload   # platformio.ini の upload_protocol=minichlink
```

**B. USB DFU bootloader 経由**

リセット時に UDP (PC17) を HIGH にすると WCH USB DFU bootloader で起動します
(atari-joystick 基板の **BOOT ボタン**を押しながら USB を挿す)。書き込みは
`wchisp` または下記 WebUSB フラッシャから:

```sh
wchisp flash platforms/ch32x035/.pio/build/joystick/firmware.bin
# または: tools/wchisp_flash.sh joystick
```

### デバッグ

```sh
cd platforms/ch32x035
pio debug -e joystick-debug
```

WCH-LinkE 接続時に minichlink GDB サーバ経由でブレーク・ステップ実行できます。

### クリーンビルド

```sh
cd platforms/ch32x035
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

# 必要なら platforms/ch32x035/src/main.c の FW_VERSION_* (MAJOR / MINOR / PATCH) も更新して commit
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
