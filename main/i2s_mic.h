#ifndef I2S_MIC_H
#define I2S_MIC_H

#include "esp_err.h"

// I2S 配置
#define I2S_MIC_PORT        I2S_NUM_0
#define I2S_MIC_CLK_PIN     5
#define I2S_MIC_DATA_PIN    6

/**
 * @brief 初始化I2S数字麦克风
 * @return ESP_OK 成功, 其他值 失败
 */
esp_err_t i2s_mic_init(void);

/**
 * @brief 获取麦克风音量大小
 * 
 * @return int32_t 音量大小 (RMS值)
 */
int32_t get_mic_volume(void);

#endif // I2S_MIC_H
