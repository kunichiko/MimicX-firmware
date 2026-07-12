# MimicX-ESP32 (BLE-MIDI 無線アダプタ)

ESP32-WROOM-32 を標準 **BLE-MIDI** ペリフェラルとして動かし、MimicX プロトコルを
無線で扱うためのファーム。CH32 版 (`../ch32x035/src/`, USB-MIDI) と同じプロトコル
(MimicX-protocol) を、トランスポートだけ BLE-MIDI に差し替えたもの。

ビルド系は **ESP-IDF (idf.py)**。CH32 版の PlatformIO ビルドとは独立しており、
互いに影響しない。

## 現在のステータス: ATARI ジョイスティック動作

iOS / macOS / Android アプリから BLE 経由で接続でき、**ATARI ジョイスティックの
GPIO 出力まで動作する**。

- ✅ 標準 BLE-MIDI GATT サービスを広告
  (service `03B80E5A-…` / characteristic `7772E5DB-…`)
- ✅ `IDENTIFY_REQUEST` → `IDENTIFY_RESPONSE` (CH32 とバイト互換、joystick/ATARI を 1ch 申告)
- ✅ `HEART_BEAT` / `DISCONNECT` / `SET_*` / `RESET` → `ACK`
- ✅ SysEx の BLE-MIDI パケット分割・再結合、チャンネルメッセージ (Note/CC) パース
- ✅ **ATARI ジョイスティック**: Note On/Off (ch0) → GPIO active-low/open-drain 出力
  (UP=25 DOWN=26 LEFT=27 RIGHT=14 A=13 B=23、暫定割り当て・実機検証済み)
- ❌ MD6 / MSX マウスモード (CH32 の DMA/EXTI 依存)、x68k キーボード/マウス、LED は **未実装**

⚠ ESP32 GPIO は 3.3V 非 5V トレラント。実機 (5V プルアップ) 直結は不可、レベルシフト
前提 (基板で対応)。動作確認は 3.3V 範囲 (LED / ロジアナ) で行う。

serial は ESP32 の base MAC を 16 桁 hex 化したもの (`0000` + MAC 12 桁)。
プロトコルバージョンは **0.8** を申告する (アプリの `knownLatest` が 0.7 のままだと
「アプリより新しいデバイス」の警告が出るが、接続自体は可能)。

## 環境構築 (macOS)

ビルド系は ESP-IDF (idf.py)。下記は macOS (Apple Silicon) での **検証済み手順**
(2026-06 時点、ESP-IDF v5.3.5)。

ESP-IDF は Espressif の **EIM (ESP-IDF Installation Manager)** で導入するのが楽だが、
macOS では **cmake / ninja / Python (3.10–3.13) は EIM が入れてくれない**ため、先に
Homebrew で用意する (システム Python が 3.10 未満だと EIM が弾く)。

```sh
# 1) 前提パッケージ (Homebrew)
brew install cmake ninja dfu-util
brew install python@3.12            # システム python が 3.10 未満なら必須

# 2) EIM を導入
brew tap espressif/eim
brew trust espressif/eim            # 新しい brew は tap の信頼登録が必要
brew install eim

# 3) ESP-IDF v5.3.x を非対話インストール (Python 3.12 を使わせる)
PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH" \
  eim install -n true -i v5.3.5 -t esp32

# 完了時に表示される activation script のパスを控える
#   例: ~/.espressif/tools/activate_idf_v5.3.5.sh
```

> 公式の git clone + `install.sh` 方式でも可。その場合も macOS では cmake/ninja を
> Homebrew で入れる必要がある (ESP-IDF は macOS 用にこれらを同梱しない)。

## 内包 CH32 イメージの用意 (ビルド前に必須)

ESP32 ファームは CH32 の i2c 版ファームを 1 つ内包し (variant により切替)、起動時に
接続中の CH32 と版が違えば SWD で書き込む。この内包 bin は **CH32 ソースから生成する
ビルド成果物**で、git では追跡しない (`main/.gitignore`)。CI はリリース毎に自動生成
するが、**ローカルビルドでは事前に手動生成が必要**:

```sh
# variant に対応する i2c 版を PlatformIO でビルドし、main/ へコピーする。
#   joystick → ch32_joy_i2c.bin / keyboard → ch32_kbd_i2c.bin / combined → ch32_combo_i2c.bin
pio run -d ../ch32x035 -e joystick-i2c
cp ../ch32x035/.pio/build/joystick-i2c/firmware.bin main/ch32_joy_i2c.bin
```

未生成のまま build すると configure 時に EMBED_FILES で失敗する。版数は CMake が
`../ch32x035/src/main.c` の `FW_VERSION_*` から自動注入するため、bin を作り直したら
CH32 ソースの版数と食い違わないよう **同じソースから生成する**こと。

## ビルド & 書き込み

```sh
# 環境を有効化 (ターミナルごとに必要)
source ~/.espressif/tools/activate_idf_v5.3.5.sh

# ターゲット設定 (初回のみ) → ビルド
idf.py -C MimicX-firmware/platforms/esp32 set-target esp32
idf.py -C MimicX-firmware/platforms/esp32 build

# 書き込み + ログ表示 (PORT は環境に合わせて。例: /dev/cu.usbserial-XXXX)
idf.py -C MimicX-firmware/platforms/esp32 -p /dev/cu.usbserial-XXXX flash monitor
# monitor 終了は Ctrl+]
```

ESP32-WROOM-32 開発ボードに焼けば、基板が無くても疎通確認できる。起動ログに
`advertising as "MimicX"` が出れば BLE 広告開始 (第一段階 OK)。

## iOS / macOS での確認手順

1. アプリ側に BLE central 起動 + スキャン (`startBluetoothCentral()` /
   `startScanningForBluetoothDevices()`) を入れる ※app 側改修は別ステップ
2. デバイス一覧に `MimicX` (BLE) が出る
3. 接続 → IDENTIFY で `MimicX-ESP32` / joystick(ATARI) として認識される
4. ジョイスティック画面に入れる (操作は効かない = 制御未実装のため正常)
5. HEART_BEAT が継続し、切断するまで接続が維持される

## 構成

| ファイル | 役割 |
|---|---|
| `main/main.c` | NVS / NimBLE host 起動、GAP アドバタイズ |
| `main/ble_midi.c` | BLE-MIDI GATT サービス、パケット復元 (§2.3)・分割送信 |
| `main/mimicx_proto.c` | SysEx 処理 (IDENTIFY / ACK)。CH32 `../ch32x035/src/main.c` とバイト互換 |

## 今後 (HAL 分割でのコード共有)

デバイス制御の本実装に進む際は、CH32 と共有する `functions/*` (joystick / x68k) を
プラットフォーム HAL (GPIO / UART / LED / transport) の上に載せ替え、両ファームで
単一ソース化する方針。詳細は workspace の方針メモを参照。
