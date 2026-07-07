# MimicX-ESP32-C6 (Seeed XIAO ESP32-C6, BLE-MIDI 無線アダプタ)

`../esp32/` (ESP32-WROOM-32 DevKit) と **同じソース** (`../esp32/main/`) を、
Seeed Studio **XIAO ESP32-C6** (RISC-V) 向けにビルドするプロジェクト。ESP32 と
C6 は同一バイナリでは動かないためプロジェクトを分離しているが、**差分は最小限**:

- ピン配置 → `../esp32/main/board_config.h` がターゲット (`CONFIG_IDF_TARGET_*`) で切替
- ビルド設定 → この `sdkconfig.defaults` (シングルコア / USB Serial-JTAG コンソール等)
- アンテナ RF スイッチ初期化 → `main.c` の `board_early_init()` (C6 のみ)
- SWD ビットバング (CH32 OTA) → `bitbang_rvswdio.h` / `ch32_swd.c` を RISC-V 対応化
  (Xtensa 版は割り込みレベル + GPIO 構造体直叩き、C6 版は portMUX + `REG_WRITE`)

Dart/アプリや BLE-MIDI プロトコルの挙動は ESP32 版と同一。全体像は `../esp32/README.md` を参照。

## ピン配置 (XIAO ESP32-C6)

| 機能 | XIAO パッド | GPIO | 備考 |
|---|---|---|---|
| I2C SDA / SWDIO | D4 | 22 | CH32 I2C-SDA / SWD SDIO (PC18) 兼用 |
| I2C SCL / SWCLK | D5 | 23 | CH32 I2C-SCL / SWD SWCLK (PC19) 兼用 |
| INT (CH32→C6) | D3 | 21 | CH32 PB1 (active-low / open-drain) |
| アンテナ RF 給電 | — | 3 | 起動時に Low 出力 (RF スイッチに給電) |
| アンテナ選択 | — | 14 | Low=オンボード / High=外部(U.FL) |

> ⚠ XIAO ESP32-C6 は GPIO3=Low + GPIO14=Low を出さないとオンボードアンテナの
> RF スイッチが有効にならず BLE が極端に弱くなる。ファームが起動時 (無線 init 前) に
> 自動設定する。外部アンテナ(U.FL)を使う場合は `board_config.h` の `BOARD_ANT_SELECT_GPIO`
> を High にするよう変更する。

## CH32 との配線 (2チップ構成)

CH32X035 側は **I2C (SDA=PC18/SCL=PC19) と 2 線式 SWD (SDI: DIO=PC18/DCK=PC19) が
同一ピン**で、ファームがモード切替する (通常は I2C、OTA 更新時のみ SWD)。よって
XIAO とは **信号 2 本 + INT + 電源** だけで接続できる (ESP32 DevKit 版と同一設計)。

```
  XIAO ESP32-C6                         CH32X035
  ------------------                    -----------------------------
  D4 / GPIO22  (SDA・SWDIO) ──┬──────── PC18  (pin25, SDA / SDI-DIO)
                             └[4.7kΩ]─ 3V3
  D5 / GPIO23  (SCL・SWCLK) ──┬──────── PC19  (pin24, SCL / SDI-DCK)
                             └[4.7kΩ]─ 3V3
  D3 / GPIO21  (INT 入力)   ─────────── PB1   (INT, open-drain active-low)
  GND                       ─────────── GND
  3V3                       ─────────── VCC   (3.3V)
```

| XIAO パッド | GPIO | CH32 ピン | 信号 | 備考 |
|---|---|---|---|---|
| D4 | 22 | PC18 (pin25) | I2C SDA / SWD DIO | 4.7kΩ で 3V3 へプルアップ |
| D5 | 23 | PC19 (pin24) | I2C SCL / SWD DCK | 4.7kΩ で 3V3 へプルアップ |
| D3 | 21 | PB1 | INT (CH32→C6) | CH32 はオープンドレイン。C6 側は内蔵プルアップ有効なので外付け不要 |
| GND | — | GND | GND | **共通必須** |
| 3V3 | — | VCC | 電源 | 3.3V。消費に余裕があれば XIAO の 3V3 から供給可。別電源でも可 (GND 共通は必須) |

補足:
- **SDA/SCL には外付けプルアップ (3.3V へ 4.7kΩ 前後) が必要**。C6 側は内蔵プルアップも
  併用するが、I2C 400kHz と SWD ビットバングの両方で外付けを推奨。
- **全て 3.3V ロジック**。XIAO ESP32-C6 の GPIO は 5V トレラントではないので、実機
  (ATARI 5V プルアップ等) を直結せず、CH32 との間は 3.3V に揃える (実機側は基板で
  レベルシフト前提)。
- CH32 が MimicX ファーム稼働中なら C6 起動時に I2C で IDENTIFY を取得し、内蔵イメージが
  新しければ `ENTER_SWD` (0x7E) を送って CH32 の SDI を再有効化 → SWD で自動 OTA する。
  未書込み(空チップ)の CH32 は SDI が既定で有効なので、いきなり SWD 直書きされる。
- 接続後にアプリが「MimicX 対応」表示になり、IDENTIFY がジョイスティック等を申告すれば
  I2C 中継まで成功。OTA (SWD 書込) が通らない場合は `ch32_swd.c` の `t1coeff` スイープ
  を実測に合わせて調整する。

## 環境構築 (macOS)

ESP-IDF v5.3.5 は `../esp32/README.md` の手順で導入済みとする。ただし EIM を
`-t esp32` で入れた場合 **RISC-V ツールチェーンが入っていない**ので追加する:

```sh
source ~/.espressif/tools/activate_idf_v5.3.5.sh
python "$IDF_PATH/tools/idf_tools.py" install --targets=esp32c6
```

EIM 生成の `activate_idf_*.sh` は Xtensa だけを PATH に通すため、C6 ビルド時は
riscv32-esp-elf を PATH に足す (バージョン部は環境に合わせる):

```sh
export PATH="$HOME/.espressif/tools/tools/riscv32-esp-elf/esp-13.2.0_20250707/riscv32-esp-elf/bin:$PATH"
```

## ビルド & 書き込み

XIAO ESP32-C6 は USB-C を SoC の USB Serial/JTAG に直結しており、書き込み・ログとも
同じケーブルで行える (ポートは `/dev/cu.usbmodemXXXX`)。

```sh
source ~/.espressif/tools/activate_idf_v5.3.5.sh
export PATH="$HOME/.espressif/tools/tools/riscv32-esp-elf/esp-13.2.0_20250707/riscv32-esp-elf/bin:$PATH"

# ターゲット設定 (初回のみ) → ビルド
idf.py -C MimicX-firmware/platforms/esp32c6 set-target esp32c6
idf.py -C MimicX-firmware/platforms/esp32c6 build

# 書き込み + ログ (PORT は /dev/cu.usbmodemXXXX)
idf.py -C MimicX-firmware/platforms/esp32c6 -p /dev/cu.usbmodemXXXX flash monitor
# monitor 終了は Ctrl+]
```

起動ログに `advertising as "MimicX"` が出れば BLE 広告開始 (第一段階 OK)。CH32 を
繋がない素の XIAO でも BLE の疎通確認まで可能 (CH32 OTA/I2C は失敗ログを出すが継続する)。

## 動作状況 (2026-07-07, 実機 XIAO ESP32-C6 + CH32X035 で確認)

- ✅ BLE-MIDI 広告 / 接続 / IDENTIFY (ESP32 版とバイト互換)。MimicX アプリから接続し
  ジョイスティック(ATARI)として認識・操作まで確認。
- ✅ オンボードアンテナ自動選択、シングルコア動作、USB Serial/JTAG ログ
- ✅ **CH32 との I2C 中継**: 起動時 probe で `CH32 device engine found at 0x33`、
  IDENTIFY / Note を I2C 経由で中継。
- ✅ **RISC-V 版 SWD ビットバングによる CH32 自動 OTA**: 空チップ検出時に SWD 直書き
  (`chip=0x0d` / 13608B / `verify OK`)、バージョン一致時はスキップ。`t1coeff` スイープ
  `{10,5,20,40}` の既定値で実機書込に成功。
- ⚠ I2C 配線 (SDA/SCL) が緩いと起動時 probe が失敗し、空チップとみなして毎回 SWD 再書込
  (BLE 広告まで ~5 秒) になる。プルアップ (3.3V/4.7kΩ) と結線を確実に。
