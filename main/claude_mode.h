#ifndef CLAUDE_MODE_H
#define CLAUDE_MODE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Claude 运行状态枚举
typedef enum {
    CLAUDE_STATE_DISCONNECTED = 0, // BLE 未连接
    CLAUDE_STATE_IDLE,             // Claude 空闲 / 等待用户输入
    CLAUDE_STATE_THINKING,         // 模型推理中
    CLAUDE_STATE_TOOL_USE,         // 正在调用工具
    CLAUDE_STATE_WRITING,          // 正在写入/编辑文件
    CLAUDE_STATE_WAITING_INPUT,    // 等待用户确认/输入
    CLAUDE_STATE_ERROR,            // 出错
    CLAUDE_STATE_DONE,             // 任务完成
    CLAUDE_STATE_MAX
} claude_state_t;

// 单次 Claude 状态快照 (由 PC 通过 BLE 推送)
typedef struct {
    claude_state_t state;
    char tool[24];          // 当前工具名称 (Read/Edit/Bash...)
    char model[24];          // 模型名 (Opus 4.7 / Sonnet 4.6 ...)
    uint32_t tokens_in;      // 累计输入 token
    uint32_t tokens_out;     // 累计输出 token
    char msg[64];            // 简短状态消息
} claude_status_t;

/**
 * @brief 初始化 Claude 状态模块 (创建 LVGL 控件, 初始化 BLE NimBLE NUS 服务)
 *        必须在 LVGL 初始化之后调用
 * @param parent  Claude UI 的父屏幕 (一般传 lv_scr_act())
 * @return ESP_OK 成功
 */
esp_err_t claude_mode_init(lv_obj_t *parent);

/**
 * @brief 进入 Claude 状态模式: 显示 Claude UI 面板, 启动 BLE 广播, 接管 WS2812
 */
void claude_mode_enter(void);

/**
 * @brief 退出 Claude 状态模式: 隐藏 Claude UI, 停止 BLE 广播, 释放 WS2812 覆盖
 */
void claude_mode_exit(void);

/**
 * @brief 当前是否处于 Claude 模式
 */
bool claude_mode_is_active(void);

/**
 * @brief 由 LVGL 任务调用, 周期性刷新 UI (使用通知机制避免 LVGL 跨任务调用)
 */
void claude_mode_lvgl_refresh(void);

/**
 * @brief 状态名 -> 字符串 (UI 显示)
 */
const char *claude_state_name(claude_state_t s);

/**
 * @brief 直接喂入 JSON 帧 (内部使用, 也可从 UART/其它来源传入)
 */
void claude_mode_feed_json(const char *json, size_t len);

/**
 * @brief 由其它传输 (例如 WiFi UDP) 控制 Claude 面板显隐
 *        BLE 路径仍走 claude_mode_enter/exit; WiFi 路径用这个独立控制可见性,
 *        而无需启动 BLE 栈
 */
void claude_mode_panel_show(bool show);

/**
 * @brief 由其它传输覆写右下角 "BLE: ..." 链路标签 (例如 "WiFi: 1.2.3.4")
 *        在 WiFi 模式下 BLE 不工作, claude_mode_lvgl_refresh 不会覆盖该标签
 * @param text       要显示的短文本 (NULL 则恢复默认)
 * @param color_rgb  0xRRGGBB 颜色
 */
void claude_mode_set_link_text(const char *text, uint32_t color_rgb);

/**
 * @brief 让 WS2812 跟随状态色 (供 WiFi 路径使用; BLE 路径自带)
 */
void claude_mode_drive_ws2812(bool enable);

/**
 * @brief 强制把"已就绪"状态写入快照 (替代 BLE 的 'Connected' 默认值)
 */
void claude_mode_set_ready_msg(const char *msg);

#ifdef __cplusplus
}
#endif

#endif // CLAUDE_MODE_H
