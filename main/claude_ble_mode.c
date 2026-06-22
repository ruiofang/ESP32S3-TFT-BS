/*
 * Claude 状态模式实现：
 *   - 通过 BLE NimBLE Nordic UART Service (NUS) 接收 PC 推送的 JSON 状态
 *   - 在 LCD (LVGL) 上展示 Claude 当前状态/工具/模型/token 计数/简短消息
 *   - 通过 WS2812 RGB 颜色覆盖反映状态
 *
 * 协议 (PC 写入 NUS RX 字符 6E400002-...):
 *   {"state":"thinking","tool":"Read","model":"Opus 4.7","ti":12345,"to":678,"msg":"..."}
 *   - state: idle / thinking / tool / writing / waiting / error / done
 *   - 其它字段均为可选
 *
 * LCD 与 WS2812 的更新由 LVGL 主任务调用 claude_ble_mode_lvgl_refresh() 完成；
 * BLE 回调只把状态写到带互斥锁的快照里，避免跨任务直接操作 LVGL。
 */

#include "claude_ble_mode.h"
#include "ws2812_control.h"
#include "Lib/cJSON/cJSON.h"

#include "esp_log.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "CLAUDE"

// -----------------------------------------------------------------------------
// NUS UUIDs (Nordic UART Service)
//   Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX     : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (PC->ESP, write)
//   TX     : 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (ESP->PC, notify)
// NimBLE 的 BLE_UUID128_INIT 是小端顺序传入
// -----------------------------------------------------------------------------
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

// -----------------------------------------------------------------------------
// 状态机 / 共享数据
// -----------------------------------------------------------------------------
typedef struct {
    uint8_t r, g, b;
} rgb_t;

// 状态 -> 显示色 (RGB888) / 状态文本
static const struct {
    const char *name;
    rgb_t       color;
    uint32_t    lcd_color_rgb;  // 0xRRGGBB
} kStateTable[CLAUDE_STATE_MAX] = {
    [CLAUDE_STATE_DISCONNECTED] = { "OFFLINE",   {0x20, 0x20, 0x20}, 0x808080 },
    [CLAUDE_STATE_IDLE]         = { "IDLE",      {0x00, 0x80, 0x00}, 0x00C800 },
    [CLAUDE_STATE_THINKING]     = { "THINKING",  {0x00, 0x40, 0xFF}, 0x4080FF },
    [CLAUDE_STATE_TOOL_USE]     = { "TOOL USE",  {0x00, 0xC0, 0xC0}, 0x00C8FF },
    [CLAUDE_STATE_WRITING]      = { "WRITING",   {0xC0, 0x20, 0xC0}, 0xFF40FF },
    [CLAUDE_STATE_WAITING_INPUT]= { "INPUT?",    {0xFF, 0xC0, 0x00}, 0xFFC000 },
    [CLAUDE_STATE_ERROR]        = { "ERROR",     {0xFF, 0x00, 0x00}, 0xFF2020 },
    [CLAUDE_STATE_DONE]         = { "DONE",      {0xFF, 0xFF, 0xFF}, 0xFFFFFF },
};

const char *claude_state_name(claude_state_t s)
{
    if (s >= CLAUDE_STATE_MAX) return "?";
    return kStateTable[s].name;
}

// 当前快照 (BLE 回调写, LVGL 任务读)
static claude_status_t s_snapshot = {
    .state = CLAUDE_STATE_DISCONNECTED,
    .tool = "",
    .model = "",
    .tokens_in = 0,
    .tokens_out = 0,
    .msg = "Waiting for BLE host...",
};
static SemaphoreHandle_t s_lock;       // 保护 s_snapshot
static volatile bool s_dirty;          // LVGL 任务需要刷新
static volatile bool s_active;         // 是否处于 Claude (BLE) 模式
static volatile bool s_ext_ws2812;     // 由 WiFi 路径请求驱动 WS2812
// 由 WiFi 路径覆写的链路标签 (BLE 模式下保持空字符串, 由本文件 refresh 自管)
static char s_ext_link_text[32] = "";
static uint32_t s_ext_link_color = 0xAAAAAA;
static volatile bool s_ext_link_set = false;

// BLE 句柄
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_attr_handle = 0;  // TX 字符 value handle (用于 notify)
static uint8_t  s_own_addr_type;
static bool     s_ble_inited = false;
static bool     s_advertising = false;

// 累积 RX 缓冲 (允许 PC 端按行 / 多包分片发送 JSON)
#define CLAUDE_RX_BUF_LEN 512
static char     s_rx_buf[CLAUDE_RX_BUF_LEN];
static size_t   s_rx_buf_len = 0;

// LVGL 控件
static lv_obj_t *s_panel = NULL;          // 整个 Claude 面板容器
static lv_obj_t *s_state_label = NULL;     // 大字状态
static lv_obj_t *s_state_dot = NULL;       // 状态色块
static lv_obj_t *s_tool_label = NULL;      // 当前工具
static lv_obj_t *s_model_label = NULL;     // 模型 + token
static lv_obj_t *s_msg_label = NULL;       // 消息
static lv_obj_t *s_link_label = NULL;      // BLE 连接状态

LV_FONT_DECLARE(claude_status_font_14);

// -----------------------------------------------------------------------------
// 状态 -> 字符串 / 解析
// -----------------------------------------------------------------------------
static claude_state_t parse_state(const char *s)
{
    if (!s) return CLAUDE_STATE_IDLE;
    if (!strcasecmp(s, "idle"))     return CLAUDE_STATE_IDLE;
    if (!strcasecmp(s, "thinking")) return CLAUDE_STATE_THINKING;
    if (!strcasecmp(s, "tool") || !strcasecmp(s, "tool_use")) return CLAUDE_STATE_TOOL_USE;
    if (!strcasecmp(s, "writing") || !strcasecmp(s, "edit"))  return CLAUDE_STATE_WRITING;
    if (!strcasecmp(s, "waiting") || !strcasecmp(s, "input")) return CLAUDE_STATE_WAITING_INPUT;
    if (!strcasecmp(s, "error") || !strcasecmp(s, "fail"))    return CLAUDE_STATE_ERROR;
    if (!strcasecmp(s, "done") || !strcasecmp(s, "stop"))     return CLAUDE_STATE_DONE;
    return CLAUDE_STATE_IDLE;
}

static void apply_status(const claude_status_t *src)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_snapshot = *src;
        s_dirty = true;
        xSemaphoreGive(s_lock);
    }
}

static void parse_and_apply_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "cJSON_Parse failed: %.*s", 64, json);
        return;
    }
    claude_status_t st;
    // 先读取当前快照作为默认值, 这样 PC 可以增量更新单个字段
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        st = s_snapshot;
        xSemaphoreGive(s_lock);
    } else {
        memset(&st, 0, sizeof(st));
    }

    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "state")) && cJSON_IsString(j)) {
        st.state = parse_state(j->valuestring);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "tool")) && cJSON_IsString(j)) {
        strncpy(st.tool, j->valuestring, sizeof(st.tool) - 1);
        st.tool[sizeof(st.tool) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "model")) && cJSON_IsString(j)) {
        strncpy(st.model, j->valuestring, sizeof(st.model) - 1);
        st.model[sizeof(st.model) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "ti")) && cJSON_IsNumber(j)) {
        st.tokens_in = (uint32_t)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "to")) && cJSON_IsNumber(j)) {
        st.tokens_out = (uint32_t)j->valuedouble;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(root, "msg")) && cJSON_IsString(j)) {
        strncpy(st.msg, j->valuestring, sizeof(st.msg) - 1);
        st.msg[sizeof(st.msg) - 1] = '\0';
    }
    cJSON_Delete(root);
    apply_status(&st);
}

void claude_ble_mode_feed_json(const char *json, size_t len)
{
    // 把 [json, json+len) 追加到 RX 缓冲, 按 '\n' 或 '\0' 切分
    for (size_t i = 0; i < len; i++) {
        char c = json[i];
        if (c == '\r') continue;
        if (c == '\n' || c == '\0') {
            if (s_rx_buf_len > 0) {
                s_rx_buf[s_rx_buf_len] = '\0';
                parse_and_apply_json(s_rx_buf);
                s_rx_buf_len = 0;
            }
        } else {
            if (s_rx_buf_len + 1 < CLAUDE_RX_BUF_LEN) {
                s_rx_buf[s_rx_buf_len++] = c;
            } else {
                // 溢出, 丢弃当前缓冲
                ESP_LOGW(TAG, "RX buffer overflow, drop");
                s_rx_buf_len = 0;
            }
        }
    }
    // PC 端不带换行也允许: 如果当前缓冲看起来已经是完整 JSON ('}' 结尾且配平), 也立即解析
    if (s_rx_buf_len > 0 && s_rx_buf[s_rx_buf_len - 1] == '}') {
        // 简单括号配平
        int depth = 0;
        bool ok = true;
        for (size_t i = 0; i < s_rx_buf_len; i++) {
            if (s_rx_buf[i] == '{') depth++;
            else if (s_rx_buf[i] == '}') depth--;
            if (depth < 0) { ok = false; break; }
        }
        if (ok && depth == 0) {
            s_rx_buf[s_rx_buf_len] = '\0';
            parse_and_apply_json(s_rx_buf);
            s_rx_buf_len = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// LVGL UI
// -----------------------------------------------------------------------------
static void ui_build(lv_obj_t *parent)
{
    // 容器: 撑满除左上 MODE 标签外的剩余区域
    s_panel = lv_obj_create(parent);
    lv_obj_set_size(s_panel, 420, 110);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 4, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // 状态色块 (左侧大圆点)
    s_state_dot = lv_obj_create(s_panel);
    lv_obj_set_size(s_state_dot, 40, 40);
    lv_obj_align(s_state_dot, LV_ALIGN_LEFT_MID, 4, -16);
    lv_obj_set_style_radius(s_state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_state_dot, lv_color_hex(0x808080), 0);
    lv_obj_set_style_border_width(s_state_dot, 0, 0);

    // 大字状态 (放色块右侧)
    s_state_label = lv_label_create(s_panel);
    lv_label_set_text(s_state_label, "OFFLINE");
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_state_label, LV_ALIGN_LEFT_MID, 56, -16);

    // 当前工具 (大字右下角)
    s_tool_label = lv_label_create(s_panel);
    lv_label_set_text(s_tool_label, "");
    lv_obj_set_style_text_color(s_tool_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_tool_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_tool_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    // 模型 + token 信息 (面板下方)
    s_model_label = lv_label_create(s_panel);
    lv_label_set_text(s_model_label, "");
    lv_obj_set_style_text_color(s_model_label, lv_color_hex(0x80C0FF), 0);
    lv_obj_set_style_text_font(s_model_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_model_label, LV_ALIGN_BOTTOM_LEFT, 4, -22);

    // 简短消息
    s_msg_label = lv_label_create(s_panel);
    lv_label_set_long_mode(s_msg_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_msg_label, 408);
    lv_label_set_text(s_msg_label, "Waiting for BLE host...");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_msg_label, &claude_status_font_14, 0);
    lv_obj_align(s_msg_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    // BLE 连接状态 (面板右下角)
    s_link_label = lv_label_create(s_panel);
    lv_label_set_text(s_link_label, "BLE: --");
    lv_obj_set_style_text_color(s_link_label, lv_color_hex(0xFFAA40), 0);
    lv_obj_set_style_text_font(s_link_label, &lv_font_montserrat_12, 0);
    lv_obj_align(s_link_label, LV_ALIGN_BOTTOM_RIGHT, -4, -4);

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void claude_ble_mode_lvgl_refresh(void)
{
    if (!s_panel) return;

    claude_status_t st;
    bool dirty;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        st = s_snapshot;
        dirty = s_dirty;
        s_dirty = false;
        xSemaphoreGive(s_lock);
    } else {
        return;
    }
    if (!dirty) return;

    // 状态文本与颜色
    lv_label_set_text(s_state_label, claude_state_name(st.state));
    lv_obj_set_style_text_color(s_state_label,
        lv_color_hex(kStateTable[st.state].lcd_color_rgb), 0);
    lv_obj_set_style_bg_color(s_state_dot,
        lv_color_hex(kStateTable[st.state].lcd_color_rgb), 0);

    // 工具
    if (st.tool[0]) {
        lv_label_set_text_fmt(s_tool_label, "TOOL: %s", st.tool);
    } else {
        lv_label_set_text(s_tool_label, "");
    }

    // 模型 + token
    if (st.model[0] || st.tokens_in || st.tokens_out) {
        lv_label_set_text_fmt(s_model_label, "%s  in:%u  out:%u",
            st.model[0] ? st.model : "?",
            (unsigned)st.tokens_in, (unsigned)st.tokens_out);
    } else {
        lv_label_set_text(s_model_label, "");
    }

    // 消息
    lv_label_set_text(s_msg_label, st.msg[0] ? st.msg : "");

    // 链路状态: 优先显示外部 (WiFi) 覆写文本; 否则按 BLE 状态自管
    if (s_ext_link_set) {
        lv_label_set_text(s_link_label, s_ext_link_text);
        lv_obj_set_style_text_color(s_link_label, lv_color_hex(s_ext_link_color), 0);
    } else if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        lv_label_set_text(s_link_label, "BLE: LINKED");
        lv_obj_set_style_text_color(s_link_label, lv_color_hex(0x00FF80), 0);
    } else if (s_advertising) {
        lv_label_set_text(s_link_label, "BLE: ADV");
        lv_obj_set_style_text_color(s_link_label, lv_color_hex(0xFFAA40), 0);
    } else {
        lv_label_set_text(s_link_label, "BLE: OFF");
        lv_obj_set_style_text_color(s_link_label, lv_color_hex(0x666666), 0);
    }

    // 同步驱动 WS2812 颜色 (BLE 模式自动启用; WiFi 模式由外部启用)
    if (s_active || s_ext_ws2812) {
        const rgb_t *c = &kStateTable[st.state].color;
        ws2812_set_claude_override(true, c->r, c->g, c->b, 200);
    }
}

// -----------------------------------------------------------------------------
// GATT 表
// -----------------------------------------------------------------------------
static int nus_rx_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    if (total == 0) return 0;
    if (total > CLAUDE_RX_BUF_LEN) total = CLAUDE_RX_BUF_LEN;

    char tmp[CLAUDE_RX_BUF_LEN];
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &out_len);
    if (rc != 0 || out_len == 0) return 0;

    claude_ble_mode_feed_json(tmp, out_len);
    return 0;
}

static int nus_tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    // TX 字符仅用于 notify, 读 / 写都返回空
    return 0;
}

static const struct ble_gatt_chr_def s_nus_chars[] = {
    {
        .uuid       = &NUS_RX_UUID.u,
        .access_cb  = nus_rx_write_cb,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        // 标准 Nordic NUS: TX 字符只支持 NOTIFY, 不支持 READ
        .uuid       = &NUS_TX_UUID.u,
        .access_cb  = nus_tx_access_cb,
        .val_handle = &s_tx_attr_handle,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type         = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid         = &NUS_SVC_UUID.u,
        .characteristics = s_nus_chars,
    },
    { 0 },
};

// -----------------------------------------------------------------------------
// GAP 广播
// -----------------------------------------------------------------------------
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    if (!s_ble_inited || s_advertising) return;

    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // 31 字节广播包预算紧张, 设备名已有 17 字符 + 头, 不再加 TX 功率字段
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    // 把 NUS 服务 UUID 放到扫描响应里 (主广播包空间有限)
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&NUS_SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_cb, NULL);
    if (rc == 0) {
        s_advertising = true;
        ESP_LOGI(TAG, "BLE adv started as '%s'", name);
    } else {
        ESP_LOGW(TAG, "adv_start rc=%d", rc);
    }
}

static void stop_advertising(void)
{
    if (!s_advertising) return;
    ble_gap_adv_stop();
    s_advertising = false;
    ESP_LOGI(TAG, "BLE adv stopped");
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected, handle=%u", s_conn_handle);
            s_advertising = false;
            // 立即推一次当前状态给 PC
            claude_status_t st;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                st = s_snapshot;
                st.state = (st.state == CLAUDE_STATE_DISCONNECTED) ? CLAUDE_STATE_IDLE : st.state;
                strncpy(st.msg, "Connected", sizeof(st.msg) - 1);
                st.msg[sizeof(st.msg) - 1] = '\0';
                s_snapshot = st;
                s_dirty = true;
                xSemaphoreGive(s_lock);
            }
        } else {
            ESP_LOGW(TAG, "BLE connect failed status=%d", event->connect.status);
            if (s_active) start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        int r = event->disconnect.reason;
        // NimBLE encodes HCI reason as 0x200 + hci_code
        int hci = (r >= 0x200 && r < 0x300) ? (r - 0x200) : -1;
        ESP_LOGI(TAG, "BLE disconnected, reason=%d (0x%03X) hci=%d",
                  r, r, hci);
    }
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_snapshot.state = CLAUDE_STATE_DISCONNECTED;
            strncpy(s_snapshot.msg, "BLE disconnected", sizeof(s_snapshot.msg) - 1);
            s_snapshot.msg[sizeof(s_snapshot.msg) - 1] = '\0';
            s_dirty = true;
            xSemaphoreGive(s_lock);
        }
        s_rx_buf_len = 0;
        if (s_active) start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_active) start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE subscribe attr=%u notify=%d",
                  event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU update conn=%u mtu=%u",
                  event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption change: status=%d conn=%u",
                  event->enc_change.status, event->enc_change.conn_handle);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // just-works (NoInputOutput) 配对通常不会进这里, 防御性处理
        ESP_LOGW(TAG, "BLE passkey action requested (action=%d) - rejecting",
                  event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // 旧 bond 与新发起的配对冲突 — 删除旧的, 接受新的
        ESP_LOGW(TAG, "BLE repeat pairing requested - deleting old bond");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// NimBLE host 回调 / 任务
// -----------------------------------------------------------------------------
static void on_host_sync(void)
{
    // 使用固定 static random 地址: 从 BT MAC 派生, 每次重启保持一致;
    // 最高字节 bit7..6 必须为 11 才符合 BLE static random 地址规范。
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ble_addr_t addr = {0};
    addr.val[5] = (mac[0] & 0x3F) | 0xC0;
    addr.val[4] = mac[1];
    addr.val[3] = mac[2];
    addr.val[2] = mac[3];
    addr.val[1] = mac[4];
    addr.val[0] = mac[5];
    int rc = 0;
    rc = ble_hs_id_set_rnd(addr.val);
    if (rc != 0) {
        ESP_LOGE(TAG, "set rnd addr rc=%d", rc);
        return;
    }
    s_own_addr_type = BLE_OWN_ADDR_RANDOM;
    ESP_LOGI(TAG, "Advertising with fixed static random addr %02X:%02X:%02X:%02X:%02X:%02X",
             addr.val[5], addr.val[4], addr.val[3],
             addr.val[2], addr.val[1], addr.val[0]);

    if (s_active) start_advertising();
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
    s_advertising = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t ble_stack_init(void)
{
    if (s_ble_inited) return ESP_OK;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_host_reset;
    ble_hs_cfg.sync_cb  = on_host_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Security Manager 配置: just-works, 不 bonding。
    // 固定 BLE 地址下如果 bonding 但 ESP 端不持久化密钥, 主机容易留下过期 bond。
    // NUS 字符不要求加密, Claude bridge 通过 GATT 直接写入即可。
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 1;
    ble_hs_cfg.sm_keypress = 0;
    ble_hs_cfg.sm_our_key_dist   = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add rc=%d", rc);
        return ESP_FAIL;
    }

    // 设备名: ESP32_Claude_XXXX  (XXXX = MAC 后 2 字节)
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    char name[32];
    snprintf(name, sizeof(name), "ESP32_Claude_%02X%02X", mac[4], mac[5]);
    rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGW(TAG, "set name rc=%d", rc);
    }

    nimble_port_freertos_init(nimble_host_task);
    s_ble_inited = true;
    ESP_LOGI(TAG, "BLE NimBLE NUS ready, device name='%s'", name);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// 对外 API
// -----------------------------------------------------------------------------
esp_err_t claude_ble_mode_init(lv_obj_t *parent)
{
    if (!parent) return ESP_ERR_INVALID_ARG;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    ui_build(parent);
    // BLE 栈采用 "延迟初始化": 默认启动只创建 LVGL 控件, 避免影响其它模式
    // 真正进入 CLAUDE 模式时才启动 NimBLE, 以减小与 WiFi/LVGL 抢资源的风险
    return ESP_OK;
}

void claude_ble_mode_enter(void)
{
    s_active = true;
    if (s_panel) lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    // 初始状态: 等待 PC 连接
    claude_status_t st = {
        .state = CLAUDE_STATE_DISCONNECTED,
        .tokens_in = 0, .tokens_out = 0,
    };
    strncpy(st.msg, "Waiting for BLE host...", sizeof(st.msg) - 1);
    apply_status(&st);

    // 默认离线色块 (先点亮 RGB, 让用户立刻看到模式切换成功)
    const rgb_t *c = &kStateTable[CLAUDE_STATE_DISCONNECTED].color;
    ws2812_set_claude_override(true, c->r, c->g, c->b, 80);

    // 第一次进入时才初始化 BLE 栈
    // (CLAUDE 模式启动时, app_main 已跳过 WiFi 初始化, DRAM 留给 BLE 控制器)
    if (!s_ble_inited) {
        esp_err_t err = ble_stack_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BLE init failed (%s); panel-only mode",
                     esp_err_to_name(err));
            return;
        }
        // 广播由 on_host_sync 在 NimBLE 同步完成时自动启动, 这里不再重复调用
    } else if (!s_advertising && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        // 二次进入 (理论上不会发生, 因为 CLAUDE 切换会触发软重启)
        start_advertising();
    }
}

void claude_ble_mode_exit(void)
{
    // 实际上 CLAUDE -> 其它模式切换会触发软重启, 此函数仅做最少的关闭收尾
    s_active = false;
    if (s_panel) lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    ws2812_set_claude_override(false, 0, 0, 0, 0);
    stop_advertising();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool claude_ble_mode_is_active(void)
{
    return s_active;
}

// -----------------------------------------------------------------------------
// 外部传输 (WiFi) 用的辅助 API
// -----------------------------------------------------------------------------
void claude_ble_mode_panel_show(bool show)
{
    if (!s_panel) return;
    if (show) lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void claude_ble_mode_set_link_text(const char *text, uint32_t color_rgb)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (text && *text) {
        strncpy(s_ext_link_text, text, sizeof(s_ext_link_text) - 1);
        s_ext_link_text[sizeof(s_ext_link_text) - 1] = '\0';
        s_ext_link_color = color_rgb;
        s_ext_link_set = true;
    } else {
        s_ext_link_text[0] = '\0';
        s_ext_link_set = false;
    }
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

void claude_ble_mode_drive_ws2812(bool enable)
{
    s_ext_ws2812 = enable;
    if (!enable && !s_active) {
        ws2812_set_claude_override(false, 0, 0, 0, 0);
    }
    // 触发一次刷新, 立刻反映状态色
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_dirty = true;
        xSemaphoreGive(s_lock);
    }
}

void claude_ble_mode_set_ready_msg(const char *msg)
{
    if (!msg) msg = "";
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_snapshot.state = CLAUDE_STATE_DISCONNECTED;
    strncpy(s_snapshot.msg, msg, sizeof(s_snapshot.msg) - 1);
    s_snapshot.msg[sizeof(s_snapshot.msg) - 1] = '\0';
    s_dirty = true;
    xSemaphoreGive(s_lock);
}
