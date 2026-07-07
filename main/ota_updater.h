#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t   ota_updater_init(void);
esp_err_t   ota_updater_set_auto_enabled(bool enabled);
bool        ota_updater_get_auto_enabled(void);
esp_err_t   ota_updater_trigger_now(void);
void        ota_updater_note_sta_got_ip(void);
const char *ota_updater_last_status(void);
const char *ota_updater_current_version(void);

// Check for pending update and confirmation
bool        ota_updater_has_pending_update(void);
const char *ota_updater_pending_version(void);
esp_err_t   ota_updater_confirm_and_start(void);
int         ota_updater_get_progress(void);
const char *ota_updater_get_progress_msg(void);

// Manifest URL management (runtime-overridable via NVS; compile-time macros are defaults).
void        ota_updater_get_manifest_urls(char *primary, size_t pn, char *backup, size_t bn);
void        ota_updater_get_default_manifest_urls(const char **primary, const char **backup);
esp_err_t   ota_updater_set_manifest_urls(const char *primary, const char *backup);
esp_err_t   ota_updater_reset_manifest_urls(void);

// Register /api/ota/{status,enable,check_now,confirm,upload,urls,urls/reset} and /ota/upload page
// on an already-started httpd. Call this from every web server that should expose OTA controls.
esp_err_t   ota_updater_register_http_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
