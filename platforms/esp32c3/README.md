# MimicX-ESP32-C3 (Seeed XIAO ESP32-C3, BLE-MIDI 無線アダプタ)

`../esp32/` (ESP32-WROOM-32) / `../esp32c6/` (XIAO C6) と **同じソース** (`../esp32/main/`)
を、Seeed Studio **XIAO ESP32-C3** (RISC-V) 向けにビルドするプロジェクト。C3 も C6 と
同じ RISC-V なので、SWD ビットバングの RISC-V 実装 (`bitbang_rvswdio.h` / `ch32_swd.c`)
をそのまま共有する。**差分はピン配置のみ**:

- ピン配置 → `../esp32/main/board_config.h` がターゲット (`CONFIG_IDF_TARGET_*`) で切替
- ビルド設定 → この `sdkconfig.defaults` (シングルコア / USB Serial-JTAG コンソール)
- アンテナ: XIAO ESP32-C3 は **U.FL 外付けのみ (オンボード無し / RF スイッチ無し)** なので
  C6 のようなアンテナ初期化は不要 (`board_config.h` で `BOARD_HAS_ANTENNA_SWITCH` を定義しない)

全体像は `../esp32/README.md` / `../esp32c6/README.md` を参照。

## ピン配置 (XIAO ESP32-C3)

| 機能 | XIAO パッド | GPIO | 備考 |
|---|---|---|---|
| I2C SDA / SWDIO | D4 | 6 | CH32 I2C-SDA / SWD SDIO (PC18) 兼用 |
| I2C SCL / SWCLK | D5 | 7 | CH32 I2C-SCL / SWD SWCLK (PC19) 兼用 |
| INT (CH32→C3) | D3 | 5 | CH32 PB1 (active-low / open-drain) |

> ⚠ アンテナ (U.FL) を必ず接続すること。未接続だと BLE がほとんど飛ばない。

## CH32 との配線 (2チップ構成)

C6 版と同じ考え方 (信号 2 本 + INT + 電源、SWD と I2C は同一線をモード切替)。

```
  XIAO ESP32-C3                         CH32X035
  ------------------                    -----------------------------
  D4 / GPIO6  (SDA・SWDIO) ──┬───────── PC18  (pin25, SDA / SDI-DIO)
                            └[4.7kΩ]─ 3V3
  D5 / GPIO7  (SCL・SWCLK) ──┬───────── PC19  (pin24, SCL / SDI-DCK)
                            └[4.7kΩ]─ 3V3
  D3 / GPIO5  (INT 入力)   ──────────── PB1   (INT, open-drain active-low)
  GND                      ──────────── GND
  3V3                      ──────────── VCC   (3.3V)
```

- SDA/SCL には 3.3V へのプルアップ (4.7kΩ 目安) が必要。
- 全て 3.3V ロジック (C3 は 5V 非トレラント)。

## 環境構築 (macOS)

C6 と同じく RISC-V ツールチェーンが要る。EIM を `-t esp32` で入れた場合は追加:

```sh
source ~/.espressif/tools/activate_idf_v5.3.5.sh
python "$IDF_PATH/tools/idf_tools.py" install --targets=esp32c3
export PATH="$HOME/.espressif/tools/tools/riscv32-esp-elf/esp-13.2.0_20250707/riscv32-esp-elf/bin:$PATH"
```

## ビルド & 書き込み

XIAO ESP32-C3 は USB-C を SoC の USB Serial/JTAG に直結 (ポートは `/dev/cu.usbmodemXXXX`)。

```sh
idf.py -C MimicX-firmware/platforms/esp32c3 set-target esp32c3
idf.py -C MimicX-firmware/platforms/esp32c3 build
idf.py -C MimicX-firmware/platforms/esp32c3 -p /dev/cu.usbmodemXXXX flash monitor
```

起動ログに `advertising as "MimicX"` が出れば BLE 広告開始。
