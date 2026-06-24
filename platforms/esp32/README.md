# MimicX-ESP32 (BLE-MIDI 無線アダプタ)

ESP32-WROOM-32 を標準 **BLE-MIDI** ペリフェラルとして動かし、MimicX プロトコルを
無線で扱うためのファーム。CH32 版 (`../ch32x035/src/`, USB-MIDI) と同じプロトコル
(MimicX-protocol) を、トランスポートだけ BLE-MIDI に差し替えたもの。

ビルド系は **ESP-IDF (idf.py)**。CH32 版の PlatformIO ビルドとは独立しており、
互いに影響しない。

## 現在のステータス: 疎通スケルトン

最初のマイルストーンとして、**iOS / macOS アプリが BLE 経由で接続 → IDENTIFY →
HEART_BEAT まで完走できること**だけを実装している。

- ✅ 標準 BLE-MIDI GATT サービスを広告
  (service `03B80E5A-…` / characteristic `7772E5DB-…`)
- ✅ `IDENTIFY_REQUEST` → `IDENTIFY_RESPONSE` (CH32 とバイト互換、joystick/ATARI を 1ch 申告)
- ✅ `HEART_BEAT` / `DISCONNECT` / `SET_*` / `RESET` → `ACK`
- ✅ SysEx の BLE-MIDI パケット分割・再結合
- ❌ デバイス制御 (joystick GPIO / x68k UART / LED) は **未実装** (Note/CC は無視)

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
