idf_component_register(
        SRCS
            "cmsis_dap/DAP.c" "cmsis_dap/DAP.h"
            "cmsis_dap/DAP_queue.c" "cmsis_dap/DAP_queue.h"
            "cmsis_dap/DAP_config.h"
            "cmsis_dap/dap_strings.h"
            "cmsis_dap/debug_cm.h"
            # "cmsis_dap/SW_DP.c"
            "cmsis_dap/SW_DP_SPI.c"
            "cmsis_dap/spi_ops.c" "cmsis_dap/spi_ops.h"
            "cmsis_dap/spi_setup.c" "cmsis_dap/spi_setup.h"
            "interface/swd_host.c" "interface/swd_host.h"
        INCLUDE_DIRS
            "cmsis_dap" "interface"
        PRIV_REQUIRES
            "driver"
)