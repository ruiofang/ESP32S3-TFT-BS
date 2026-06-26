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
#include <stdlib.h>
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
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <time.h>

#include "cJSON.h"

#define TAG "ota_updater"

#define OTA_NVS_NAMESPACE   "ota"
#define OTA_NVS_KEY_ENABLED "auto"
#define OTA_NVS_KEY_PROGRESS "progress"
#define OTA_NVS_KEY_URL      "url"
#define OTA_NVS_KEY_VERSION  "version"
#define OTA_NVS_KEY_SERIAL   "serial"

// 安全校验配置
#define OTA_SIGNATURE_HEADER "X-Firmware-Signature"
#define OTA_HASH_HEADER      "X-Firmware-Hash"
#define OTA_PUBLIC_KEY_SIZE  64
#define OTA_SIGNATURE_SIZE   64
#define OTA_HASH_SIZE        32

// 固件验证模式
typedef enum {
    OTA_VERIFY_NONE = 0,      // 不验证
    OTA_VERIFY_HASH,          // 仅验证哈希
    OTA_VERIFY_SIGNATURE,     // 验证签名
    OTA_VERIFY_HMAC          // HMAC验证
} ota_verify_mode_t;

// Manifest URL configuration
// Primary URL (GitHub) and backup URL (Gitee) for OTA manifest.
// HTTPS requires the device time to be valid for certificate verification.

#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL \
    "https://github.com/ruiofang/ESP32S3-TFT-BS/releases/download/latest/latest.json"
#endif

#ifndef OTA_MANIFEST_URL_BACKUP
#define OTA_MANIFEST_URL_BACKUP \
    "https://gitee.com/ruiofang/ESP32S3-TFT-BS/releases/download/latest/latest.json"
#endif

#define MANIFEST_MAX_LEN       1024
#define MANIFEST_FETCH_RETRIES 3
#define MANIFEST_BACKOFF_MS    1500   // doubles per attempt: 1.5s, 3s, 6s
#define ROLLBACK_CONFIRM_MS    30000  // 30s after init -> mark image valid
#define MIN_VALID_UNIX_TIME    1577836800
#define SNTP_SYNC_TIMEOUT_MS   15000
#define SNTP_POLL_INTERVAL_MS  500

#ifndef OTA_SNTP_SERVER
#define OTA_SNTP_SERVER "pool.ntp.org"
#endif

#define OTA_FALLBACK_DNS_MAIN   "8.8.8.8"
#define OTA_FALLBACK_DNS_BACKUP "1.1.1.1"

static volatile bool s_auto_enabled       = false;
static volatile bool s_check_in_progress  = false;
static volatile bool s_did_initial_check  = false;
static char          s_last_status[128]   = "idle";
static bool          s_sntp_started       = false;

// Long-lived OTA worker: spawned once at init so triggers don't need to
// allocate an 8KB task stack from internal heap at runtime (which fails
// once LVGL/WiFi/BLE have warmed up). Callers signal work via xTaskNotify.
static TaskHandle_t  s_ota_worker         = NULL;
#define OTA_NOTIFY_CHECK    (1U << 0)
#define OTA_NOTIFY_DOWNLOAD (1U << 1)

// OTA更新发现的新版本信息
typedef struct {
    char version[32];
    char url[256];
    bool update_available;
    bool user_confirmed;
} ota_pending_update_t;
static ota_pending_update_t s_pending_update = {0};

// 断点续传信息
typedef struct {
    char url[256];
    char version[32];
    size_t downloaded_bytes;
    size_t total_bytes;
    time_t last_update;
} ota_resume_info_t;

// OTA下载状态
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_PAUSED,
    OTA_STATE_ERROR,
    OTA_STATE_COMPLETED
} ota_state_t;
static volatile ota_state_t s_ota_state = OTA_STATE_IDLE;

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_status, sizeof(s_last_status), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "status: %s", s_last_status);
}

static bool is_system_time_valid(void)
{
    return time(NULL) >= MIN_VALID_UNIX_TIME;
}

static void start_sntp_if_needed(void)
{
    if (s_sntp_started) return;

    // China Standard Time (UTC+8). Affects localtime() output only;
    // SSL cert validation uses absolute UTC and is unaffected.
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, OTA_SNTP_SERVER);
    esp_sntp_init();
    s_sntp_started = true;

    ESP_LOGI(TAG, "SNTP started with server: %s", OTA_SNTP_SERVER);
}

static esp_err_t wait_for_system_time_sync(uint32_t timeout_ms)
{
    if (is_system_time_valid()) {
        return ESP_OK;
    }

    start_sntp_if_needed();
    ESP_LOGI(TAG, "Waiting for SNTP time sync (timeout: %u ms)...", timeout_ms);

    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms) {
        if (is_system_time_valid()) {
            ESP_LOGI(TAG, "SNTP time sync completed after %u ms", waited_ms);
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(SNTP_POLL_INTERVAL_MS));
        waited_ms += SNTP_POLL_INTERVAL_MS;
    }

    ESP_LOGW(TAG, "SNTP time sync timed out after %u ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

static bool dns_info_is_valid(const esp_netif_dns_info_t *dns)
{
    return dns && dns->ip.type == IPADDR_TYPE_V4 && dns->ip.u_addr.ip4.addr != 0;
}

static void log_sta_dns_servers(esp_netif_t *netif)
{
    if (!netif) return;

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns_info_is_valid(&dns)) {
        ESP_LOGI(TAG, "STA DNS main: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK && dns_info_is_valid(&dns)) {
        ESP_LOGI(TAG, "STA DNS backup: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &dns) == ESP_OK && dns_info_is_valid(&dns)) {
        ESP_LOGI(TAG, "STA DNS fallback: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
}

static void ensure_sta_dns_servers(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGW(TAG, "STA netif not found, skipping DNS fallback setup");
        return;
    }

    esp_netif_dns_info_t dns_main = {0};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK &&
        dns_info_is_valid(&dns_main)) {
        log_sta_dns_servers(netif);
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = IPADDR_TYPE_V4;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_str_to_ip4(OTA_FALLBACK_DNS_MAIN, &dns.ip.u_addr.ip4));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_str_to_ip4(OTA_FALLBACK_DNS_BACKUP, &dns.ip.u_addr.ip4));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns));

    ESP_LOGW(TAG, "STA DNS server missing, applied fallback DNS: %s / %s",
             OTA_FALLBACK_DNS_MAIN, OTA_FALLBACK_DNS_BACKUP);
    log_sta_dns_servers(netif);
}



// 获取设备序列号（简化版本，使用随机数）
 static esp_err_t get_device_serial_number(char *serial_out, size_t serial_size)
 {
     if (!serial_out || serial_size < 9) return ESP_ERR_INVALID_ARG;
     
     memset(serial_out, 0, serial_size);
     
     // 使用简单的时间戳和随机数作为序列号（简化实现）
     time_t now = time(NULL);
     uint32_t random_val = esp_random() & 0xFFFFFF;
     
     // 生成简单的序列号
     snprintf(serial_out, serial_size, "%08lx%06lx", 
              (unsigned long)now, (unsigned long)random_val);
     
     ESP_LOGI(TAG, "Device serial number (generated): %s", serial_out);
     return ESP_OK;
 }

// 保存序列号到NVS
static esp_err_t save_serial_number(const char *serial)
{
    if (!serial) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    
    err = nvs_set_str(nvs, OTA_NVS_KEY_SERIAL, serial);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    
    nvs_close(nvs);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved serial number to NVS: %s", serial);
    }
    
    return err;
}

// 从NVS加载序列号
static esp_err_t load_serial_number(char *serial_out, size_t serial_size)
{
    if (!serial_out || serial_size < 65) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    
    size_t len = serial_size;
    err = nvs_get_str(nvs, OTA_NVS_KEY_SERIAL, serial_out, &len);
    
    nvs_close(nvs);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded serial number from NVS: %s", serial_out);
    }
    
    return err;
}

// 保存断点续传信息到NVS
static esp_err_t save_resume_info(const ota_resume_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    
    // 保存URL
    err = nvs_set_str(nvs, OTA_NVS_KEY_URL, info->url);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    // 保存版本
    err = nvs_set_str(nvs, OTA_NVS_KEY_VERSION, info->version);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    // 保存下载进度
    err = nvs_set_u32(nvs, OTA_NVS_KEY_PROGRESS, (uint32_t)info->downloaded_bytes);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    err = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved resume info: url=%s, version=%s, downloaded=%d bytes",
                 info->url, info->version, info->downloaded_bytes);
    }
    
    return err;
}

// 从NVS加载断点续传信息
static esp_err_t load_resume_info(ota_resume_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    
    memset(info, 0, sizeof(ota_resume_info_t));
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    
    // 加载URL
    size_t url_len = sizeof(info->url);
    err = nvs_get_str(nvs, OTA_NVS_KEY_URL, info->url, &url_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    // 加载版本
    size_t version_len = sizeof(info->version);
    err = nvs_get_str(nvs, OTA_NVS_KEY_VERSION, info->version, &version_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    // 加载下载进度
    uint32_t progress = 0;
    err = nvs_get_u32(nvs, OTA_NVS_KEY_PROGRESS, &progress);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    info->downloaded_bytes = (size_t)progress;
    
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Loaded resume info: url=%s, version=%s, downloaded=%d bytes",
             info->url, info->version, info->downloaded_bytes);
    
    return ESP_OK;
}

// 清除断点续传信息
static esp_err_t clear_resume_info(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    
    err = nvs_erase_key(nvs, OTA_NVS_KEY_URL);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    err = nvs_erase_key(nvs, OTA_NVS_KEY_VERSION);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    err = nvs_erase_key(nvs, OTA_NVS_KEY_PROGRESS);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    
    err = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Cleared resume info");
    }
    
    return err;
}

// Check system time and log it
static void log_system_time(void)
{
    time_t now = time(NULL);
    struct tm timeinfo = *localtime(&now);

    // Check if time is reasonable (after 2020)
    if (now < MIN_VALID_UNIX_TIME) {
        ESP_LOGW(TAG, "⚠️  System time appears to be incorrect!");
        ESP_LOGW(TAG, "   Current time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        ESP_LOGW(TAG, "   SSL certificate verification may fail!");
        ESP_LOGW(TAG, "   Please sync device time (e.g., via SNTP)");
    } else {
        ESP_LOGI(TAG, "System time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
}

// 改进的版本比较函数，支持更灵活的版本格式
static int version_cmp(const char *a, const char *b)
{
    if (!a || !b) return 0;
    
    // 支持 x.y.z 格式
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    int a_count = sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    int b_count = sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
    
    // 如果版本格式不同，使用字符串比较
    if (a_count != b_count) {
        return strcmp(a, b);
    }
    
    // 比较主版本号
    if (a1 != b1) return a1 - b1;
    // 比较次版本号
    if (a2 != b2) return a2 - b2;
    // 比较修订版本号
    return a3 - b3;
}

// 检查是否为有效版本号格式
static bool is_valid_version_format(const char *version)
{
    if (!version || strlen(version) == 0) return false;
    
    // 检查是否包含至少一个数字
    for (int i = 0; version[i]; i++) {
        if (version[i] >= '0' && version[i] <= '9') {
            return true;
        }
    }
    return false;
}

// Fetch the manifest JSON from a specific URL
static esp_err_t fetch_manifest_from_url(const char *url, char *out, size_t out_size)
{
    ESP_LOGI(TAG, "Fetching manifest from: %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,  // Increased timeout from 10s to 20s
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client (out of memory)");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Connecting to manifest server (timeout: 20000ms)...");

    // Manual redirect loop. With the open/fetch_headers/read flow the client
    // does NOT auto-follow redirects (that only happens inside _perform()).
    // GitHub Releases /latest/download/... 302s to objects.githubusercontent.com,
    // so we must call esp_http_client_set_redirection() ourselves.
    esp_err_t err;
    int status = 0;
    const int MAX_REDIRECTS = 5;
    for (int hop = 0; ; hop++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "❌ Failed to connect to manifest server: %s (0x%x)", esp_err_to_name(err), err);
            if (err == ESP_ERR_INVALID_ARG) {
                ESP_LOGE(TAG, "  -> Invalid argument (bad URL or config)");
            } else if (err == ESP_ERR_NO_MEM) {
                ESP_LOGE(TAG, "  -> Out of memory");
            } else if (err == ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "  -> Connection timeout (check network connectivity)");
            } else if (err == ESP_FAIL || err == 0x7002) {
                ESP_LOGE(TAG, "  -> Connection failure");
                ESP_LOGW(TAG, "  HTTPS SSL/TLS errors often caused by:");
                ESP_LOGW(TAG, "    1. Incorrect system time (most common!)");
                ESP_LOGW(TAG, "       -> Check if device time is in reasonable range (after 2020)");
                ESP_LOGW(TAG, "       -> Set time via SNTP/NTP if available");
                ESP_LOGW(TAG, "    2. Network connectivity issues (DNS, firewall)");
                ESP_LOGW(TAG, "    3. Certificate bundle problem");
            }
            esp_http_client_cleanup(client);
            return err;
        }

        ESP_LOGI(TAG, "Connected! Fetching headers...");
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Response Status: %d (hop %d)", status, hop);

        // Follow 301/302/303/307/308 by asking the client to re-target the URL.
        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308) && hop < MAX_REDIRECTS) {
            ESP_LOGI(TAG, "Following redirect (hop %d/%d)", hop + 1, MAX_REDIRECTS);
            esp_http_client_close(client);
            esp_err_t rerr = esp_http_client_set_redirection(client);
            if (rerr != ESP_OK) {
                ESP_LOGE(TAG, "set_redirection failed: %s", esp_err_to_name(rerr));
                esp_http_client_cleanup(client);
                return rerr;
            }
            continue;
        }
        break;
    }

    if (status / 100 != 2) {
        ESP_LOGE(TAG, "Manifest fetch failed with HTTP %d (expected 2xx)", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "Content-Length: %d bytes", content_length);

    ESP_LOGI(TAG, "Reading manifest data...");
    size_t total = 0;
    while (total + 1 < out_size) {
        int n = esp_http_client_read(client, out + total, out_size - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    out[total] = '\0';

    ESP_LOGI(TAG, "Successfully read %d bytes of manifest", total);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total > 0) {
        ESP_LOGI(TAG, "Manifest content (first 200 chars): %.200s", out);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "No data received from manifest URL");
        return ESP_FAIL;
    }
}

// Fetch manifest with fallback to backup URL
static esp_err_t fetch_manifest(char *out, size_t out_size)
{
    esp_err_t err;

    // Try primary URL
    ESP_LOGI(TAG, "=== OTA Manifest Fetch ===");
    ESP_LOGI(TAG, "Primary URL:  %s", OTA_MANIFEST_URL);
    ESP_LOGI(TAG, "Backup URL:   %s", OTA_MANIFEST_URL_BACKUP);

    err = fetch_manifest_from_url(OTA_MANIFEST_URL, out, out_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ Primary URL successful");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Primary URL failed, trying backup...");

    // Try backup URL
    err = fetch_manifest_from_url(OTA_MANIFEST_URL_BACKUP, out, out_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ Backup URL successful (Gitee)");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Both primary and backup URLs failed");
    return ESP_FAIL;
}

static void do_perform_check(void)
{
    if (s_check_in_progress) {
        ESP_LOGW(TAG, "check already in progress, skipping");
        return;
    }
    s_check_in_progress = true;

    // Log system info before attempting OTA
    ESP_LOGI(TAG, "=== OTA System Check ===");
    log_system_time();
    if (!is_system_time_valid()) {
        set_status("syncing system time");
        esp_err_t time_err = wait_for_system_time_sync(SNTP_SYNC_TIMEOUT_MS);
        log_system_time();
        if (time_err != ESP_OK) {
            ESP_LOGW(TAG, "System time is still invalid, HTTPS OTA may fail");
        }
    }
    // Breakdown matters: esp-aes alloc failures are about INTERNAL RAM,
    // not total free heap (which is dominated by PSRAM on S3).
    ESP_LOGI(TAG, "Heap: total=%u internal=%u psram=%u min_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    set_status("checking manifest");

    char *manifest = heap_caps_malloc(MANIFEST_MAX_LEN, MALLOC_CAP_DEFAULT);
    if (!manifest) {
        ESP_LOGE(TAG, "Failed to allocate memory for manifest");
        set_status("memory allocation failed");
        goto done;
    }
    memset(manifest, 0, MANIFEST_MAX_LEN);
    esp_err_t fetch_err = ESP_FAIL;

    for (int attempt = 0; attempt < MANIFEST_FETCH_RETRIES; ++attempt) {
        ESP_LOGI(TAG, "--- Manifest fetch attempt %d/%d ---", attempt + 1, MANIFEST_FETCH_RETRIES);
        fetch_err = fetch_manifest(manifest, MANIFEST_MAX_LEN);
        if (fetch_err == ESP_OK) {
            ESP_LOGI(TAG, "Manifest fetch successful!");
            break;
        }
        if (attempt + 1 < MANIFEST_FETCH_RETRIES) {
            int backoff = MANIFEST_BACKOFF_MS << attempt;
            ESP_LOGW(TAG, "Manifest fetch failed, retrying in %dms...", backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
        }
    }

    if (fetch_err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to fetch manifest after %d attempts", MANIFEST_FETCH_RETRIES);
        ESP_LOGE(TAG, "Tried both URLs:");
        ESP_LOGE(TAG, "  • Primary (GitHub): %s", OTA_MANIFEST_URL);
        ESP_LOGE(TAG, "  • Backup (Gitee):   %s", OTA_MANIFEST_URL_BACKUP);
        ESP_LOGE(TAG, "Please check:");
        ESP_LOGE(TAG, "  1. WiFi network connectivity");
        ESP_LOGE(TAG, "  2. DNS resolution (can device reach 8.8.8.8?)");
        ESP_LOGE(TAG, "  3. GitHub and/or Gitee are accessible from your network");
        ESP_LOGE(TAG, "  4. Device time is set correctly (for SSL verification)");
        set_status("manifest fetch failed");
        heap_caps_free(manifest);
        goto done;
    }

    cJSON *root = cJSON_Parse(manifest);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse manifest JSON");
        set_status("manifest parse error");
        heap_caps_free(manifest);
        goto done;
    }
    
    cJSON *jver = cJSON_GetObjectItem(root, "version");
    cJSON *jurl = cJSON_GetObjectItem(root, "url");
    
    // 验证manifest格式
    if (!cJSON_IsString(jver) || !cJSON_IsString(jurl) ||
        !jver->valuestring || !jurl->valuestring) {
        ESP_LOGE(TAG, "Manifest missing required fields: version or url");
        set_status("manifest missing version/url");
        cJSON_Delete(root);
        heap_caps_free(manifest);
        goto done;
    }
    
    // 验证版本号格式
    if (!is_valid_version_format(jver->valuestring)) {
        ESP_LOGE(TAG, "Invalid version format: %s", jver->valuestring);
        set_status("invalid version format");
        cJSON_Delete(root);
        heap_caps_free(manifest);
        goto done;
    }
    
    // 验证URL格式
    if (strlen(jurl->valuestring) == 0 || 
        (strncmp(jurl->valuestring, "http://", 7) != 0 && 
         strncmp(jurl->valuestring, "https://", 8) != 0)) {
        ESP_LOGE(TAG, "Invalid URL format: %s", jurl->valuestring);
        set_status("invalid URL format");
        cJSON_Delete(root);
        heap_caps_free(manifest);
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

    // 检查当前版本是否有效
    if (!is_valid_version_format(cur_ver)) {
        ESP_LOGW(TAG, "Current version format is invalid, proceeding with update check");
    }
    
    // 检查是否需要更新
    int cmp_result = version_cmp(latest_ver, cur_ver);
    if (cmp_result <= 0) {
        ESP_LOGI(TAG, "Device is up to date (version comparison: %d)", cmp_result);
        set_status("up to date (%s)", cur_ver);
        cJSON_Delete(root);
        heap_caps_free(manifest);
        goto done;
    }
    
    ESP_LOGI(TAG, "New version available! Version comparison: %d", cmp_result);

    ESP_LOGI(TAG, "New version available! Waiting for user confirmation...");
    set_status("new version %s available", latest_ver);

    // Save pending update info
    strncpy(s_pending_update.version, latest_ver, sizeof(s_pending_update.version) - 1);
    strncpy(s_pending_update.url, download_url, sizeof(s_pending_update.url) - 1);
    s_pending_update.update_available = true;
    s_pending_update.user_confirmed = false;

    cJSON_Delete(root);
    heap_caps_free(manifest);
    ESP_LOGI(TAG, "Waiting for user confirmation to proceed with download...");

done:
    s_check_in_progress = false;
}

static void got_ip_event_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    ensure_sta_dns_servers();
    if (!s_auto_enabled || s_did_initial_check) return;
    if (!s_ota_worker) {
        ESP_LOGW(TAG, "OTA worker not ready, skipping initial check");
        return;
    }
    s_did_initial_check = true;
    xTaskNotify(s_ota_worker, OTA_NOTIFY_CHECK, eSetBits);
}

// Perform the actual OTA download and update with retry and resume support
static void do_perform_download(void)
{
    if (!s_pending_update.update_available || !s_pending_update.user_confirmed) {
        ESP_LOGW(TAG, "OTA download task called but no confirmed update pending");
        return;
    }

    s_ota_state = OTA_STATE_DOWNLOADING;
    set_status("downloading %s", s_pending_update.version);
    ESP_LOGI(TAG, "Starting OTA download from: %s", s_pending_update.url);

    // 检查是否有可恢复的下载
    ota_resume_info_t resume_info;
    if (load_resume_info(&resume_info) == ESP_OK) {
        if (strcmp(resume_info.url, s_pending_update.url) == 0 &&
            strcmp(resume_info.version, s_pending_update.version) == 0 &&
            resume_info.downloaded_bytes > 0) {
            ESP_LOGI(TAG, "Found resume point: %d bytes downloaded", resume_info.downloaded_bytes);
            // TODO: 实现断点续传功能
        }
    }

    esp_http_client_config_t ota_http = {
        .url = s_pending_update.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,  // 增加超时时间
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
        .max_redirection_count = 3,
        .buffer_size = 2048,  // small HTTP rx buffer; OTA body lives in PSRAM via buffer_caps
    };

    // buffer_caps: route the OTA scratch buffer to PSRAM so internal SRAM stays
    //   free for esp-aes DMA descriptors (root cause of the prior alloc failure).
    //   (esp_https_ota runs synchronously in the OTA worker task, which already
    //   has an 8 KB stack from ota_updater_init.)
    esp_https_ota_config_t ota_cfg = {
        .http_config = &ota_http,
        .buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };

    ESP_LOGI(TAG, "=== Starting OTA Update ===");
    
    // 重试机制
    const int max_retries = 3;
    int retry_count = 0;
    esp_err_t err = ESP_FAIL;
    
    while (retry_count < max_retries) {
        if (retry_count > 0) {
            ESP_LOGW(TAG, "Retry attempt %d/%d after 2 second delay", 
                     retry_count, max_retries);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        
        err = esp_https_ota(&ota_cfg);
        
        if (err == ESP_OK) {
            break;
        } else if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "OTA validation failed - firmware corrupted or invalid");
            set_status("validation failed");
            break;  // 验证失败不需要重试
        } else if (err == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "Out of memory during OTA");
            set_status("out of memory");
            break;  // 内存不足不需要重试
        } else {
            ESP_LOGE(TAG, "OTA update failed (attempt %d): %s", 
                     retry_count + 1, esp_err_to_name(err));
            set_status("failed: %s (retry %d)", esp_err_to_name(err), retry_count + 1);
            retry_count++;
        }
    }

    if (err == ESP_OK) {
        set_status("OTA ok, restarting");
        ESP_LOGI(TAG, "OTA update successful! Restarting in 500ms...");
        
        // 清除断点续传信息
        clear_resume_info();
        
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        set_status("OTA failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "OTA update failed after %d attempts: %s", 
                 retry_count, esp_err_to_name(err));
        
        // 保存当前下载进度以便恢复
        if (s_ota_state == OTA_STATE_DOWNLOADING) {
            // 注意：esp_https_ota内部不提供进度信息，这里需要扩展
            // 暂时保存基本信息
            ota_resume_info_t info = {
                .downloaded_bytes = 0,  // 需要实际实现获取进度
            };
            strncpy(info.url, s_pending_update.url, sizeof(info.url) - 1);
            strncpy(info.version, s_pending_update.version, sizeof(info.version) - 1);
            info.last_update = time(NULL);
            
            save_resume_info(&info);
            ESP_LOGW(TAG, "Saved resume info for recovery");
        }
        
        s_pending_update.update_available = false;
        s_pending_update.user_confirmed = false;
        s_ota_state = OTA_STATE_ERROR;
    }
}

// Long-lived OTA worker. Created once at init_time and never exits.
// Waits on task notifications: bit 0 = check manifest, bit 1 = download.
// Both bits in one notification are handled in sequence in this loop turn.
static void ota_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, ULONG_MAX, &bits, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (bits & OTA_NOTIFY_CHECK) {
            do_perform_check();
        }
        if (bits & OTA_NOTIFY_DOWNLOAD) {
            do_perform_download();
        }
    }
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
    if (!s_ota_worker) {
        ESP_LOGE(TAG, "OTA worker not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_ota_worker, OTA_NOTIFY_CHECK, eSetBits);
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

    if (!s_ota_worker) {
        ESP_LOGE(TAG, "OTA worker not initialized");
        s_pending_update.user_confirmed = false;
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_ota_worker, OTA_NOTIFY_DOWNLOAD, eSetBits);
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
    
    ESP_LOGI(TAG, "Rollback confirmation timer fired");
    
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return;
    }
    
    ESP_LOGI(TAG, "Running partition: %s (type: %d, subtype: %d)", 
             running->label, running->type, running->subtype);
    
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get partition state: %s", esp_err_to_name(err));
        return;
    }
    
    ESP_LOGI(TAG, "Partition state: %d", state);
    
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking OTA image as valid (cancelling rollback)");
        err = esp_ota_mark_app_valid_cancel_rollback();
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Successfully marked image as valid");
            
            // 清除断点续传信息（如果存在）
            clear_resume_info();
            
            // 记录成功升级
            ESP_LOGI(TAG, "OTA upgrade completed successfully");
        } else {
            ESP_LOGE(TAG, "Failed to mark image as valid: %s", esp_err_to_name(err));
            
            // 如果标记失败，可能需要手动触发回滚
            ESP_LOGW(TAG, "Consider manual intervention if device is unstable");
        }
    } else if (state == ESP_OTA_IMG_VALID) {
        ESP_LOGI(TAG, "Image already marked as valid");
    } else if (state == ESP_OTA_IMG_INVALID) {
        ESP_LOGE(TAG, "Image is marked as INVALID - bootloader will roll back");
        
        // 记录无效状态，可能需要用户干预
        set_status("image invalid - rollback expected");
    } else if (state == ESP_OTA_IMG_ABORTED) {
        ESP_LOGW(TAG, "Image state is ABORTED - previous OTA was interrupted");
        
        // 清除旧的断点续传信息
        clear_resume_info();
    } else {
        ESP_LOGW(TAG, "Unknown partition state: %d", state);
    }
    
    // 检查是否有其他需要清理的状态
    if (s_ota_state == OTA_STATE_DOWNLOADING || s_ota_state == OTA_STATE_PAUSED) {
        ESP_LOGW(TAG, "Resetting OTA state from %d to IDLE", s_ota_state);
        s_ota_state = OTA_STATE_IDLE;
    }
}

esp_err_t ota_updater_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA updater...");

    // 初始化NVS并加载配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(nvs, OTA_NVS_KEY_ENABLED, &v) == ESP_OK) {
            s_auto_enabled = (v != 0);
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
    }

    // Spawn the long-lived worker BEFORE registering the got_ip handler so
    // the handler can always notify it. Done early while internal heap is
    // still fresh — runtime task creation was failing once LVGL/WiFi/BLE
    // had warmed up.
    BaseType_t ok = xTaskCreatePinnedToCore(
        ota_worker_task, "ota_worker", 8192, NULL, 5, &s_ota_worker, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA worker task");
        s_ota_worker = NULL;
        return ESP_ERR_NO_MEM;
    }

    // 注册WiFi事件处理器
    err = esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_handler, NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "IP event loop not ready yet, auto OTA check will wait for manual trigger");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
        return err;
    }
    
    // 尝试获取设备序列号
    char serial_number[65] = {0};
    if (get_device_serial_number(serial_number, sizeof(serial_number)) == ESP_OK) {
        ESP_LOGI(TAG, "Device has serial number: %s", serial_number);
        save_serial_number(serial_number);
    } else {
        ESP_LOGW(TAG, "Device does not have a serial number");
    }
    
    // 设置rollback确认定时器
    esp_timer_create_args_t targs = {
        .callback = rollback_confirm_cb,
        .name = "ota_rollback",
    };
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&targs, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)ROLLBACK_CONFIRM_MS * 1000);
        ESP_LOGI(TAG, "Rollback confirmation timer set for %d ms", ROLLBACK_CONFIRM_MS);
    } else {
        ESP_LOGW(TAG, "Failed to create rollback timer");
    }
    
    // 检查是否有未完成的下载
    ota_resume_info_t resume_info;
    if (load_resume_info(&resume_info) == ESP_OK) {
        ESP_LOGW(TAG, "Found incomplete download from previous session:");
        ESP_LOGW(TAG, "  URL: %s", resume_info.url);
        ESP_LOGW(TAG, "  Version: %s", resume_info.version);
        ESP_LOGW(TAG, "  Downloaded: %d bytes", resume_info.downloaded_bytes);
        
        // 可以在这里提示用户是否继续下载
        set_status("incomplete download found");
    }
    
    const esp_app_desc_t *cur = esp_app_get_description();
    ESP_LOGI(TAG, "OTA updater initialized successfully");
    ESP_LOGI(TAG, "  Auto-update: %s", s_auto_enabled ? "enabled" : "disabled");
    ESP_LOGI(TAG, "  Current version: %s", cur ? cur->version : "unknown");
    ESP_LOGI(TAG, "  Free heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    
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

// Browser-friendly page that lets the user pick a .bin and POST it raw.
// Kept self-contained (inline CSS/JS) so it works on any httpd without
// extra static-file plumbing.
static esp_err_t ota_http_upload_page_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>OTA Upload</title><style>"
        "body{font-family:sans-serif;max-width:560px;margin:40px auto;padding:0 16px;color:#222}"
        "h1{font-size:1.4em}input,button{padding:10px;margin:6px 0;width:100%;box-sizing:border-box;font-size:1em}"
        "button{background:#0066cc;color:#fff;border:0;border-radius:4px;cursor:pointer}"
        "button:disabled{background:#999}#log{background:#f4f4f4;padding:12px;margin-top:10px;"
        "white-space:pre-wrap;font-family:monospace;font-size:.9em;border-radius:4px;min-height:1em}"
        "progress{width:100%;height:22px;margin-top:6px}</style></head><body>"
        "<h1>固件升级 / Firmware Upload</h1>"
        "<p>选择编译产物 <code>build/panda.bin</code>，上传后设备会自动校验并重启。</p>"
        "<input type=\"file\" id=\"f\" accept=\".bin\"/>"
        "<button id=\"btn\" onclick=\"up()\">上传 Upload</button>"
        "<progress id=\"p\" max=\"100\" value=\"0\"></progress>"
        "<div id=\"log\">就绪 ready</div><script>"
        "function up(){const f=document.getElementById('f').files[0];"
        "if(!f){alert('请选择 .bin 文件');return}"
        "const log=document.getElementById('log'),p=document.getElementById('p'),b=document.getElementById('btn');"
        "b.disabled=true;log.textContent='上传中: '+f.name+' ('+f.size+' bytes)';"
        "const x=new XMLHttpRequest();x.open('POST','/api/ota/upload');"
        "x.setRequestHeader('Content-Type','application/octet-stream');"
        "x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded*100/e.total};"
        "x.onload=()=>{log.textContent=x.status+': '+x.responseText;b.disabled=false};"
        "x.onerror=()=>{log.textContent='传输错误';b.disabled=false};"
        "x.send(f)}</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, sizeof(page) - 1);
}

// Receives raw firmware bytes (application/octet-stream) and streams them
// into the next OTA partition. Validates the first byte is the ESP image
// magic (0xE9) before committing flash writes.
static esp_err_t ota_http_upload_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_set_status(req, "411 Length Required");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"content-length required\"}");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"no OTA partition\"}");
    }
    if ((size_t)total > part->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"firmware exceeds partition\"}");
    }

    ESP_LOGI(TAG, "Upload OTA: %d bytes -> partition %s @ 0x%lx",
             total, part->label, part->address);
    set_status("uploading %d bytes", total);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"esp_ota_begin failed\"}");
    }

    // 4 KB chunk: matches httpd's typical recv granularity and keeps the
    // buffer in PSRAM if available, so internal heap stays free.
    const int CHUNK = 4096;
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT);
    if (!buf) {
        esp_ota_abort(handle);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"alloc failed\"}");
    }

    int remaining = total;
    bool header_checked = false;
    while (remaining > 0) {
        int want = remaining > CHUNK ? CHUNK : remaining;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed at %d/%d", total - remaining, total);
            esp_ota_abort(handle);
            free(buf);
            set_status("upload recv failed");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"recv failed\"}");
        }
        if (!header_checked) {
            // ESP_IMAGE_HEADER_MAGIC == 0xE9 (first byte of any IDF app image).
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "rejected: first byte 0x%02x != 0xE9", (uint8_t)buf[0]);
                esp_ota_abort(handle);
                free(buf);
                set_status("upload rejected: not an app image");
                httpd_resp_set_status(req, "400 Bad Request");
                return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"not a valid app image (magic mismatch)\"}");
            }
            header_checked = true;
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            set_status("upload write failed");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"esp_ota_write failed\"}");
        }
        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        set_status("upload validation failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req,
            err == ESP_ERR_OTA_VALIDATE_FAILED
            ? "{\"ok\":false,\"err\":\"image validation failed\"}"
            : "{\"ok\":false,\"err\":\"esp_ota_end failed\"}");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        set_status("set boot failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"set_boot failed\"}");
    }

    ESP_LOGI(TAG, "Upload OTA complete, rebooting in 1s");
    set_status("upload ok, restarting");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"upload complete, rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK; // unreachable
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
    static const httpd_uri_t u_upload_page = {
        .uri = "/ota/upload",       .method = HTTP_GET,
        .handler = ota_http_upload_page_handler,
    };
    static const httpd_uri_t u_upload = {
        .uri = "/api/ota/upload",   .method = HTTP_POST,
        .handler = ota_http_upload_handler,
    };
    esp_err_t e1 = httpd_register_uri_handler(server, &u_status);
    esp_err_t e2 = httpd_register_uri_handler(server, &u_enable);
    esp_err_t e3 = httpd_register_uri_handler(server, &u_check);
    esp_err_t e4 = httpd_register_uri_handler(server, &u_confirm);
    esp_err_t e5 = httpd_register_uri_handler(server, &u_upload_page);
    esp_err_t e6 = httpd_register_uri_handler(server, &u_upload);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK ||
        e4 != ESP_OK || e5 != ESP_OK || e6 != ESP_OK) {
        ESP_LOGW(TAG, "register http: status=%d enable=%d check=%d confirm=%d upload_page=%d upload=%d",
                 e1, e2, e3, e4, e5, e6);
        return ESP_FAIL;
    }
    return ESP_OK;
}
