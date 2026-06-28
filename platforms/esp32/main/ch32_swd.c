// ===================================================================================
// ch32_swd.c  (ESP32 / CH32 RVSWD ビットバング — bitbang_rvswdio.h のプラットフォーム glue)
// ===================================================================================
#include "ch32_swd.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ch32_swd";

// bitbang_rvswdio.h は GPIO 構造体直叩き + xtensa PrecDelay を使う ESP32 実装を内蔵。
// 以下のシンボルを与えてから include する。
//   DisableISR/EnableISR: ビットバング中の割り込みを止めてタイミングを守る
//   (cookbook と同じく xtensa の割り込みレベル操作)。
#include "xtensa/xtruntime.h"
#define DisableISR()  XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL)
#define EnableISR()   XTOS_SET_INTLEVEL(0)

#define RUNNING_ON_ESP32
#define BB_PRINTF_DEBUG(...) do{}while(0)
#define MAX_IN_TIMEOUT 1000     // ビット読み出しのタイムアウト (cookbook 準拠)
#include "bitbang_rvswdio.h"

// 配線: SWDIO=GPIO21 (CH32 PC18 と結線), SWCLK=GPIO22 (CH32 PC19 と結線)。
#define SWDIO_PIN 21
#define SWCLK_PIN 22

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
    GPIO.out_w1ts    = s.pinmaskD | s.pinmaskC;
    GPIO.enable_w1ts = s.pinmaskD | s.pinmaskC;

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
