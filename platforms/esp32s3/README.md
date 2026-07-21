# MimicX-ESP32-S3 (Seeed XIAO ESP32-S3, BLE-MIDI + USB-MIDI 無線/有線アダプタ)

`../esp32/main/` を共有する S3 向けプロジェクト (分離方針は `../esp32c6/README.md` と同じ)。
**S3 だけの追加機能: USB-OTG による USB-MIDI デバイス化** (`usb_midi_bridge.c`)。
XIAO の USB-C 1 ポートで「普通の有線 USB MimicX アダプタ」としても動く。

```
  phone --BLE-MIDI--┐
                    ├--> ESP32-S3 --I2C(0x33)--> CH32 --GPIO--> ターゲット機
  PC ---USB-MIDI----┘
```

- C3/C6 では不可 (USB は固定機能の USB Serial/JTAG のみ)。S3 は USB-OTG を持つ
- BLE と USB は併走。device→host 通知は接続中の全トランスポートへ送る
- BRIDGE_IDENTIFY (§6.4.5) は要求が届いた経路に transport (0x00=USB / 0x01=BLE) を
  申告して自答する
- CH32 の解放 (DISCONNECT+RESET) は「最後のホストが居なくなったとき」のみ

## XIAO ESP32-C6 との差分

| 項目 | C6 | S3 |
|---|---|---|
| I2C SDA / SWDIO | GPIO22 (D4) | **GPIO5 (D4)** |
| I2C SCL / SWCLK | GPIO23 (D5) | **GPIO6 (D5)** |
| INT | GPIO21 (D3) | **GPIO4 (D3)** |
| アンテナ | RF スイッチ初期化必要 | オンボード直結 (初期化不要) |
| USB | Serial/JTAG (ログ専用) | **OTG → USB-MIDI (TinyUSB)** |
| ログ/コンソール | USB Serial/JTAG | **UART0 (D6=TX/GPIO43, D7=RX/GPIO44)** |
| SWD ビットバング | portMUX + REG_WRITE | 同左 (Xtensa でも portMUX/REG_WRITE を使用) |

**XIAO パッド互換**: D3/D4/D5 の物理位置は C6 と同一なので、wireless-combo 基板は
無改造で C6 ↔ S3 を差し替えられる (GPIO 番号差は `board_config.h` が吸収)。

## ⚠ ログについて (USB は TinyUSB が占有)

S3 の USB-OTG と USB Serial/JTAG は同じ USB ピン (GPIO19/20) を共有するため、
USB-MIDI 動作中は USB 経由のログが使えない。ログ/コンソールは UART0 (D6/D7) に
出るので、観測には USB-UART アダプタが必要。書き込みは BOOT ボタンで ROM
ダウンロードモード (USB) に入れば同じ USB-C ケーブルで可能。

## 依存コンポーネント

`espressif/esp_tinyusb` (IDF Component Manager が初回ビルドで自動取得、要ネットワーク)。
`../esp32/main/idf_component.yml` の rules により **S3 ビルドのみ**取得され、
ESP32/C3/C6 のビルドには影響しない (`Skipping optional dependency` と表示される)。
USB-MIDI クラスは `CONFIG_TINYUSB_MIDI_COUNT=1` (sdkconfig.defaults) で有効化。

## ビルド & 書き込み

```sh
source ~/.espressif/tools/activate_idf_v5.3.5.sh
# S3 は Xtensa (xtensa-esp-elf は EIM の -t esp32 導入で既に入っている)

idf.py -C MimicX-firmware/platforms/esp32s3 set-target esp32s3   # 初回のみ
idf.py -C MimicX-firmware/platforms/esp32s3 build

# 書き込み: BOOT を押しながら USB 接続 → ROM ダウンロードモード
idf.py -C MimicX-firmware/platforms/esp32s3 -p /dev/cu.usbmodemXXXX flash
```

内包 CH32 イメージの variant 切替は他プロジェクトと同じ
(`idf.py -DMIMICX_CH32_VARIANT=combined build` 等)。

## 状態 (2026-07-21)

- ✅ esp32s3 ビルド成功 (esp32 / esp32c3 / esp32c6 のリグレッションなしも確認)
- ✅ USB-MIDI enumerate (macOS): "Mimic X (Bridge)" / iSerial `mimicx-bridge-<MAC>` を確認
- ✅ BRIDGE_IDENTIFY 自答 (USB 経路): proto/fw/transport=0x00/serial/name とも仕様どおり
- ✅ CH32 中継 (USB → I2C): IDENTIFY (0x01) が CH32 まで届き応答が返る (実機 joy 基板)
- ✅ アプリ (MimicX-app, macOS) が本ブリッジを MimicX として認識・接続できることを確認
- ⬜ Windows / Android での USB enumerate + アプリ接続
- ⬜ BLE と USB の同時接続時の挙動 (アービトレーションポリシーは現状「両方許可」)
- ⬜ SWD ビットバング (CH32 自動 OTA) の t1coeff が S3 の実機で合うか

### ⚠ flash 直後は USB-MIDI が現れない (実機で確認済みの挙動)

`idf.py flash` (USB Serial/JTAG 経由) の完了後、esptool の hard reset では ROM
ダウンロードモードから抜けきらず、"USB JTAG_serial debug unit" のまま残ることがある。
**もう一度リセット** (RESET ボタン、または JTAG ポートを開いて RTS パルス) すると
アプリが起動して "Mimic X (Bridge)" に切り替わる。
