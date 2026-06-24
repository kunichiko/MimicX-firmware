# MimicX-ESP32 (BLE-MIDI 無線アダプタ)

ESP32-WROOM-32 を標準 **BLE-MIDI** ペリフェラルとして動かし、MimicX プロトコルを
無線で扱うためのファーム。CH32 版 (`../src/`, USB-MIDI) と同じプロトコル
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

## ビルド & 書き込み

ESP-IDF v5.x がセットアップ済み (`. $IDF_PATH/export.sh`) であること。

```sh
cd MimicX-firmware/esp32

# ターゲットを esp32 に設定 (初回のみ)
idf.py set-target esp32

# ビルド → 書き込み → ログ表示 (PORT は環境に合わせて)
idf.py -p /dev/tty.usbserial-XXXX flash monitor
```

ESP32-WROOM-32 開発ボードに焼けば、基板が無くても疎通確認できる。

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
| `main/mimicx_proto.c` | SysEx 処理 (IDENTIFY / ACK)。CH32 `../src/main.c` とバイト互換 |

## 今後 (HAL 分割でのコード共有)

デバイス制御の本実装に進む際は、CH32 と共有する `functions/*` (joystick / x68k) を
プラットフォーム HAL (GPIO / UART / LED / transport) の上に載せ替え、両ファームで
単一ソース化する方針。詳細は workspace の方針メモを参照。
