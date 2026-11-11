#include "wm_runtime_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t ssid[32], psk[64];
    char full_ssid[33], full_psk[65];
    memset(full_ssid, 's', 32); full_ssid[32] = 0;
    memset(full_psk, 'a', 64); full_psk[64] = 0;
    assert(wm_wifi_credentials(ssid, psk, full_ssid, full_psk));
    assert(memcmp(ssid, full_ssid, 32) == 0);
    assert(memcmp(psk, full_psk, 64) == 0);
    assert(!wm_wifi_credentials(ssid, psk, "", ""));
    assert(!wm_wifi_credentials(ssid, psk, "123456789012345678901234567890123", ""));
    assert(wm_wifi_credentials(ssid, psk, "open", NULL));
    assert(ssid[4] == 0 && psk[0] == 0 && psk[63] == 0);
    assert(wm_https_needs_clock("https://example.test", 0));
    assert(!wm_https_needs_clock("http://192.168.1.2", 0));
    assert(!wm_https_needs_clock("https://example.test", 1794370654));
    assert(!wm_reboot_acknowledged(0, 0));
    assert(!wm_reboot_acknowledged(42, 0));
    assert(!wm_reboot_acknowledged(42, 41));
    assert(wm_reboot_acknowledged(42, 42));
    puts("test_wm_runtime_policy: PASS");
}
