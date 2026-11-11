#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wm_net_test_sdk.h"
#ifndef WM_NET_SOURCE
#define WM_NET_SOURCE "../main/wm_net.c"
#endif
#include WM_NET_SOURCE

esp_event_base_t WIFI_EVENT="wifi", IP_EVENT="ip";
typedef struct { esp_event_base_t base; int32_t id; unsigned char bytes[sizeof(wifi_config_t)]; } event;
static event events[128];
static unsigned head,tail;
static bool dispatching,driver_started,driver_connecting, timer_pending;
static int connect_calls, failures;
static wm_net_state_t observed;
static wm_net_fail_t failure;
static wifi_config_t configured;
static void (*timer_cb)(void *);
static void pump_one(void) {
    assert(head < tail);
    event e=events[head++];
    bool before=dispatching;
    dispatching=true;
    on_event(NULL,e.base,e.id,e.bytes);
    dispatching=before;
}
static void drain(void) { while(head<tail) pump_one(); head=tail=0; }
esp_err_t esp_event_post(esp_event_base_t b,int32_t id,const void *data,size_t n,unsigned wait) {
    (void)wait; assert(tail<128 && n<=sizeof(events[0].bytes));
    event *e=&events[tail++]; memset(e,0,sizeof(*e)); e->base=b;e->id=id;
    if(n)memcpy(e->bytes,data,n);
    return ESP_OK;
}
static void got_ip(void) { esp_event_post(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL,0,0);drain(); }
static void disconnected(uint8_t reason) {
    driver_connecting=false;
    wifi_event_sta_disconnected_t d={.reason=reason};
    esp_event_post(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&d,sizeof(d),0);
}
static void state_changed(wm_net_state_t s,wm_net_fail_t f) { observed=s;failure=f;if(s==WM_NET_FAILED)++failures; }
void test_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
const char *esp_err_to_name(esp_err_t e) {(void)e;return "error";}
esp_err_t esp_event_loop_create_default(void){return ESP_OK;}
esp_err_t esp_event_handler_register(esp_event_base_t b,int32_t id,void (*f)(void *,esp_event_base_t,int32_t,void *),void *arg){(void)b;(void)id;(void)f;(void)arg;return ESP_OK;}
esp_err_t esp_netif_init(void){return ESP_OK;}
void *esp_netif_create_default_wifi_sta(void){return &observed;}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c){(void)c;return ESP_OK;}
esp_err_t esp_netif_sntp_start(void){return ESP_OK;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *t){timer_cb=a->callback;*t=&observed;return ESP_OK;}
esp_err_t esp_timer_stop(esp_timer_handle_t t){(void)t;timer_pending=false;return ESP_OK;}
esp_err_t esp_timer_start_once(esp_timer_handle_t t,uint64_t delay){(void)t;(void)delay;timer_pending=true;return ESP_OK;}
esp_err_t esp_wifi_init(const wifi_init_config_t *c){(void)c;return ESP_OK;}
esp_err_t esp_wifi_set_storage(int v){(void)v;return ESP_OK;}
esp_err_t esp_wifi_set_mode(int v){(void)v;return ESP_OK;}
esp_err_t esp_wifi_start(void){driver_started=true;return esp_event_post(WIFI_EVENT,WIFI_EVENT_STA_START,NULL,0,0);}
esp_err_t esp_wifi_stop(void){
    assert(driver_started);
    disconnected(8);
    esp_event_post(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL,0,0); // Late event from the old link.
    esp_event_post(WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL,0,0);
    driver_started=driver_connecting=false;
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void){disconnected(8);return ESP_OK;}
esp_err_t esp_wifi_connect(void){assert(driver_started);++connect_calls;driver_connecting=true;return ESP_OK;}
esp_err_t esp_wifi_set_config(int iface,wifi_config_t *cfg){
    (void)iface;
    // Reproduce the real old callback arriving between disconnect and set_config.
    if(!dispatching && head<tail)pump_one();
    if(driver_connecting)return ESP_ERR_WIFI_STATE;
    configured=*cfg;return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap){ap->rssi=-30;return ESP_OK;}
int main(void){
    assert(wm_net_init(state_changed)==ESP_OK);
    assert(wm_net_connect("first","password-one")==ESP_OK);drain();got_ip();
    assert(observed==WM_NET_ONLINE);
    int before=connect_calls;
    assert(wm_net_connect("second","password-two")==ESP_OK);drain();
    assert(failures==0 && observed==WM_NET_CONNECTING && connect_calls==before+1);
    assert(strcmp((char *)configured.sta.ssid,"second")==0);got_ip();
    assert(observed==WM_NET_ONLINE);

    // Several queued replacements must ignore intermediate START/STOP/IP events.
    before=connect_calls;
    assert(wm_net_connect("third","password-three")==ESP_OK);
    assert(wm_net_connect("fourth","password-four")==ESP_OK);drain();
    assert(connect_calls==before+1 && failures==0 && observed==WM_NET_CONNECTING);
    assert(strcmp((char *)configured.sta.ssid,"fourth")==0);got_ip();

    // An established network uses delayed retries, and disconnect cancels them.
    before=connect_calls;disconnected(1);drain();
    assert(timer_pending && connect_calls==before && observed==WM_NET_CONNECTING);
    timer_cb(NULL);drain();assert(connect_calls==before+1);
    wm_net_disconnect();drain();before=connect_calls;
    timer_cb(NULL);got_ip();assert(observed==WM_NET_IDLE && connect_calls==before);

    // Wrong credentials still fail with an authentication reason after five retries.
    assert(wm_net_connect("first","wrong-password")==ESP_OK);drain();before=connect_calls;
    for(int i=0;i<6;i++){disconnected(WIFI_REASON_AUTH_FAIL);drain();}
    assert(observed==WM_NET_FAILED && failure==WM_NET_FAIL_AUTH);
    assert(connect_calls==before+5 && failures==1);
    puts("wm_net: reconfiguration race, stale events, retries and cancellation PASS");
}
