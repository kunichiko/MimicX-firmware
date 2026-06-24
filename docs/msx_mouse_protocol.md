# MSX マウスプロトコル 実装メモ

MimicX で MSX マウス (joystick 端子 D-SUB 9pin 経由) をエミュレートする際に
得られた知見をまとめる。実機 (Panasonic FS-A1GT + MSX View) とオシロでの
信号観測を経て確定した動作仕様。

公開資料が断片的でコミュニティ wiki にも記載のない挙動 (特に MSX BIOS の
**バイト 2 の補数否定 (NEG)**) を含むため、実装者向けの記録として残す。

---

## 1. 物理層

### 1.1 D-SUB 9pin (joystick port) ピンアサイン

MSX マウスは同じ joystick port を使用する。MimicX では `joystick` 基板 /
`combined` 基板の D-SUB 9pin にそのまま結線される。

| D-SUB pin | 信号 | CH32X035 ピン | 方向 | 用途 (MSX マウスモード) |
|---|---|---|---|---|
| 1 | D0 | PA7 | OD 出力 | データニブル bit 0 |
| 2 | D1 | PA5 | OD 出力 | データニブル bit 1 |
| 3 | D2 | PA3 | OD 出力 | データニブル bit 2 |
| 4 | D3 | PA2 | OD 出力 | データニブル bit 3 |
| 5 | +5V | - | - | 給電 |
| 6 | TL (TRIG-A) | PA6 | OD 出力 | 左マウスボタン (active-low) |
| 7 | TR (TRIG-B) | PA4 | OD 出力 | 右マウスボタン (active-low) |
| 8 | TH (STROBE) | PA0 | 入力 | MSX 側クロック (ニブル切替トリガ) |
| 9 | GND | - | - | グランド |

- データ出力ピン (D0-D3, TL, TR) はすべて **オープンドレイン**
- MSX 側に pullup 抵抗があるため、Hi-Z 時は電気的に HIGH に固定される
- "Drive LOW" は明示的に MOSFET で GND に引き込むことを指す

### 1.2 STROBE (TH, pin 8) の挙動

MSX BIOS は PSG レジスタ 15 の bit 6 を書き換えることで STROBE を制御する。
読み取りは PSG レジスタ 14 から行う。

---

## 2. 観測されたサイクル構造

### 2.1 MSX View での実測波形 (Panasonic FS-A1GT)

`MSX View` 経由でマウス読み取りを発生させたときの TH (pin 8) のシーケンス:

```
[idle] L
       ↓ rising
       H (~150 us)   ← nibble 1 (byte 1 high = X high)
       ↓ falling
       L (~120 us)   ← nibble 2 (byte 1 low  = X low)
       ↓ rising
       H (~120 us)   ← nibble 3 (byte 2 high = Y high)
       ↓ falling
       L (~120 us)   ← nibble 4 (byte 2 low  = Y low)
       ↓ rising
       H (~200 us)   ← nibble 5 (byte 3 high)
       ↓ falling
       L (~50  us)   ← nibble 6 (byte 3 low)
       ↓ rising
       H (~70  us)   ← nibble 7 (byte 4 high)
       ↓ falling
[idle] L              ← nibble 8 (byte 4 low) + 次サイクルまで idle
```

- 1 サイクル = **8 ニブル = 4 バイト**
- 4 byte (X, Y, byte 3, byte 4) 構成。byte 3/4 の用途は時代と driver で異なる
  (後述)
- 4 fallings + 4 risings = 8 edges
- サイクル長合計 ~830 us
- **サイクル間隔 ~16 ms (= 60 Hz VSYNC 同期)**

### 2.2 ニブル順序 (byte 1 を例に)

```
STROBE = H → mouse drives D0-D3 で byte の上位 4 bit (high nibble)
STROBE = L → mouse drives D0-D3 で byte の下位 4 bit (low nibble)
```

つまり 1 byte = 「H state での読み + L state での読み」で構成される。
リーディングエッジ (rising) で上位、トレイリングエッジ (falling) で下位。

---

## 3. **重要発見: MSX BIOS の 2 の補数否定 (NEG)**

### 3.1 観測

BASIC で以下のコードを実行:

```basic
40 S=PAD(12)
50 XX=XX+PAD(13):YY=YY+PAD(14)
```

**結果:**

| 状態 | PAD(13) | PAD(14) | 解釈 |
|---|---|---|---|
| ジョイスティックポートに何も接続なし | +1 | +1 | カーソルが右下にドリフトし続ける |
| 純正 MSX マウス接続、停止中 | 0 | 0 | カーソル静止 |
| 純正 MSX マウス、右下に動かす | 正の値 | 正の値 | カーソル右下移動 |

### 3.2 メカニズム

未接続時 D0-D3 は MSX 側 pullup で全 HIGH → PSG レジスタ 14 の下位ニブルは
`0xF`。BIOS は high nibble / low nibble を組み立てて byte = `0xFF` を得る。
このバイトに対して BIOS は **2 の補数で否定 (NEG)** してデルタを返す:

```
PAD(13) = -(int8_t)byte_raw

byte_raw = 0xFF (= signed -1)
→ PAD(13) = -(-1) = +1   ← ジョイスティック未接続時のデフォルト値
```

real mouse が「動きなし = byte 0x00」を返すと:

```
byte_raw = 0x00
→ PAD(13) = -0 = 0       ← ドリフトしない
```

### 3.3 raw byte と PAD() の符号関係 (重要)

MSX マウスの設計は raw 層と API 層で **符号が反転** している。
[MSX Wiki - Mouse/Trackball](https://www.msx.org/wiki/Mouse/Trackball) の
"Extended mouse protocol (2014)" 項目の記述:

> Byte 1: X-axis signed delta. **Positive value means mouse is moved to Left.**
> Byte 2: Y-axis signed delta. **Positive value means mouse is moved to Up.**

これは **wire 上の raw byte** の話で、mouse ハードウェア (内部カウンタ)
が出力するバイト値の符号規約。BIOS / BASIC の `PAD()` は raw byte を
NEG してから返すので、ユーザー目線では:

```
mouse moved Right → raw byte = negative → PAD = positive
mouse moved Left  → raw byte = positive → PAD = negative
mouse moved Down  → raw byte = negative → PAD = positive
mouse moved Up    → raw byte = positive → PAD = negative
```

= **PAD() は標準的な「右/下が正」になる**。raw 層が逆向きなのは mouse 内部
カウンタの実装都合と思われ、ユーザー向けに BIOS が NEG で吸収する設計。

本実装も最終的に PAD = acc (ユーザー意図) になることを目指すので、ユーザー
が「+5 = 右へ 5」と考えて acc に +5 を入れたとき raw byte は -5 になる
必要がある (= 5 章のエンコーディングの根拠)。

### 3.3 「未接続」と「停止中」を区別する物理的トリック

real MSX マウスは「動きなし」を以下のように表現する:

> **D0-D3 を全部 LOW にドライブする** (= MOSFET で GND に引き込む)

これによって電気的には:

| 状態 | D0-D3 | BIOS の byte | PAD 出力 |
|---|---|---|---|
| 未接続 | 全 HIGH (pullup 任せ) | `0xFF` | +1 (ドリフト) |
| マウス接続・停止 | **全 LOW (active 駆動)** | `0x00` | 0 |
| マウス接続・移動 | 一部 LOW / 一部 HIGH | 任意値 | 移動量 |

Hi-Z (pullup 経由 HIGH) と active LOW を意図的に使い分けるのがポイントで、
これを知らないと「停止中 = 全 Hi-Z」と思って実装すると未接続と区別が
つかなくなり、BIOS の +1 default に化けて右下ドリフトが発生する。

---

## 4. エンコーディング (V = acc - 1)

### 4.1 求める関係式

ユーザー意図のデルタを `acc` (signed int8) とし、PAD 出力もこの値にしたい:

```
PAD = acc                      ← 目標
PAD = -(int8_t)byte_raw        ← BIOS が実装している計算
→ byte_raw = -acc (mod 256)
```

### 4.2 active-low pack との関係

`msx_pack_nibble()` は active-low (bit=1 → 該当ピン LOW、bit=0 → Hi-Z) で
動作する。我々が「encode したい値」を `V` とすると、BIOS が見る生バイトは:

```
byte_raw = ~V (8bit bitwise NOT)
```

(active-low なので、bit=1 の位置で pin LOW = BIOS の bit 値 0 になる。
逆も同様で、結局 V の bitwise NOT が BIOS の読みとなる。)

### 4.3 結合

```
byte_raw = ~V = -acc (mod 256)
V = ~(-acc) = acc - 1 (mod 256)
```

(2 の補数で `~x = -x - 1` の関係を使った。)

### 4.4 具体値

| acc (目標 PAD) | V = acc - 1 | encode 後の D0-D3 (high+low) | byte_raw (BIOS 読み) | PAD 出力 |
|---|---|---|---|---|
| 0 | -1 = 0xFF | 全 LOW + 全 LOW | 0x00 | 0 ✓ |
| +5 | +4 = 0x04 | 全 HIGH + D2 のみ LOW | 0xFB | +5 ✓ |
| -3 | -4 = 0xFC | 全 LOW + D2,D3 LOW | 0x03 | -3 ✓ |
| -1 | -2 = 0xFE | 全 LOW + D1,D2,D3 LOW | 0x01 | -1 ✓ |

`acc = 0` のとき pin pattern が「全 LOW 駆動」になることに注目。これが
real mouse の「停止中」挙動と一致する。

### 4.5 実装コード (`joystick.c`)

```c
static void msx_rebuild_lut(void) {
    uint8_t x = (uint8_t)((int)msx_dx_acc - 1);  // V_x = acc_x - 1
    uint8_t y = (uint8_t)((int)msx_dy_acc - 1);  // V_y = acc_y - 1
    msx_lut_rising[0]  = msx_pack_nibble((x >> 4) & 0x0F);  // X 上位
    msx_lut_falling[0] = msx_pack_nibble(x & 0x0F);          // X 下位
    msx_lut_rising[1]  = msx_pack_nibble((y >> 4) & 0x0F);  // Y 上位
    msx_lut_falling[1] = msx_pack_nibble(y & 0x0F);          // Y 下位
}
```

---

## 5. byte 3 / byte 4 の扱い

### 5.1 観測事実

MSX View は 8 nibble (4 byte) を読むが、本実装の LUT は 2 エントリ循環
なので、DMA が自然に折り返して **byte 3 = byte 1 の繰り返し / byte 4 = byte
2 の繰り返し** を返す。

### 5.2 これで動く理由

MSX wiki 引用:

> "If bytes 3/4 are not read within 3 ms (counting starts from reading of
> 1st byte) mouse will return 1st byte on next read."

= 元々 original protocol mouse は byte 3/4 を要求されたら byte 1/2 を繰り
返して返す挙動が標準。MSX View もこれを前提にしている。

なお 2014 年に NYYRIKKI / Prodatron が策定した extended protocol では
byte 3 = ID + button、byte 4 = Z 軸となっているが、Turbo-R (1991) には
適用されない。Original protocol mouse として byte 1/2 の繰り返しを返す
だけで MSX View では正常動作する。

### 5.3 サイクル境界検出

`TIM2_CC_IRQHandler` で **falling edge のみ** を数え、1 サイクル = 4
fallings として境界を検出する。初期 strobe 状態 (HIGH / LOW どちらでも)
に依存せず安定。サイクル末で `msx_dx_acc` / `msx_dy_acc` を 0 リセット
して次サイクルでは「動きなし」を返す状態に戻す。

`TIM3` のアイドル watchdog (3 ms) でも 0 リセットする二重セーフティを
入れているが、MSX View の inter-strobe (~120-200 us) より長く、inter-cycle
(~15 ms) より短いので通常はサイクル間でのみ発火する。

---

## 6. アーキテクチャ概要 (実装側)

```
              ┌─────────────────┐
              │  MSX (TH 出力)  │
              └────────┬────────┘
                       │ pin 8
                       ▼
                   PA0 / TI1
                       │
              ┌────────┴────────┐
              │  TIM2 capture   │  rising / falling 両エッジ
              └────────┬────────┘
                       │ DMA トリガ
            ┌──────────┴───────────┐
            ▼                      ▼
  DMA1_Channel7 (rising)   DMA1_Channel5 (falling)
  msx_lut_rising[0..1]     msx_lut_falling[0..1]
            │                      │
            └──────────┬───────────┘
                       ▼
              GPIOA->BSHR  ← D0-D3 の HIGH/LOW を直接更新
                       │
                       ▼
               D-SUB pin 1-4 (MSX 側へ)


              ┌─────────────────┐
              │ USB MIDI CC 受信│
              └────────┬────────┘
                       ▼
              on_cc: msx_d{x,y}_acc 更新
                       ▼
              msx_rebuild_lut(): V = acc - 1 で
              4 ニブルを pack して LUT へ


              ┌─────────────────┐
              │ TIM2 CC ISR     │  4 falling = 1 cycle 完了
              │ (cycle complete)│  msx_d{x,y}_acc = 0
              └─────────────────┘  msx_rebuild_lut() で全 LUT を pack(0xF)
                                   = 全 LOW 状態へ
```

CPU 介入なしで DMA が TH エッジに同期して GPIOA->BSHR を更新するため、
ニブル切替レイテンシは数十 ns 以内。

---

## 7. MIDI プロトコル (アプリ ↔ firmware)

joystick の MIDI チャンネル (`MIDI_CH_JOYSTICK = 0`) を共用:

| メッセージ | 内容 |
|---|---|
| SysEx SET_CONFIG (key=0x03, val=3) | `PAD_MODE_MSX_MOUSE` へモード切替 |
| CC 0x30 (DX) | X デルタ加算。value=64 を中央としたオフセット表現 (-64..+63) |
| CC 0x31 (DY) | Y デルタ加算。同上 |
| Note On/Off 19 | 左マウスボタン |
| Note On/Off 20 | 右マウスボタン |

CC は 7bit (-64..+63) なので、それを超える delta はアプリ側で複数 CC に
分割して送る。firmware 側は受信のたびに `msx_d{x,y}_acc` に累積する。
サイクル末で 0 リセットされるので、累積はあくまで「次 MSX 読み取りまでに
受信した delta の合計」を表す。

---

## 8. 既知の制限事項

- 2014 年の extended protocol (byte 3 = ID + 拡張ボタン, byte 4 = Z 軸)
  には未対応。本実装は byte 3/4 を byte 1/2 の繰り返しで返す original
  protocol のみ。Wheel スクロールや 3rd+ ボタンは出ない
- inter-strobe 3 ms を超える driver には未検証 (TIM3 アイドルリセットが
  3 ms なので、それ以上の inter-strobe があると mid-cycle で再同期される)。
  実機 MSX View の inter-strobe は ~120-200 us なので余裕あり

---

## 9. 参照

- [MSX Wiki - Mouse/Trackball](https://www.msx.org/wiki/Mouse/Trackball)
- 実装: `platforms/ch32x035/src/functions/joystick/joystick.c` の MSX マウス関連セクション
- アプリ側: `MimicX-app/lib/joystick_page.dart` の `MsxMouseMode`

---

## 10. 変更履歴

- 2026-05-27 (v0.7.3): 初版。Turbo-R + MSX View 実機での観測と動作確認を
  経て、BIOS NEG 仕様および V = acc - 1 エンコーディングを確定。
