idf_component_register(
    SRCS "main.c"
         "wm_app.c"
         "wm_ui.c"
         "wm_listen.c"
         "wm_sonic.c"
         "wm_rs.c"
         "wm_proto.c"
         "wm_net.c"
         "wm_cloud.c"
         "wm_store.c"
         "../assets/fonts/wm_font_16.c"
         "../assets/fonts/wm_font_16_ext.c"
         "../assets/fonts/wm_font_28.c"
    INCLUDE_DIRS "."
    # 字体位图以二进制嵌入(符号 _binary_wm_font_*_bin_start),避免把几 MB 位图写成 C 源码。
    EMBED_FILES "../assets/fonts/wm_font_16.bin"
                "../assets/fonts/wm_font_16_ext.bin"
                "../assets/fonts/wm_font_28.bin"
    REQUIRES bsp esp_app_format esp_event esp_http_client esp_hw_support esp_netif
             esp_timer esp_wifi esp-tls json mbedtls nvs_flash
)
