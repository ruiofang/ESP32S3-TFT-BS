#include "i2s_mic.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "I2S_MIC";

// 读取缓冲区长度（样本数）
#define I2S_READ_LEN 1024

// I2S 通道句柄
static i2s_chan_handle_t rx_handle = NULL;

// 使用静态缓冲区，避免频繁分配释放
static int16_t s_i2s_read_buff[I2S_READ_LEN];
static int32_t s_last_volume = 0;

esp_err_t i2s_mic_init(void) {
    // 1. 创建 I2S 接收通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    // 2. 配置 PDM RX 模式
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_MIC_CLK_PIN,
            .din = I2S_MIC_DATA_PIN,
        },
    };

    // 初始化 PDM RX 通道
    err = i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // 3. 启用通道
    err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2S PDM mic initialized (CLK GPIO=%d, DATA GPIO=%d) using New Driver", I2S_MIC_CLK_PIN, I2S_MIC_DATA_PIN);
    return ESP_OK;
}

int32_t get_mic_volume(void) {
    if (rx_handle == NULL) {
        return 0;
    }

    size_t bytes_read = 0;
    
    // 读取数据
    esp_err_t err = i2s_channel_read(rx_handle, s_i2s_read_buff, I2S_READ_LEN * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(50));
    
    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
             ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
        }
        return s_last_volume;
    }

    if (bytes_read <= 0) {
        // ESP_LOGW(TAG, "i2s_channel_read returned 0 bytes");
        return s_last_volume;
    }

    int sample_count = bytes_read / sizeof(int16_t);
    if (sample_count <= 0) {
        return s_last_volume;
    }

    // 计算RMS音量
    double sum = 0.0;
    for (int i = 0; i < sample_count; i++) {
        int32_t s = (int32_t)s_i2s_read_buff[i];
        sum += (double)s * (double)s;
    }
    double rms = sqrt(sum / (double)sample_count);

    // 平滑处理，减少抖动
    int32_t vol = (int32_t)rms;
    s_last_volume = (s_last_volume * 3 + vol) / 4;

    // 适度日志，便于现场调试
    ESP_LOGI(TAG, "RMS Volume: %" PRId32 " (samples=%d)", s_last_volume, sample_count);

    return s_last_volume;
}

