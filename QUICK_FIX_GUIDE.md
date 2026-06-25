# OTA 快速修复指南 (Quick Fix)

## 🔴 你的问题

```
E (18367) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
```

**原因：** 系统时间不对，导致 SSL 证书验证失败

---

## ⚡ 快速修复 (3 种方案)

### 方案 A：使用本地 HTTP 测试（最快 - 5分钟）

```bash
# 1. 启动本地服务器
python3 tools/ota_test_server.py

# 2. 编辑 main/ota_updater.c 第 37-45 行
# 把 https 改成你的电脑 IP（从上面的脚本输出看）
#define OTA_MANIFEST_URL "http://192.168.x.x:8000/latest.json"

# 3. 重新编译烧录
cmake --build build
cmake --build build -- flash

# 4. 点击 Web UI 的"立即检查更新"
# ✓ 如果成功，说明 OTA 代码没问题，就是时间和 SSL
```

---

### 方案 B：修复系统时间（推荐 - 永久解决）

**需要修改的文件：** `main/claude_wifi_mode.c` 或 WiFi 初始化代码

**添加以下代码：**

```c
#include <time.h>
#include <sys/time.h>

// 在 WiFi 连接成功后调用这个函数
void sync_time_with_sntp(void)
{
    ESP_LOGI(TAG, "Starting SNTP time sync...");
    
    // 配置 NTP 服务器
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // 等待时间同步（最多 10 秒）
    int count = 0;
    while (count < 100) {
        time_t now = time(NULL);
        if (now > 1577836800) {  // After 2020
            struct tm *tm = localtime(&now);
            ESP_LOGI(TAG, "✓ Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        count++;
    }
    ESP_LOGW(TAG, "SNTP sync timeout");
}
```

**在 WiFi 连接成功后调用：**

```c
// 比如在 WiFi 事件处理函数中
if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    sync_time_with_sntp();  // ← 添加这一行
}
```

然后重新编译：
```bash
cmake --build build
cmake --build build -- flash
```

---

### 方案 C：临时禁用 HTTPS（最快修复，但不安全）

编辑 `main/ota_updater.c` 第 44-46 行，改成 HTTP：

```c
#define OTA_MANIFEST_URL \
    "http://github.com/ruiofang/ESP32S3-TFT-BS/releases/latest/download/latest.json"
```

**⚠️ 注意：** GitHub 的 HTTP 可能不可用。只用于测试。

---

## ✅ 验证修复成功

烧录后，点击"立即检查更新"，查看日志：

**✓ 成功的日志：**
```
I (xxx) ota_updater: System time: 2026-06-26 14:30:45
I (xxx) ota_updater: Connecting to manifest server...
I (xxx) ota_updater: Connected! Fetching headers...
I (xxx) ota_updater: HTTP Response Status: 200
✓ Successfully read XXX bytes of manifest
I (xxx) ota_updater: Current version: 2.1
I (xxx) ota_updater: Latest version:  2.2
```

**❌ 失败的日志：**
```
⚠️  System time appears to be incorrect!
E (xxx) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
```

---

## 📚 更详细的说明

- **完整解决方案**: 看 [OTA_SSL_SOLUTION.md](OTA_SSL_SOLUTION.md)
- **一般故障排除**: 看 [OTA_TROUBLESHOOTING.md](OTA_TROUBLESHOOTING.md)

---

## 🆘 还是不行？

1. **方案 A 可以成功** → 问题确实是 SSL/时间，用方案 B 修复
2. **方案 A 也失败** → 可能网络问题，查看 [OTA_TROUBLESHOOTING.md](OTA_TROUBLESHOOTING.md)
3. **都不行** → 收集完整日志和网络信息，提交反馈
