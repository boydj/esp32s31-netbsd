idf_component_register(SRCS "main.c" "ring.c" "wifi.c" "bt.c" "sensors.c" "power.c"
                       INCLUDE_DIRS "."
                       REQUIRES espcp_sys
                       PRIV_REQUIRES esp_wifi esp_event bt esp_timer log heap esp_app_format
                                    esp_driver_tsens esp_adc esp_hw_support esp_rom)
