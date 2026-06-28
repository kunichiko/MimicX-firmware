// ===================================================================================
// i2c_bridge.c  (ESP32 / I2C マスター — CH32 デバイスエンジンへの橋渡し, protocol §2.5)
// ===================================================================================
#include "i2c_bridge.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bridge";

// --- 配線 (暫定。基板に合わせて変更可) ---
#define I2C_SDA_GPIO   GPIO_NUM_21
#define I2C_SCL_GPIO   GPIO_NUM_22
#define I2C_INT_GPIO   GPIO_NUM_19      // CH32 PB1(INT) ← 入力
#define CH32_ADDR      0x33
#define I2C_HZ         400000

#define FRAME_MAX      64               // 1 フレーム最大 ([LEN] + payload)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_mutex;     // バス排他 (write と read タスク)
static TaskHandle_t            s_reader;
static void (*s_on_rx)(const uint8_t* midi, int len);

// host→device: [LEN][MIDI] を CH32 へ書き込む。
void i2c_bridge_write(const uint8_t* midi, int len) {
    if (len <= 0 || len > FRAME_MAX - 1) return;
    uint8_t frame[FRAME_MAX];
    frame[0] = (uint8_t)len;
    memcpy(&frame[1], midi, len);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    esp_err_t e = i2c_master_transmit(s_dev, frame, len + 1, 50);
    xSemaphoreGive(s_mutex);
    if (e != ESP_OK) ESP_LOGW(TAG, "write err %d", (int)e);
}

// device→host: CH32 から 1 フレーム読む。戻り値 = payload バイト数 (0=データ無し, <0=err)。
static int read_one_frame(uint8_t* out, int out_max) {
    uint8_t buf[FRAME_MAX];
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return -1;
    esp_err_t e = i2c_master_receive(s_dev, buf, sizeof(buf), 50);
    xSemaphoreGive(s_mutex);
    if (e != ESP_OK) return -1;
    int len = buf[0];                    // 先頭が LEN
    if (len <= 0) return 0;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    if (len > out_max) len = out_max;
    memcpy(out, &buf[1], len);
    return len;
}

// INT (CH32→ESP32) Low で起こされ、CH32 のキューを空 (LEN=0) になるまで読み出す。
static void reader_task(void *arg) {
    (void)arg;
    uint8_t midi[FRAME_MAX];
    for (;;) {
        // INT がすでに Low なら即処理、そうでなければ通知待ち (フォールバックで定期起床)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        for (int guard = 0; guard < 32; guard++) {     // 1 起床で最大 32 フレーム
            int n = read_one_frame(midi, sizeof(midi));
            if (n <= 0) break;                          // 0=空, <0=err
            if (s_on_rx) s_on_rx(midi, n);
        }
    }
}

static void IRAM_ATTR int_isr(void *arg) {
    (void)arg;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_reader, &hp);
    portYIELD_FROM_ISR(hp);
}

void i2c_bridge_init(void (*on_rx)(const uint8_t* midi, int len)) {
    s_on_rx = on_rx;
    s_mutex = xSemaphoreCreateMutex();

    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = 0,
        .scl_io_num        = I2C_SCL_GPIO,
        .sda_io_num        = I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // 外付けプルアップ併用 (必須)
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CH32_ADDR,
        .scl_speed_hz    = I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    // INT 入力 (CH32 active-low / open-drain)。内蔵プルアップ有効、立ち下がり割り込み。
    gpio_config_t ic = {
        .pin_bit_mask = (1ULL << I2C_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&ic);

    xTaskCreate(reader_task, "i2c_rd", 4096, NULL, 8, &s_reader);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(I2C_INT_GPIO, int_isr, NULL);

    ESP_LOGI(TAG, "I2C bridge: SDA=%d SCL=%d INT=%d addr=0x%02X",
             I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_INT_GPIO, CH32_ADDR);
}
