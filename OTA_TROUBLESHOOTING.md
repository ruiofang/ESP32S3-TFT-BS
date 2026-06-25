# OTA 更新故障排除指南

## 问题：OTA 检查失败（SSL连接错误）

### 错误信息
```
mbedtls_ssl_setup returned -0x7F00
Failed to open new connection
select() timeout
Connection failed, sock < 0
```

这表示 ESP32 无法连接到 GitHub 来获取最新的固件信息。

---

## 原因诊断

### 1. WiFi 连接问题 ⚠️

**最常见的原因：设备没有互联网连接**

检查设备连接的是什么网络：
- ✅ 如果是 **STA 模式**（连接到有互联网的WiFi）→ 继续第2步
- ❌ 如果是 **AP 模式**（设备本身是热点）→ **这是问题！** 设备没有互联网

**解决方案：**
```
连接设备到有互联网的WiFi网络（而不是用设备做热点）
查看日志是否显示 "WiFi AP initialized" 或其他连接方式
```

### 2. DNS 解析问题

GitHub 网址需要DNS才能解析。如果本地网络DNS配置有问题：

**诊断方法：**
- 检查设备是否能ping 8.8.8.8（Google DNS）
- 检查路由器的DNS转发是否正常

### 3. SSL/证书问题

错误 `-0x7F00` 通常表示 SSL 初始化失败。可能原因：

- **设备时间不对** - ESP32 的RTC时间用于验证SSL证书有效期
- **证书bundle不完整** - esp_crt_bundle_attach 可能无法验证证书

**诊断方法：**
```
检查 console 日志中是否有时间相关的错误信息
看是否有其他SSL相关的错误
```

### 4. GitHub 网络限制

某些网络可能会屏蔽或限制对GitHub的访问。

---

## 快速修复方案

### 方案 A：使用本地 HTTP 服务器（推荐测试）

如果GitHub无法访问，可以在本地网络中创建一个简单的HTTP服务器来测试OTA流程。

**步骤 1：在 PC 上启动本地服务器**

在包含 `latest.json` 的目录中：

```bash
# Python 3.x
python3 -m http.server 8000

# 或使用 Python 2.x
python -m SimpleHTTPServer 8000
```

**步骤 2：创建 latest.json 文件**

```json
{
  "version": "2.2",
  "url": "http://192.168.x.x:8000/panda.bin"
}
```

替换 `192.168.x.x` 为你的PC在局域网上的IP地址。

**步骤 3：编译时使用本地URL**

编辑 `main/ota_updater.c` 第39-42行，改成：

```c
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL "http://192.168.x.x:8000/latest.json"
#endif
```

然后重新编译：
```bash
cmake --build build
```

**步骤 4：烧录并测试**

新固件会从本地HTTP服务器获取更新。

### 方案 B：增加超时时间

某些网络连接较慢，20秒超时可能还不够。编辑 `main/ota_updater.c`：

```c
.timeout_ms = 30000,  // 改成 30秒
```

### 方案 C：检查 WiFi 连接质量

如果WiFi连接不稳定，OTA可能无法完成。查看：

```c
// 在 main/main.c 或 wifi 初始化代码中
// 确认设备已正确连接到有互联网的WiFi
// 查看日志中的 RSSI（信号强度）
```

---

## 调试日志解读

当你烧录新固件并运行 OTA 检查时，会看到详细的日志：

```
I (xxx) ota_updater: === OTA Manifest Check ===
I (xxx) ota_updater: Manifest URL: https://github.com/...
I (xxx) ota_updater: Manifest fetch attempt 1/3
I (xxx) ota_updater: Fetching manifest from: https://github.com/...
I (xxx) ota_updater: Connecting to manifest server (timeout: 20000ms)...
```

如果失败：
```
E (xxx) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00
E (xxx) ota_updater: Failed to connect to manifest server: ESP_ERR_INVALID_ARG (0x102)
  -> Invalid argument (bad URL or config)
E (xxx) ota_updater: Please check:
  1. WiFi network connectivity
  2. DNS resolution (can device reach 8.8.8.8?)
  3. GitHub is accessible from your network
  4. Device time is set correctly (for SSL verification)
```

---

## 完整检查清单

- [ ] 设备连接到有互联网的 WiFi（STA 模式）
- [ ] WiFi 信号强度足够（RSSI > -70 dBm）
- [ ] 设备时间已设置（用于 SSL 证书验证）
- [ ] DNS 可用（可以 resolve 域名）
- [ ] GitHub 或指定的 OTA 服务器可访问
- [ ] 足够的堆内存（显示 >100KB 空闲）
- [ ] 网络延迟不超过 20 秒

---

## 报告问题

如果以上方案都不能解决，请收集以下信息：

1. **完整的console日志**（从启动到OTA检查失败）
2. **网络信息**：
   - WiFi 名称和连接模式（STA/AP）
   - 路由器信息
   - 是否能访问 Google 或其他外网服务
3. **设备信息**：
   - ESP32-S3 版本
   - 当前固件版本
   - 启动时显示的系统信息

---

## 参考资源

- [ESP-IDF TLS/SSL 文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_tls.html)
- [mbedTLS 错误码参考](https://tls.mbed.org/error-codes)
- [ESP32 WiFi 配置](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html)
