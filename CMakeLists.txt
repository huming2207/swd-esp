idf_component_register(
        SRCS
            "cmsis_dap/DAP.c" "cmsis_dap/DAP.h"
            "cmsis_dap/DAP_queue.c" "cmsis_dap/DAP_queue.h"
            "cmsis_dap/DAP_config.h"
            "cmsis_dap/dap_strings.h"
            "cmsis_dap/debug_cm.h"
            "cmsis_dap/SW_DP.c"
            "cmsis_dap/SW_DP_PARLIO.c" "cmsis_dap/SW_DP_PARLIO.h"
            "cmsis_dap/SW_DP_SPI.c" "cmsis_dap/SW_DP_SPI.h"
            "interface/swd_host.c" "interface/swd_host.h"
            "interface/swd_perf.h"
        INCLUDE_DIRS
            "cmsis_dap" "interface"
        PRIV_REQUIRES
            "driver" "esp_driver_gpio" "esp_driver_dma" "esp_hal_dma"
            "esp_hal_gpspi" "esp_hal_parlio" "esp_hw_support"
        REQUIRES
            "esp_timer"
)
