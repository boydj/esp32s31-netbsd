idf_component_register(SRCS "entry.S" "start.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_rom esp_hw_support heap riscv
                       PRIV_REQUIRES esp_system log freertos)

# Pull in the entry point and the overrides next to it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u espcp_entry")
