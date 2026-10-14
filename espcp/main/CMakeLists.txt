idf_component_register(SRCS "main.c" "ring.c" "wifi.c" "bt.c"
                       INCLUDE_DIRS "."
                       REQUIRES espcp_sys
                       PRIV_REQUIRES esp_wifi esp_event bt esp_timer log heap)
