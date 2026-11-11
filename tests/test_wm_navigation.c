#include "wm_navigation.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    wm_notice_nav_t nav = {0};
    const uint32_t ids[] = {40, 30, 20, 10};
    wm_notice_key(&nav, WM_NAV_OK, ids, 4);
    assert(nav.view == WM_NOTICE_LIST && nav.selected_id == 40);
    wm_notice_key(&nav, WM_NAV_UP, ids, 4);
    assert(nav.selected == 3 && nav.selected_id == 10); // Fourth item is reachable.
    wm_notice_key(&nav, WM_NAV_OK, ids, 4);
    assert(nav.view == WM_NOTICE_DETAIL);
    wm_notice_key(&nav, WM_NAV_DOWN, ids, 4);
    assert(nav.selected_id == 10); // Detail arrows must not select another notice.
    wm_notice_key(&nav, WM_NAV_BACK, ids, 4);
    assert(nav.view == WM_NOTICE_LIST && nav.selected == 3);
    wm_notice_key(&nav, WM_NAV_DOWN, ids, 4);
    assert(nav.selected == 0);
    const uint32_t refreshed[] = {50, 40, 30, 20};
    wm_notice_reconcile(&nav, refreshed, 4);
    assert(nav.selected == 1 && nav.selected_id == 40);
    wm_notice_key(&nav, WM_NAV_BACK, refreshed, 4);
    assert(nav.view == WM_NOTICE_OVERVIEW);
    wm_notice_key(&nav, WM_NAV_OK, NULL, 0);
    assert(nav.view == WM_NOTICE_OVERVIEW && nav.selected_id == 0);
    nav = (wm_notice_nav_t) { .view = WM_NOTICE_LIST, .selected = 3, .selected_id = 10 };
    wm_notice_reconcile(&nav, refreshed, 1);
    assert(nav.selected == 0 && nav.selected_id == 50);
    wm_notice_key(&nav, WM_NAV_BACK, NULL, 0);
    assert(nav.view == WM_NOTICE_OVERVIEW);
    assert(wm_battery_fill_width(-1) == 0);
    assert(wm_battery_fill_width(0) == 0);
    assert(wm_battery_fill_width(50) == 8);
    assert(wm_battery_fill_width(100) == 16);
    assert(wm_battery_fill_width(101) == 16);
    puts("Notice navigation and battery geometry: PASS");
}
