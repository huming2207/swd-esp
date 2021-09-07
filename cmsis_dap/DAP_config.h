/**
 * DAPLink on ESP32-S2
 * HAL Wrapper
 *
 * By Jackson Mong Hu <huming2207@gmail.com>
 * License: MIT
 *
 */

#pragma once

#include <sdkconfig.h>
#include <driver/gpio.h>
#include <hal/gpio_ll.h>

#define CPU_CLOCK               CONFIG_ESP32S2_DEFAULT_CPU_FREQ_MHZ * 1000000        ///< Specifies the CPU Clock in Hz
#define DAP_SWD                 1               ///< SWD Mode:  1 = available, 0 = not available
#define DAP_JTAG                0               ///< JTAG Mode: 1 = available, 0 = not available.
#define DAP_JTAG_DEV_CNT        0               ///< Maximum number of JTAG devices on scan chain
#define DAP_DEFAULT_PORT        1               ///< Default JTAG/SWJ Port Mode: 1 = SWD, 2 = JTAG.
#define DAP_DEFAULT_SWJ_CLOCK   50000000         ///< Default SWD/JTAG clock frequency in Hz.
#define IO_PORT_WRITE_CYCLES    2               ///< I/O Cycles: 2=default, 1=Cortex-M0+ fast I/0

/// Maximum Package Size for Command and Response data.
/// This configuration settings is used to optimized the communication performance with the
/// debugger and depends on the USB peripheral. Change setting to 1024 for High-Speed USB.
#define DAP_PACKET_SIZE        64              ///< USB: 64 = Full-Speed, 1024 = High-Speed.

/// Maximum Package Buffers for Command and Response data.
/// This configuration settings is used to optimized the communication performance with the
/// debugger and depends on the USB peripheral. For devices with limited RAM or USB buffer the
/// setting can be reduced (valid range is 1 .. 255). Change setting to 4 for High-Speed USB.
#define DAP_PACKET_COUNT       4              ///< Buffers: 64 = Full-Speed, 4 = High-Speed.

/// Indicate that UART Serial Wire Output (SWO) trace is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define SWO_UART                0               ///< SWO UART:  1 = available, 0 = not available

/// Maximum SWO UART Baudrate
#define SWO_UART_MAX_BAUDRATE   10000000U       ///< SWO UART Maximum Baudrate in Hz

/// Indicate that Manchester Serial Wire Output (SWO) trace is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define SWO_MANCHESTER          0               ///< SWO Manchester:  1 = available, 0 = not available

/// SWO Trace Buffer Size.
#define SWO_BUFFER_SIZE         4096U           ///< SWO Trace Buffer Size in bytes (must be 2^n)

/// SWO Streaming Trace.
#define SWO_STREAM              0               ///< SWO Streaming Trace: 1 = available, 0 = not available.

/// Clock frequency of the Test Domain Timer. Timer value is returned with \ref TIMESTAMP_GET.
#define TIMESTAMP_CLOCK         80000000U      ///< Timestamp clock in Hz (0 = timestamps not supported).


/// Debug Unit is connected to fixed Target Device.
/// The Debug Unit may be part of an evaluation board and always connected to a fixed
/// known device.  In this case a Device Vendor and Device Name string is stored which
/// may be used by the debugger or IDE to configure device parameters.
#define TARGET_DEVICE_FIXED     0               ///< Target Device: 1 = known, 0 = unknown;

#if TARGET_DEVICE_FIXED
#define TARGET_DEVICE_VENDOR    ""              ///< String indicating the Silicon Vendor
#define TARGET_DEVICE_NAME      ""              ///< String indicating the Target Device
#endif


#define PIN_SWCLK   1
#define PIN_SWDIO   2
#define PIN_nRST    3
#define PIN_LED     5

static inline void PORT_JTAG_SETUP(void)
{
    (void)0; // Not supported
}

static inline void PORT_SWD_SETUP(void)
{
    // Set SWCLK HIGH, pull-up only
    gpio_ll_output_enable(&GPIO, PIN_SWCLK);
    gpio_ll_od_disable(&GPIO, PIN_SWCLK);
    gpio_ll_set_level(&GPIO, PIN_SWCLK, 1);
    gpio_ll_pulldown_dis(&GPIO, PIN_SWCLK);
    gpio_ll_pullup_en(&GPIO, PIN_SWCLK);


    // Set SWDIO HIGH, pull-up only
    gpio_ll_output_enable(&GPIO, PIN_SWDIO);
    gpio_ll_od_disable(&GPIO, PIN_SWDIO);
    gpio_ll_set_level(&GPIO, PIN_SWDIO, 1);
    gpio_ll_pulldown_dis(&GPIO, PIN_SWDIO);
    gpio_ll_pullup_en(&GPIO, PIN_SWDIO);

    // Set RESET HIGHm pull-up only
    gpio_ll_output_enable(&GPIO, PIN_nRST);
    gpio_ll_od_disable(&GPIO, PIN_nRST);
    gpio_ll_set_level(&GPIO, PIN_nRST, 1);
    gpio_ll_pulldown_dis(&GPIO, PIN_nRST);
    gpio_ll_pullup_en(&GPIO, PIN_nRST);
}

static inline void PORT_OFF(void)
{
    gpio_ll_output_disable(&GPIO, PIN_SWCLK);
    gpio_ll_output_disable(&GPIO, PIN_SWDIO);
    gpio_ll_output_disable(&GPIO, PIN_nRST);
    gpio_ll_input_enable(&GPIO, PIN_SWCLK);
    gpio_ll_input_enable(&GPIO, PIN_SWDIO);
    gpio_ll_input_enable(&GPIO, PIN_nRST);
}

static __always_inline uint32_t PIN_SWCLK_TCK_IN(void)
{
    return ((&GPIO)->out & (1 << PIN_SWCLK)) == 0 ? 0 : 1;
}

static __always_inline void PIN_SWCLK_TCK_SET(void)
{
     (&GPIO)->out_w1ts = (1 << PIN_SWCLK);
}

static __always_inline void PIN_SWCLK_TCK_CLR(void)
{
    (&GPIO)->out_w1tc = (1 << PIN_SWCLK);
}

static __always_inline uint32_t PIN_SWDIO_TMS_IN(void)
{
    return ((&GPIO)->out & (1 << PIN_SWDIO)) == 0 ? 0 : 1;
}

static __always_inline void PIN_SWDIO_TMS_SET(void)
{
    (&GPIO)->out_w1ts = (1 << PIN_SWDIO);
}

static __always_inline void PIN_SWDIO_TMS_CLR(void)
{
    (&GPIO)->out_w1tc = (1 << PIN_SWDIO);
}

static __always_inline uint32_t PIN_SWDIO_IN(void)
{
    return ((&GPIO)->in & (1 << PIN_SWDIO)) == 0 ? 0 : 1;
}

static __always_inline void PIN_SWDIO_OUT(uint32_t bit)
{
    if (bit & 1) {
        (&GPIO)->out_w1ts = (1 << PIN_SWDIO);
    } else {
        (&GPIO)->out_w1tc = (1 << PIN_SWDIO);
    }
}

static __always_inline void PIN_SWDIO_OUT_ENABLE(void)
{
    (&GPIO)->enable_w1ts = (1 << PIN_SWDIO);
    PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[PIN_SWDIO]);
}

static __always_inline void PIN_SWDIO_OUT_DISABLE(void)
{
    (&GPIO)->enable_w1tc = (1 << PIN_SWDIO);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO]);
}

static __always_inline uint32_t PIN_TDI_IN(void)
{
    return (0);   // Not available
}

static __always_inline void PIN_TDI_OUT(uint32_t bit)
{
    ;             // Not available
}

static __always_inline uint32_t PIN_TDO_IN(void)
{
    return (0);   // Not available
}

static __always_inline uint32_t PIN_nTRST_IN(void)
{
    return (0);   // Not available
}

static __always_inline void PIN_nTRST_OUT(uint32_t bit)
{
    ;             // Not available
}

static __always_inline uint32_t PIN_nRESET_IN(void)
{
    return ((&GPIO)->out & (1 << PIN_nRST)) == 0 ? 0 : 1;
}

static __always_inline void PIN_nRESET_OUT(uint32_t bit)
{
    if (bit & 1) {
        (&GPIO)->out_w1ts = (1 << PIN_nRST);
    } else {
        (&GPIO)->out_w1tc = (1 << PIN_nRST);
    }
}

#include "../../../esp_timer/private_include/esp_timer_impl.h"

static __always_inline uint32_t TIMESTAMP_GET()
{
    return esp_timer_impl_get_counter_reg();
}

static inline void DAP_SETUP(void)
{
    PORT_SWD_SETUP(); // Or maybe no need to set up again??

    gpio_ll_output_enable(&GPIO, PIN_LED);
    gpio_ll_input_disable(&GPIO, PIN_LED);
    gpio_ll_od_disable(&GPIO, PIN_LED);
    gpio_ll_pullup_en(&GPIO, PIN_LED);
    gpio_ll_pulldown_dis(&GPIO, PIN_LED);
}

static inline uint32_t RESET_TARGET(void)
{
    return 0; // No need
}

static inline void LED_CONNECTED_OUT(uint32_t bit)
{
    if (bit & 1) {
        (&GPIO)->out_w1ts = (1 << PIN_LED);
    } else {
        (&GPIO)->out_w1tc = (1 << PIN_LED);
    }
}

static inline void LED_RUNNING_OUT(uint32_t bit)
{
    (void) 0; // Not supported?
}

//**************************************************************************************************