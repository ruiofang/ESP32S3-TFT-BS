#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t   ota_updater_init(void);
esp_err_t   ota_updater_set_auto_enabled(bool enabled);
bool        ota_updater_get_auto_enabled(void);
esp_err_t   ota_updater_trigger_now(void);
const char *ota_updater_last_status(void);
const char *ota_updater_current_version(void);

// Check for pending update and confirmation
bool        ota_updater_has_pending_update(void);
const char *ota_updater_pending_version(void);
esp_err_t   ota_updater_confirm_and_start(void);

// Register /api/ota/status, /api/ota/enable, /api/ota/check_now, /api/ota/confirm on an already-started httpd.
// Call this from every web server that should expose OTA controls.
esp_err_t   ota_updater_register_http_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
