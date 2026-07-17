// ===================================================================================
// ch32_swd.c  (ESP32 / CH32 RVSWD ビットバング — bitbang_rvswdio.h のプラットフォーム glue)
// ===================================================================================
#include "ch32_swd.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "board_config.h"

static const char *TAG = "ch32_swd";

// bitbang_rvswdio.h は ESP32(Xtensa) 実装を内蔵する。GPIO アクセスと PrecDelay /
// 割り込み制御はアーキ依存なので、下記シンボルを与えてから include する。
//   DisableISR/EnableISR: ビットバング中の割り込みを止めてタイミングを守る。
#if defined(CONFIG_IDF_TARGET_ARCH_RISCV) || defined(CONFIG_IDF_TARGET_ESP32S3)
// RISC-V (ESP32-C3 / C6 等) と ESP32-S3: portMUX のクリティカルセクション
// (割り込み無効 + スピンロック) でタイミングを守る (portMUX は Xtensa でも動く)。
// S3 は Xtensa だが GPIO 構造体レイアウトが ESP32 classic と異なるため、
// レジスタ直アクセス版 (RUNNING_ON_ESP32_RISCV = REG_WRITE/READ) を使う。
// SWD 書込は起動時 (BLE 接続前) の一過性処理なので短時間の割り込み停止は許容。
static portMUX_TYPE s_swd_mux = portMUX_INITIALIZER_UNLOCKED;
#define DisableISR()  portENTER_CRITICAL(&s_swd_mux)
#define EnableISR()   portEXIT_CRITICAL(&s_swd_mux)
#define RUNNING_ON_ESP32_RISCV
#else
// Xtensa (ESP32 classic): cookbook と同じく割り込みレベルを直接操作。
#include "xtensa/xtruntime.h"
#define DisableISR()  XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL)
#define EnableISR()   XTOS_SET_INTLEVEL(0)
#define RUNNING_ON_ESP32
#endif

#define BB_PRINTF_DEBUG(...) do{}while(0)
#define MAX_IN_TIMEOUT 1000     // ビット読み出しのタイムアウト (cookbook 準拠)
#include "bitbang_rvswdio.h"

// 配線: SWDIO=SDA線 (CH32 PC18), SWCLK=SCL線 (CH32 PC19)。ピンは board_config.h。
#define SWDIO_PIN BOARD_SWDIO_GPIO
#define SWCLK_PIN BOARD_SWCLK_GPIO

static bool try_probe_once(int t1coeff, bool pullup, uint32_t *chip_type_out)
{
    struct SWIOState s;
    memset(&s, 0, sizeof(s));
    s.t1coeff          = t1coeff;
    s.pinmaskD         = 1u << SWDIO_PIN;
    s.pinmaskC         = 1u << SWCLK_PIN;
    s.target_chip_type = CHIP_UNKNOWN;
    s.sectorsize       = 256;

    // 入力+出力の両方を有効化する (INPUT_OUTPUT)。gpio_reset_pin は入力を殺すので不可:
    // ビットバングは GPIO.in で読むため入力を生かす必要がある。
    gpio_set_direction(SWDIO_PIN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(SWCLK_PIN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_pull_mode(SWDIO_PIN, pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
    gpio_set_pull_mode(SWCLK_PIN, pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
    BB_OUT_SET(s.pinmaskD | s.pinmaskC);
    BB_OE_SET(s.pinmaskD | s.pinmaskC);

    if (InitializeSWDSWIO(&s) != 0) return false;
    ESP_LOGI(TAG, "  link up! t1coeff=%d pullup=%d opmode=%d", t1coeff, pullup, s.opmode);
    if (DetermineChipTypeAndSectorInfo(&s, NULL) != 0) {
        ESP_LOGW(TAG, "  init ok but DetermineChipType failed");
        return false;
    }
    ESP_LOGI(TAG, "  chip_type=0x%02x sectorsize=%d", (unsigned)s.target_chip_type, s.sectorsize);
    if (chip_type_out) *chip_type_out = s.target_chip_type;
    return true;
}

bool ch32_swd_probe(uint32_t *chip_type_out)
{
    // 実機検証で t1coeff=10 + 内蔵プルアップで安定動作。念のため数段フォールバックする。
    static const int coeffs[] = { 10, 5, 20, 40 };
    for (unsigned i = 0; i < sizeof(coeffs)/sizeof(coeffs[0]); i++) {
        if (try_probe_once(coeffs[i], true, chip_type_out)) return true;
    }
    ESP_LOGW(TAG, "probe failed - SDI 有効? 配線? プルアップ?");
    return false;
}

// ピンを INPUT_OUTPUT で初期化し、RVSWD リンクを確立してチップ種別を判定する。
static bool link_up(struct SWIOState *s)
{
    memset(s, 0, sizeof(*s));
    s->t1coeff = 10;
    s->pinmaskD = 1u << SWDIO_PIN;
    s->pinmaskC = 1u << SWCLK_PIN;
    s->target_chip_type = CHIP_UNKNOWN;
    s->sectorsize = 256;

    gpio_set_direction(SWDIO_PIN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(SWCLK_PIN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_pull_mode(SWDIO_PIN, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SWCLK_PIN, GPIO_PULLUP_ONLY);
    BB_OUT_SET(s->pinmaskD | s->pinmaskC);
    BB_OE_SET(s->pinmaskD | s->pinmaskC);

    if (InitializeSWDSWIO(s) != 0) { ESP_LOGW(TAG, "link init failed"); return false; }
    if (DetermineChipTypeAndSectorInfo(s, NULL) != 0) { ESP_LOGW(TAG, "chip detect failed"); return false; }
    return true;
}

bool ch32_swd_flash(uint32_t flash_addr, const uint8_t *img, size_t len)
{
    if (len == 0) return false;

    struct SWIOState s;
    if (!link_up(&s)) return false;
    ESP_LOGI(TAG, "flash: chip=0x%02x sector=%d, writing %u bytes @0x%08x",
             (unsigned)s.target_chip_type, s.sectorsize, (unsigned)len, (unsigned)flash_addr);

    // halt + スレーブ出力許可 (DefaultHaltMode の HALT_BUT_NO_RESET と同等)
    MCFWriteReg32(&s, DMSHDWCFGR, 0x5aa50000 | (1 << 10));
    MCFWriteReg32(&s, DMCFGR,     0x5aa50000 | (1 << 10));
    MCFWriteReg32(&s, DMCONTROL, 0x80000001);
    MCFWriteReg32(&s, DMCONTROL, 0x80000001);
    MCFWriteReg32(&s, DMCONTROL, 0x80000001);
    esp_rom_delay_us(10000);

    // 64 バイト単位で erase+program (cookbook と同じ呼び方)
    for (size_t off = 0; off < len; off += 64) {
        uint8_t blk[64];
        size_t n = len - off; if (n > 64) n = 64;
        memset(blk, 0xFF, sizeof(blk));
        memcpy(blk, img + off, n);
        int r = WriteBlock(&s, flash_addr + (uint32_t)off, blk, 64, 1);
        if (r) { ESP_LOGE(TAG, "WriteBlock @0x%08x failed (%d)", (unsigned)(flash_addr+off), r); return false; }
        if ((off & 0x7FF) == 0) ESP_LOGI(TAG, "  prog %u/%u", (unsigned)off, (unsigned)len);
    }

    // verify (4 バイトずつ読み戻して比較。末尾は 0xFF 詰め)
    for (size_t off = 0; off < len; off += 4) {
        uint32_t expect = 0xFFFFFFFF;
        size_t n = len - off; if (n > 4) n = 4;
        memcpy(&expect, img + off, n);             // little-endian、残りは 0xFF のまま
        uint32_t got = 0;
        if (ReadWord(&s, flash_addr + (uint32_t)off, &got)) {
            ESP_LOGE(TAG, "ReadWord @0x%08x failed", (unsigned)(flash_addr+off)); return false;
        }
        if (got != expect) {
            ESP_LOGE(TAG, "verify mismatch @0x%08x: got=%08x want=%08x",
                     (unsigned)(flash_addr+off), (unsigned)got, (unsigned)expect);
            return false;
        }
    }
    ESP_LOGI(TAG, "verify OK (%u bytes)", (unsigned)len);

    // reboot (DefaultHaltMode の HALT_MODE_REBOOT)
    MCFWriteReg32(&s, DMCONTROL, 0x80000001);
    MCFWriteReg32(&s, DMCONTROL, 0x80000001);
    MCFWriteReg32(&s, DMCONTROL, 0x80000003);   // ndmreset
    MCFWriteReg32(&s, DMCONTROL, 0x40000001);   // resume
    esp_rom_delay_us(10000);
    ESP_LOGI(TAG, "flash done + reboot");
    return true;
}
