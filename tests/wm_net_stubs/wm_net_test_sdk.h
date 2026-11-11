#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_WIFI_STATE 4
const char *esp_err_to_name(esp_err_t e);
void test_log(const char *tag, const char *fmt, ...);
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
typedef const char *esp_event_base_t;
#define ESP_EVENT_DEFINE_BASE(name) esp_event_base_t name = #name
extern esp_event_base_t WIFI_EVENT, IP_EVENT;
#define ESP_EVENT_ANY_ID -1
#define pdMS_TO_TICKS(x) (x)
enum { WIFI_EVENT_STA_START, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_STA_STOP, IP_EVENT_STA_GOT_IP };
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, void (*)(void *,esp_event_base_t,int32_t,void *), void *);
esp_err_t esp_event_post(esp_event_base_t,int32_t,const void *,size_t,unsigned);
esp_err_t esp_netif_init(void);
void *esp_netif_create_default_wifi_sta(void);
typedef struct { bool start; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) ((esp_sntp_config_t){.start=true})
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
esp_err_t esp_netif_sntp_start(void);
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *,esp_timer_handle_t *);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_timer_start_once(esp_timer_handle_t,uint64_t);
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 0
#define WIFI_IF_STA 0
#define WPA3_SAE_PWE_BOTH 0
#define WIFI_ALL_CHANNEL_SCAN 0
#define WIFI_CONNECT_AP_BY_SIGNAL 0
enum { WIFI_REASON_NO_AP_FOUND=201, WIFI_REASON_AUTH_FAIL, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, WIFI_REASON_HANDSHAKE_TIMEOUT, WIFI_REASON_MIC_FAILURE };
typedef struct { uint8_t reason; } wifi_event_sta_disconnected_t;
typedef struct { int rssi; } wifi_ap_record_t;
typedef struct { struct { uint8_t ssid[32], password[64]; int sae_pwe_h2e; struct { bool capable; } pmf_cfg; int scan_method,sort_method; } sta; } wifi_config_t;
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_config(int,wifi_config_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
