#include "lvgl.h"

#include "wm_ui.h"
#include "wm_listen.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
static uint8_t screen[240*320*3];
static uint16_t draw[240*20];
void wm_listen_get(wm_listen_status_t *out) { memset(out,0,sizeof(*out)); }
static void flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
    uint16_t *p=(uint16_t*)px;
    for (int y=a->y1;y<=a->y2;y++) for(int x=a->x1;x<=a->x2;x++) {
        uint16_t v=*p++;
        int at=(y*240+x)*3;
        screen[at]=((v>>11)&31)*255/31;screen[at+1]=((v>>5)&63)*255/63;screen[at+2]=(v&31)*255/31;
    }
    lv_display_flush_ready(disp);
}
static void save(const char *name) {
    lv_refr_now(NULL);
    FILE *f=fopen(name,"wb"); assert(f);fprintf(f,"P6\n240 320\n255\n");fwrite(screen,1,sizeof(screen),f);fclose(f);
}
int main(void) {
    lv_init(); lv_display_t *d=lv_display_create(240,320);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,draw,NULL,sizeof(draw),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);wm_ui_init();
    wm_ui_status_t st={.battery=100,.wifi_bars=4,.minutes=870};
    static wm_model_t m; m.card.saved=true;
    strcpy(m.card.name,"林晓");strcpy(m.card.title,"产品设计师");strcpy(m.card.org,"WeMeet");
    strcpy(m.card.code,"BEGIN:VCARD\r\nVERSION:3.0\r\nN:;Lin Xiao;;;\r\nFN:Lin Xiao\r\nORG:WeMeet\r\nTEL:+65 12345678\r\nEND:VCARD\r\n");
    wm_ui_show_card(&st,&m);save("card.ppm");
    m.card.avatar_ready=true;for(int i=0;i<48*48;i++){m.card.avatar[2*i]=0xe0;m.card.avatar[2*i+1]=0x07;}
    wm_ui_show_card(&st,&m);save("avatar.ppm");
    m.notice_count=4;for(int i=0;i<4;i++){m.notices[i].id=i+1;snprintf(m.notices[i].title,48,"第 %d 条会议通知",i+1);strcpy(m.notices[i].body,"活动地点已更新，请前往一层大厅签到。会议开始前请查看最新日程。");}
    wm_ui_show_notices(&st,&m,true);save("notices.ppm");
    wm_ui_show_notice_list(&st,&m,3);save("list.ppm");
    wm_ui_show_notice_detail(&st,&m.notices[3],0);save("detail.ppm");
    wm_ui_show_qr(m.card.code,m.card.name);save("qr.ppm");
    return 0;
}
