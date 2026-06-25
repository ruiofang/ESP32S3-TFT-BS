# OTA SSL 连接失败 - 完整解决方案

## 你的问题诊断

从日志分析，你遇到的是 **SSL/TLS 握手失败**：

```
E (18367) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
E (18440) esp-tls: create_ssl_handle failed
```

这通常由以下原因引起（按优先级排列）：

### 🔴 **最可能的原因：系统时间不正确**

SSL 证书验证需要设备的系统时间在正确范围内。如果时间：
- 早于 2020 年
- 或者相差超过几分钟

则会导致证书验证失败，连接被拒绝。

---

## 解决方案

### 方案 1️⃣：设置系统时间（推荐 - 永久解决）

**最好的办法是让设备通过 SNTP 同步网络时间。**

#### A. 如果设备有互联网连接

在项目的 WiFi 初始化代码中添加时间同步：

编辑 `main/main.c` 或 `main/claude_wifi_mode.c`，找到 WiFi 连接成功的地方，添加：

```c
#include <time.h>
#include <sys/time.h>

// 在 WiFi 连接成功后调用
void sync_time_with_sntp(void)
{
    time_t now = time(NULL);
    struct tm timeinfo = *localtime(&now);
    
    ESP_LOGI(TAG, "Before SNTP sync: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    // 配置 SNTP
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // 等待时间同步（最多等待 10 秒）
    time_t start = time(NULL);
    while (time(NULL) - start < 10) {
        time_t t = time(NULL);
        if (t > 1577836800) {  // After Jan 1, 2020
            struct tm *tm = localtime(&t);
            ESP_LOGI(TAG, "✓ Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

**需要的头文件：**
```c
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"  // 或其他SNTP相关头文件
```

#### B. 如果设备没有互联网（离线场景）

可以通过 Web UI 或 Serial 命令设置时间：

```bash
# 通过 date 命令（如果支持）
date -s "2026-06-26 14:30:45"

# 或通过 RTC 芯片（如果有）
# 需要根据硬件确定具体方法
```

---

### 方案 2️⃣：使用 HTTP 本地服务器进行测试（快速诊断）

如果无法立即修复时间同步，可以先用 HTTP 测试来确认其他部分正常工作。

#### 步骤 1：启动本地服务器

```bash
cd /home/phi/文档/ESP32S3-TFT-BS
python3 tools/ota_test_server.py --port 8000
```

输出会显示：
```
🚀 ESP32 OTA 测试服务器
= = = = = = = = = = = = = = = = = = = = =
✓ IP地址:      192.168.x.x
✓ 端口:        8000
...
```

#### 步骤 2：修改设备配置使用本地 HTTP

编辑 `main/ota_updater.c`，改成本地 IP：

```c
#define OTA_MANIFEST_URL "http://192.168.x.x:8000/latest.json"
#define OTA_MANIFEST_URL_BACKUP "http://192.168.x.x:8000/latest.json"
```

替换 `192.168.x.x` 为你电脑的 IP（从 ota_test_server.py 的输出获取）

#### 步骤 3：重新编译烧录

```bash
cmake --build build
cmake --build build -- flash
```

#### 步骤 4：测试 OTA

点击 Web UI 中的"立即检查更新"按钮。

**预期结果：**
```
I (xxx) ota_updater: Fetching manifest from: http://192.168.x.x:8000/latest.json
I (xxx) ota_updater: Connecting to manifest server...
I (xxx) ota_updater: Connected! Fetching headers...
I (xxx) ota_updater: HTTP Response Status: 200
✓ Successfully read XX bytes of manifest
```

如果 HTTP 测试成功，说明网络和 OTA 代码正常，问题确实是 SSL 时间验证。

---

### 方案 3️⃣：在 sdkconfig 中启用 HTTP 本地模式

如果想暂时禁用 HTTPS 进行调试，可以编辑 `sdkconfig` 添加：

```
CONFIG_OTA_MANIFEST_USE_HTTP_LOCAL=y
```

然后重新编译：
```bash
cmake --build build
```

这会强制使用本地 HTTP（`http://192.168.4.1:8000/latest.json`）。

---

## 完整修复流程（一步步指南）

### 第一步：验证问题（5 分钟）

1. 烧录最新固件（已包含诊断功能）
2. 点击"立即检查更新"
3. 查看日志中的"System time"行

如果显示：
```
⚠️  System time appears to be incorrect!
   Current time: 1970-01-01 00:00:00
```

那就确认是时间问题。

### 第二步：修复时间同步（10-15 分钟）

在你的 WiFi 连接代码中添加 SNTP 同步（见方案 1）：

```bash
# 编辑相关文件
nano main/claude_wifi_mode.c  # 或其他WiFi初始化文件

# 添加上面的sync_time_with_sntp()函数
# 在WiFi连接成功后调用它

# 重新编译
cmake --build build
cmake --build build -- flash
```

### 第三步：验证修复（5 分钟）

1. 烧录新固件
2. 等待 WiFi 连接
3. 点击"立即检查更新"
4. 检查日志中的时间
5. 应该看到成功连接GitHub或Gitee

```
I (xxx) ota_updater: System time: 2026-06-26 14:30:45
✓ Primary URL successful
I (xxx) ota_updater: Current version: 2.1
I (xxx) ota_updater: Latest version:  2.2
```

---

## 日志中的关键信息

### ✅ 正常的日志

```
I (xxx) ota_updater: System time: 2026-06-26 14:30:45
I (xxx) ota_updater: Primary URL:  https://github.com/...
I (xxx) ota_updater: Fetching manifest from: https://github.com/...
I (xxx) ota_updater: Connecting to manifest server (timeout: 20000ms)...
I (xxx) ota_updater: Connected! Fetching headers...
I (xxx) ota_updater: HTTP Response Status: 200
✓ Successfully read XXX bytes of manifest
I (xxx) ota_updater: Current version: 2.1
I (xxx) ota_updater: Latest version:  2.2
I (xxx) ota_updater: New version available!
```

### ❌ 时间问题的日志

```
⚠️  System time appears to be incorrect!
    Current time: 1970-01-01 00:00:00
    SSL certificate verification may fail!

E (xxx) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
E (xxx) ota_updater: ❌ Connection failure
   HTTPS SSL/TLS errors often caused by:
    1. Incorrect system time (most common!)
```

### ❌ 网络问题的日志

```
I (xxx) ota_updater: System time: 2026-06-26 14:30:45
I (xxx) ota_updater: Fetching manifest from: https://github.com/...
I (xxx) ota_updater: Connecting to manifest server...
W (xxx) esp-tls: [sock=XX] select() timeout
E (xxx) transport_base: Failed to open a new connection
E (xxx) HTTP_CLIENT: Connection failed, sock < 0
```

---

## 参考资源

- [ESP-IDF SNTP 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/system_time.html)
- [SSL/TLS 配置](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_tls.html)
- [OTA 完整指南](OTA_TROUBLESHOOTING.md)

---

## 支持

如果按照上述步骤操作后仍然失败，请收集以下信息：

1. **完整的 console 日志**（包括 WiFi 连接到 OTA 检查失败的全过程）
2. **当前时间输出**（从"System time:"那行）
3. **已尝试的方案**（方案1/2/3）
4. **网络环境**（WiFi 名称、是否有代理等）

然后提交 issue 或反馈。
