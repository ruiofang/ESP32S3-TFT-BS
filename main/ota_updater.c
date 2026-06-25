// OTA over HTTPS, manifest published as a GitHub release asset.
//
// Flow:
//   1) Boot -> ota_updater_init() loads s_auto_enabled from NVS.
//   2) When WiFi STA gets an IP and s_auto_enabled, spawn a one-shot task that
//      GETs OTA_MANIFEST_URL, parses {version,url}, compares to esp_app_desc,
//      and if newer runs esp_https_ota(url) then reboots.
//   3) Web server can call ota_updater_trigger_now() to force a check.

#include "ota_updater.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "cJSON.h"

#define TAG "ota_updater"

#define OTA_NVS_NAMESPACE   "ota"
#define OTA_NVS_KEY_ENABLED "auto"
#define OTA_MANIFEST_URL \
    "https://github.com/ruiofang/ESP32S3-TFT-BS/releases/latest/download/latest.json"
#define MANIFEST_MAX_LEN       1024
#define MANIFEST_FETCH_RETRIES 3
#define MANIFEST_BACKOFF_MS    1500   // doubles per attempt: 1.5s, 3s, 6s
#define ROLLBACK_CONFIRM_MS    30000  // 30s after init -> mark image valid

static volatile bool s_auto_enabled       = false;
static volatile bool s_check_in_progress  = false;
static volatile bool s_did_initial_check  = false;
static char          s_last_status[128]   = "idle";

// OTA更新发现的新版本信息
typedef struct {
    char version[32];
    char url[256];
    bool update_available;
    bool user_confirmed;
} ota_pending_update_t;
static ota_pending_update_t s_pending_update = {0};

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_status, sizeof(s_last_status), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "status: %s", s_last_status);
}

static int version_cmp(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
    if (a1 != b1) return a1 - b1;
    if (a2 != b2) return a2 - b2;
    return a3 - b3;
}

// Fetch the manifest JSON into a caller-provided buffer.
static esp_err_t fetch_manifest(char *out, size_t out_size)
{
    esp_http_client_config_t cfg = {
        .url = OTA_MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manifest open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status / 100 != 2) {
        ESP_LOGW(TAG, "manifest http status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    size_t total = 0;
    while (total + 1 < out_size) {
        int n = esp_http_client_read(client, out + total, out_size - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    out[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (total > 0) ? ESP_OK : ESP_FAIL;
}

static void perform_check_task(void *arg)
{
    (void)arg;

    if (s_check_in_progress) {
        ESP_LOGW(TAG, "check already in progress, skipping");
        vTaskDelete(NULL);
        return;
    }
    s_check_in_progress = true;

    set_status("checking manifest");

    char manifest[MANIFEST_MAX_LEN];
    memset(manifest, 0, sizeof(manifest));
    esp_err_t fetch_err = ESP_FAIL;
    for (int attempt = 0; attempt < MANIFEST_FETCH_RETRIES; ++attempt) {
        fetch_err = fetch_manifest(manifest, sizeof(manifest));
        if (fetch_err == ESP_OK) break;
        if (attempt + 1 < MANIFEST_FETCH_RETRIES) {
            int backoff = MANIFEST_BACKOFF_MS << attempt;
            ESP_LOGW(TAG, "manifest fetch attempt %d failed (%s), retrying in %dms",
                     attempt + 1, esp_err_to_name(fetch_err), backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
        }
    }
    if (fetch_err != ESP_OK) {
        set_status("manifest fetch failed");
        goto done;
    }

    cJSON *root = cJSON_Parse(manifest);
    if (!root) {
        set_status("manifest parse error");
        goto done;
    }
    cJSON *jver = cJSON_GetObjectItem(root, "version");
    cJSON *jurl = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(jver) || !cJSON_IsString(jurl) ||
        !jver->valuestring || !jurl->valuestring) {
        set_status("manifest missing version/url");
        cJSON_Delete(root);
        goto done;
    }

    const esp_app_desc_t *cur = esp_app_get_description();
    const char *cur_ver       = cur ? cur->version : "0.0.0";
    const char *latest_ver    = jver->valuestring;
    const char *download_url  = jurl->valuestring;

    ESP_LOGI(TAG, "=== OTA Manifest Check ===");
    ESP_LOGI(TAG, "Current version: %s", cur_ver);
    ESP_LOGI(TAG, "Latest version:  %s", latest_ver);
    ESP_LOGI(TAG, "Download URL: %s", download_url);

    if (version_cmp(latest_ver, cur_ver) <= 0) {
        ESP_LOGI(TAG, "Device is up to date");
        set_status("up to date (%s)", cur_ver);
        cJSON_Delete(root);
        goto done;
    }

    ESP_LOGI(TAG, "New version available! Waiting for user confirmation...");
    set_status("new version %s available", latest_ver);

    // Save pending update info
    strncpy(s_pending_update.version, latest_ver, sizeof(s_pending_update.version) - 1);
    strncpy(s_pending_update.url, download_url, sizeof(s_pending_update.url) - 1);
    s_pending_update.update_available = true;
    s_pending_update.user_confirmed = false;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Waiting for user confirmation to proceed with download...");

done:
    s_check_in_progress = false;
    vTaskDelete(NULL);
}

static void got_ip_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    if (!s_auto_enabled || s_did_initial_check) return;
    s_did_initial_check = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        perform_check_task, "ota_check", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn ota_check task (heap exhausted)");
        s_did_initial_check = false;
    }
}

// Perform the actual OTA download and update
static void perform_ota_download_task(void *arg)
{
    (void)arg;

    if (!s_pending_update.update_available || !s_pending_update.user_confirmed) {
        ESP_LOGW(TAG, "OTA download task called but no confirmed update pending");
        vTaskDelete(NULL);
        return;
    }

    set_status("downloading %s", s_pending_update.version);
    ESP_LOGI(TAG, "Starting OTA download from: %s", s_pending_update.url);

    esp_http_client_config_t ota_http = {
        .url = s_pending_update.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &ota_http,
    };

    ESP_LOGI(TAG, "=== Starting OTA Update ===");
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        set_status("OTA ok, restarting");
        ESP_LOGI(TAG, "OTA update successful! Restarting in 500ms...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        set_status("OTA failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
        s_pending_update.update_available = false;
        s_pending_update.user_confirmed = false;
    }

    vTaskDelete(NULL);
}

bool ota_updater_get_auto_enabled(void)
{
    return s_auto_enabled;
}

esp_err_t ota_updater_set_auto_enabled(bool enabled)
{
    s_auto_enabled = enabled;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, OTA_NVS_KEY_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t ota_updater_trigger_now(void)
{
    if (s_check_in_progress) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xTaskCreatePinnedToCore(
        perform_check_task, "ota_check_now", 4096, NULL, 5, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA check task (heap may be exhausted)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

const char *ota_updater_last_status(void)
{
    return s_last_status;
}

esp_err_t ota_updater_confirm_and_start(void)
{
    if (!s_pending_update.update_available) {
        ESP_LOGW(TAG, "No pending update available");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "User confirmed update to version %s", s_pending_update.version);
    s_pending_update.user_confirmed = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        perform_ota_download_task, "ota_download", 6144, NULL, 5, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA download task");
        s_pending_update.user_confirmed = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool ota_updater_has_pending_update(void)
{
    return s_pending_update.update_available && !s_pending_update.user_confirmed;
}

const char *ota_updater_pending_version(void)
{
    return s_pending_update.version;
}

const char *ota_updater_current_version(void)
{
    const esp_app_desc_t *cur = esp_app_get_description();
    return cur ? cur->version : "?";
}

// One-shot rollback timer callback. After the device has been up long enough
// (default 30s) without crashing, commit the new image. If we crash/reset
// before this fires, bootloader rolls back to the previous slot automatically.
static void rollback_confirm_cb(void *arg)
{
    (void)arg;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "marked image valid, cancel rollback: %s",
                 esp_err_to_name(err));
    }
}

esp_err_t ota_updater_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nvs, OTA_NVS_KEY_ENABLED, &v) == ESP_OK) {
            s_auto_enabled = (v != 0);
        }
        nvs_close(nvs);
    }
    esp_err_t err = esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event register failed: %s", esp_err_to_name(err));
    }

    // Arm rollback-confirmation timer. Only matters when we booted into a
    // freshly-applied OTA image (PENDING_VERIFY); on factory/already-confirmed
    // images mark_app_valid_cancel_rollback is a harmless no-op.
    esp_timer_create_args_t targs = {
        .callback = rollback_confirm_cb,
        .name = "ota_rollback",
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&targs, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)ROLLBACK_CONFIRM_MS * 1000);
    }

    const esp_app_desc_t *cur = esp_app_get_description();
    ESP_LOGI(TAG, "init: auto=%d, ver=%s",
             s_auto_enabled, cur ? cur->version : "?");
    return ESP_OK;
}

// --- HTTP handlers ----------------------------------------------------------
// Registered via ota_updater_register_http_handlers() so multiple httpds
// (LED control panel + Claude WiFi config page) can share the same routes.

static esp_err_t ota_http_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", ota_updater_current_version());
    cJSON_AddBoolToObject(root, "auto_update", ota_updater_get_auto_enabled());
    cJSON_AddStringToObject(root, "last_status", ota_updater_last_status());
    cJSON_AddBoolToObject(root, "check_in_progress", s_check_in_progress);

    // Add pending update info if available
    if (ota_updater_has_pending_update()) {
        cJSON *pending = cJSON_CreateObject();
        cJSON_AddStringToObject(pending, "version", ota_updater_pending_version());
        cJSON_AddStringToObject(pending, "url", s_pending_update.url);
        cJSON_AddBoolToObject(pending, "waiting_confirmation", true);
        cJSON_AddItemToObject(root, "pending_update", pending);
    }

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    cJSON_AddNumberToObject(root, "free_heap_bytes", free_heap);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) free(out);
    return ESP_OK;
}

static esp_err_t ota_http_enable_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int to_read = req->content_len;
    if (to_read <= 0 || to_read >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, buf, to_read);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(buf);
    cJSON *jen = root ? cJSON_GetObjectItem(root, "enabled") : NULL;
    if (!cJSON_IsBool(jen)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled bool required");
        return ESP_FAIL;
    }
    bool en = cJSON_IsTrue(jen);
    cJSON_Delete(root);
    esp_err_t err = ota_updater_set_auto_enabled(en);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"auto_update\":%s}",
                 en ? "true" : "false");
        httpd_resp_sendstr(req, resp);
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"nvs write failed\"}");
    }
    return ESP_OK;
}

static esp_err_t ota_http_check_handler(httpd_req_t *req)
{
    esp_err_t err = ota_updater_trigger_now();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"check started\"}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"already in progress\"}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"failed to start\"}");
    }
    return ESP_OK;
}

static esp_err_t ota_http_confirm_handler(httpd_req_t *req)
{
    if (!ota_updater_has_pending_update()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"no pending update\"}");
        return ESP_OK;
    }

    esp_err_t err = ota_updater_confirm_and_start();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "message", "Update started");
        cJSON_AddStringToObject(resp, "version", ota_updater_pending_version());
        char *out = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        httpd_resp_sendstr(req, out ? out : "{}");
        if (out) free(out);
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"message\":\"failed to start download\"}");
    }
    return ESP_OK;
}

esp_err_t ota_updater_register_http_handlers(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    static const httpd_uri_t u_status = {
        .uri = "/api/ota/status",  .method = HTTP_GET,
        .handler = ota_http_status_handler,
    };
    static const httpd_uri_t u_enable = {
        .uri = "/api/ota/enable",  .method = HTTP_POST,
        .handler = ota_http_enable_handler,
    };
    static const httpd_uri_t u_check  = {
        .uri = "/api/ota/check_now", .method = HTTP_POST,
        .handler = ota_http_check_handler,
    };
    static const httpd_uri_t u_confirm = {
        .uri = "/api/ota/confirm", .method = HTTP_POST,
        .handler = ota_http_confirm_handler,
    };
    esp_err_t e1 = httpd_register_uri_handler(server, &u_status);
    esp_err_t e2 = httpd_register_uri_handler(server, &u_enable);
    esp_err_t e3 = httpd_register_uri_handler(server, &u_check);
    esp_err_t e4 = httpd_register_uri_handler(server, &u_confirm);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK) {
        ESP_LOGW(TAG, "register http: status=%d enable=%d check=%d confirm=%d", e1, e2, e3, e4);
        return ESP_FAIL;
    }
    return ESP_OK;
}
