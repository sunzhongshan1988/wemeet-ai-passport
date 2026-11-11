#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Driver buffers accept full-width SSID/PSK without trailing NUL. */
static inline bool wm_wifi_credentials(uint8_t ssid_out[32], uint8_t psk_out[64],
                                       const char *ssid, const char *psk) {
    size_t a = ssid ? strlen(ssid) : 0;
    size_t b = psk ? strlen(psk) : 0;
    if (!a || a > 32 || b > 64) return false;
    memset(ssid_out, 0, 32);
    memset(psk_out, 0, 64);
    memcpy(ssid_out, ssid, a);
    if (b) memcpy(psk_out, psk, b);
    return true;
}
static inline bool wm_https_needs_clock(const char *base, int64_t now) {
    return strncmp(base, "https://", 8) == 0 && now < 1700000000;
}
/* An unrelated/older sync must not permit reboot before this command's ack. */
static inline bool wm_reboot_acknowledged(uint32_t pending, uint32_t synced_ack) {
    return pending != 0 && pending == synced_ack;
}
