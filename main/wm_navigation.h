#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { WM_NOTICE_OVERVIEW, WM_NOTICE_LIST, WM_NOTICE_DETAIL } wm_notice_view_t;
typedef enum { WM_NAV_UP, WM_NAV_DOWN, WM_NAV_OK, WM_NAV_BACK } wm_nav_key_t;
typedef struct { wm_notice_view_t view; int selected; uint32_t selected_id; } wm_notice_nav_t;

/* Selection follows identity across a background refresh, not the old row number. */
static inline void wm_notice_reconcile(wm_notice_nav_t *nav, const uint32_t *ids, int count) {
    if (count <= 0) { nav->selected = 0; nav->selected_id = 0; return; }
    for (int i = 0; i < count; i++) {
        if (ids[i] == nav->selected_id) { nav->selected = i; return; }
    }
    if (nav->selected >= count) nav->selected = count - 1;
    nav->selected_id = ids[nav->selected];
}
static inline void wm_notice_key(wm_notice_nav_t *nav, wm_nav_key_t key, const uint32_t *ids, int count) {
    wm_notice_reconcile(nav, ids, count);
    if (key == WM_NAV_BACK) {
        nav->view = nav->view == WM_NOTICE_DETAIL ? WM_NOTICE_LIST : WM_NOTICE_OVERVIEW;
    } else if (key == WM_NAV_OK && count > 0) {
        if (nav->view == WM_NOTICE_OVERVIEW) nav->view = WM_NOTICE_LIST;
        else if (nav->view == WM_NOTICE_LIST) nav->view = WM_NOTICE_DETAIL;
    } else if (nav->view == WM_NOTICE_LIST && count > 0 && (key == WM_NAV_UP || key == WM_NAV_DOWN)) {
        nav->selected = (nav->selected + (key == WM_NAV_UP ? -1 : 1) + count) % count;
        nav->selected_id = ids[nav->selected];
    }
}
/* 22x11 outline: 1px border + 2px white inset on each side. */
static inline int wm_battery_fill_width(int percent) {
    if (percent <= 0) return 0;
    if (percent > 100) percent = 100;
    return (16 * percent + 99) / 100;
}
