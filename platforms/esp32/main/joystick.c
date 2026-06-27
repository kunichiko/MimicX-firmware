// ===================================================================================
// joystick.c  (ESP32 / ATARI ジョイスティック出力)
// ===================================================================================
// ATARI / D-SUB 9pin 互換のデジタルジョイスティックを GPIO で駆動する。
// 配線は active-low (open-drain): 押下 = Low、解放 = Hi-Z (コンソール側プルアップ)。
//
// CH32 版 (platforms/ch32x035/.../joystick.c) の ATARI モード相当。MD6 / MSX マウス
// モードは CH32 の DMA/EXTI ハードに依存するため ESP32 では未対応 (将来別実装)。
//
// ※ GPIO 割り当ては暫定。ハードウェア基板 (MimicX-hardware) 確定時に再マップする。
// ===================================================================================
#include "joystick.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "joystick";

// MIDI Note 番号 (protocol/joystick と一致): 1=UP 2=DOWN 3=LEFT 4=RIGHT 6=A 7=B
// → ESP32 GPIO (暫定割り当て。入力専用 34-39 / strapping 0,2,5,12,15 / flash 6-11 は回避)
typedef struct {
    uint8_t     note;
    gpio_num_t  pin;
    const char *name;
} jbtn_t;

// COMMON (console SELECT 入力) = GPIO32。ATARI モードでは COMMON を未使用 (MD6/MSX 実装時に使う)。
// 出力 6 本はすべて出力可・strapping/入力専用を回避済み。
static const jbtn_t BTNS[] = {
    {1, GPIO_NUM_13, "UP"},      // UP     = GPIO13
    {2, GPIO_NUM_27, "DOWN"},    // DOWN   = GPIO27
    {3, GPIO_NUM_25, "LEFT"},    // LEFT   = GPIO25
    {4, GPIO_NUM_33, "RIGHT"},   // RIGHT  = GPIO33
    {6, GPIO_NUM_14, "A"},       // TRIG-A = GPIO14
    {7, GPIO_NUM_26, "B"},       // TRIG-B = GPIO26
};
#define NBTN ((int)(sizeof(BTNS) / sizeof(BTNS[0])))

static const jbtn_t *find_btn(uint8_t note) {
    for (int i = 0; i < NBTN; i++) {
        if (BTNS[i].note == note) return &BTNS[i];
    }
    return NULL;
}

// open-drain: 押下 = 0 (Low / アクティブ)、解放 = 1 (Hi-Z)
static void set_btn(const jbtn_t *b, int pressed) {
    gpio_set_level(b->pin, pressed ? 0 : 1);
    ESP_LOGI(TAG, "%-5s %s (GPIO%d -> %s)",
             b->name, pressed ? "press  " : "release",
             (int)b->pin, pressed ? "LOW" : "Hi-Z");
}

void joystick_init(void) {
    uint64_t mask = 0;
    for (int i = 0; i < NBTN; i++) mask |= (1ULL << BTNS[i].pin);

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT_OD,   // open-drain (ATARI active-low)
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    for (int i = 0; i < NBTN; i++) gpio_set_level(BTNS[i].pin, 1);  // 全解放 (Hi-Z)
    ESP_LOGI(TAG, "ATARI joystick init: %d GPIO (open-drain, active-low)", NBTN);
}

void joystick_note_on(uint8_t note) {
    const jbtn_t *b = find_btn(note);
    if (b) set_btn(b, 1);
}

void joystick_note_off(uint8_t note) {
    const jbtn_t *b = find_btn(note);
    if (b) set_btn(b, 0);
}

void joystick_release_all(void) {
    for (int i = 0; i < NBTN; i++) gpio_set_level(BTNS[i].pin, 1);
    ESP_LOGI(TAG, "release all");
}
