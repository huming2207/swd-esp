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
#include <esp_cpu.h>
#include <esp_rom_sys.h>

#include "esp_timer.h"
#ifdef CONFIG_ESP_SWD_USE_SPI
#include "SW_DP_SPI.h"
#endif
#if defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
#include <hal/dedic_gpio_cpu_ll.h>
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S2)
#define CPU_CLOCK               CONFIG_ESP32S2_DEFAULT_CPU_FREQ_MHZ * 1000000        ///< Specifies the CPU Clock in Hz
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define CPU_CLOCK               CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ * 1000000        ///< Specifies the CPU Clock in Hz
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
#define CPU_CLOCK               CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000            ///< Specifies the CPU Clock in Hz
#endif
#if (CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ != -1) && (CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ < 10000)
#error "CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ must be -1 or at least 10000"
#endif
#define DAP_SWD                 1               ///< SWD Mode:  1 = available, 0 = not available
#define DAP_JTAG                0               ///< JTAG Mode: 1 = available, 0 = not available.
#define DAP_JTAG_DEV_CNT        0               ///< Maximum number of JTAG devices on scan chain
#define DAP_DEFAULT_PORT        1               ///< Default JTAG/SWJ Port Mode: 1 = SWD, 2 = JTAG.
#define DAP_DEFAULT_SWJ_CLOCK   CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ ///< Default SWD/JTAG clock in Hz, or -1 for unpaced transfers.
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

#ifndef CONFIG_ESP_SWD_CLK_PIN
#define PIN_SWCLK   1
#else
#define PIN_SWCLK   CONFIG_ESP_SWD_CLK_PIN
#endif

#ifndef CONFIG_ESP_SWD_IO_PIN
#define PIN_SWDIO   2
#else
#define PIN_SWDIO   CONFIG_ESP_SWD_IO_PIN
#endif

#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
#define PIN_SWDIO_OUT_GPIO CONFIG_ESP_SWD_DATA_OUT_PIN
#define PIN_SWDIO_IN_GPIO  CONFIG_ESP_SWD_DATA_IN_PIN
#define ESP_SWD_TURNAROUND_GUARD_NS \
    (CONFIG_ESP_SWD_TURNAROUND_DELAY_US * 1000U + CONFIG_ESP_SWD_TURNAROUND_DELAY_NS)
#define ESP_SWD_TURNAROUND_GUARD_CYCLES \
    ((uint32_t)(((uint64_t)CPU_CLOCK * ESP_SWD_TURNAROUND_GUARD_NS + 999999999ULL) / 1000000000ULL))
#endif

#ifndef CONFIG_ESP_SWD_NRST_PIN
#define PIN_nRST    6
#else
#define PIN_nRST   CONFIG_ESP_SWD_NRST_PIN
#endif

#ifndef CONFIG_ESP_SWD_LED_PIN
#define PIN_LED     3
#else
#define PIN_LED     CONFIG_ESP_SWD_LED_PIN
#endif

#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
extern uint32_t g_swd_dedic_clk_mask;
extern uint32_t g_swd_dedic_data_out_mask;
extern uint32_t g_swd_dedic_data_in_mask;
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
extern uint32_t g_swd_dedic_translator_noe_mask;
extern uint32_t g_swd_dedic_translator_dir1_mask;
extern uint32_t g_swd_dedic_translator_dir_mask;
#endif

static __always_inline void swd_esp_translator_set_noe(uint32_t level)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    /* SPI2 CS0 owns the active-low SWDIO translator output enable. */
    (void)level;
#elif defined(CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS)
    dedic_gpio_cpu_ll_write_mask(g_swd_dedic_translator_noe_mask,
                                 level ? g_swd_dedic_translator_noe_mask : 0U);
#else
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_DATA_NOE_PIN, level);
#endif
}

static __always_inline void swd_esp_translator_set_direction(uint32_t host_owns_bus)
{
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
    dedic_gpio_cpu_ll_write_mask(g_swd_dedic_translator_dir_mask,
                                 host_owns_bus ? g_swd_dedic_translator_dir1_mask : 0U);
#else
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_DATA_DIR1_PIN, host_owns_bus);
    gpio_ll_set_level(&GPIO, CONFIG_ESP_SWD_DATA_DIR2_PIN, 0U);
#endif
}

esp_err_t swd_esp_port_init(void);
void swd_esp_port_setup(void);
void swd_esp_port_off(void);
void swd_esp_swdio_host_drive(uint32_t initial_bit);
void swd_esp_swdio_target_drive(void);
#endif


static inline void PORT_JTAG_SETUP(void)
{
    (void)0; // Not supported
}

static inline void PORT_SWD_SETUP(void)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    swd_esp_port_setup();
#else
    // Set SWCLK HIGH, pull-up only
    gpio_ll_output_enable(&GPIO, PIN_SWCLK);
    gpio_ll_od_disable(&GPIO, PIN_SWCLK);
    gpio_ll_set_level(&GPIO, PIN_SWCLK, 1);
    gpio_ll_pulldown_dis(&GPIO, PIN_SWCLK);
    gpio_ll_pullup_en(&GPIO, PIN_SWCLK);
    gpio_ll_pin_filter_disable(&GPIO, PIN_SWCLK);


    // Set SWDIO HIGH, pull-up only
    gpio_ll_output_enable(&GPIO, PIN_SWDIO);
    gpio_ll_od_disable(&GPIO, PIN_SWDIO);
    gpio_ll_set_level(&GPIO, PIN_SWDIO, 1);
    gpio_ll_pulldown_dis(&GPIO, PIN_SWDIO);
    gpio_ll_pullup_en(&GPIO, PIN_SWDIO);
    gpio_ll_pin_filter_disable(&GPIO, PIN_SWDIO);

    // Set RESET HIGH, pull-up only
    gpio_ll_output_enable(&GPIO, PIN_nRST);
    gpio_ll_od_disable(&GPIO, PIN_nRST);
    gpio_ll_set_level(&GPIO, PIN_nRST, 1);
    gpio_ll_pulldown_dis(&GPIO, PIN_nRST);
    gpio_ll_pullup_en(&GPIO, PIN_nRST);
#endif
}

static inline void PORT_OFF(void)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    swd_esp_port_off();
#else
    gpio_ll_output_disable(&GPIO, PIN_SWCLK);
    gpio_ll_output_disable(&GPIO, PIN_SWDIO);
    gpio_ll_output_disable(&GPIO, PIN_nRST);
    gpio_ll_input_enable(&GPIO, PIN_SWCLK);
    gpio_ll_input_enable(&GPIO, PIN_SWDIO);
    gpio_ll_input_enable(&GPIO, PIN_nRST);
#endif
}

static __always_inline uint32_t PIN_SWCLK_TCK_IN(void)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    return swd_esp_spi_clock_level();
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
    return (dedic_gpio_cpu_ll_read_out() & g_swd_dedic_clk_mask) != 0U;
#else
    return (GPIO.out & (1 << PIN_SWCLK)) == 0 ? 0 : 1;
#endif
}

static __always_inline void PIN_SWCLK_TCK_SET(void)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    swd_esp_spi_set_clock_idle(1U);
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
    dedic_gpio_cpu_ll_write_mask(g_swd_dedic_clk_mask, g_swd_dedic_clk_mask);
#else
     GPIO.out_w1ts = (1 << PIN_SWCLK);
#endif
}

static __always_inline void PIN_SWCLK_TCK_CLR(void)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    swd_esp_spi_set_clock_idle(0U);
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
    dedic_gpio_cpu_ll_write_mask(g_swd_dedic_clk_mask, 0U);
#else
    GPIO.out_w1tc = (1 << PIN_SWCLK);
#endif
}

static __always_inline uint32_t PIN_SWDIO_TMS_IN(void)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    return swd_esp_spi_data_level();
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
    return (dedic_gpio_cpu_ll_read_out() & g_swd_dedic_data_out_mask) != 0U;
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245)
    return (GPIO.out & (1U << PIN_SWDIO_OUT_GPIO)) != 0U;
#else
    return (GPIO.out & (1 << PIN_SWDIO)) == 0 ? 0 : 1;
#endif
}

static __always_inline void PIN_SWDIO_TMS_SET(void)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    swd_esp_spi_set_data_idle(1U);
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
    dedic_gpio_cpu_ll_write_mask(g_swd_dedic_data_out_mask, g_swd_dedic_data_out_mask);
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245)
    gpio_ll_set_level(&GPIO, PIN_SWDIO_OUT_GPIO, 1U);
#else
    GPIO.out_w1ts = (1 << PIN_SWDIO);
#endif
}

static __always_inline void PIN_SWDIO_TMS_CLR(void)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    swd_esp_spi_set_data_idle(0U);
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
    dedic_gpio_cpu_ll_write_mask(g_swd_dedic_data_out_mask, 0U);
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245)
    gpio_ll_set_level(&GPIO, PIN_SWDIO_OUT_GPIO, 0U);
#else
    GPIO.out_w1tc = (1 << PIN_SWDIO);
#endif
}

static __always_inline uint32_t PIN_SWDIO_IN(void)
{
#ifdef CONFIG_ESP_SWD_USE_SPI
    return swd_esp_spi_data_in_level();
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245) && defined(CONFIG_ESP_SWD_USE_DEDICATED_GPIO)
    return (dedic_gpio_cpu_ll_read_in() & g_swd_dedic_data_in_mask) != 0U;
#elif defined(CONFIG_ESP_SWD_PHY_AXC2T245)
    return gpio_ll_get_level(&GPIO, PIN_SWDIO_IN_GPIO);
#else
    return (GPIO.in & (1 << PIN_SWDIO)) == 0 ? 0 : 1;
#endif
}

static __always_inline void PIN_SWDIO_OUT(uint32_t bit)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    if (bit & 1U) {
        PIN_SWDIO_TMS_SET();
    } else {
        PIN_SWDIO_TMS_CLR();
    }
#else
    if (bit & 1) {
        GPIO.out_w1ts = (1 << PIN_SWDIO);
    } else {
        GPIO.out_w1tc = (1 << PIN_SWDIO);
    }
#endif
}

static __always_inline void PIN_SWDIO_OUT_ENABLE(void)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    swd_esp_swdio_host_drive(1U);
#else
    GPIO.enable_w1ts = (1 << PIN_SWDIO);
    PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[PIN_SWDIO]);
#endif
}

static __always_inline void PIN_SWDIO_OUT_ENABLE_VALUE(uint32_t bit)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    swd_esp_swdio_host_drive(bit);
#else
    PIN_SWDIO_OUT(bit);
    PIN_SWDIO_OUT_ENABLE();
#endif
}

static __always_inline void PIN_SWDIO_OUT_DISABLE(void)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    swd_esp_swdio_target_drive();
#else
    GPIO.enable_w1tc = (1 << PIN_SWDIO);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO]);
#endif
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
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    return gpio_ll_get_level(&GPIO, PIN_nRST) == 0U ? 1U : 0U;
#else
    return (GPIO.out & (1 << PIN_nRST)) == 0 ? 0 : 1;
#endif
}

static __always_inline void PIN_nRESET_OUT(uint32_t bit)
{
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    /* HOST_SW_RST drives an N-MOSFET gate: high asserts target nRESET. */
    gpio_ll_set_level(&GPIO, PIN_nRST, (bit & 1U) ? 0U : 1U);
#else
    if (bit & 1) {
        GPIO.out_w1ts = (1 << PIN_nRST);
    } else {
        GPIO.out_w1tc = (1 << PIN_nRST);
    }
#endif
}

static __always_inline uint32_t TIMESTAMP_GET()
{
    return esp_timer_get_time();
}

static inline void DAP_SETUP(void)
{
    PORT_SWD_SETUP(); // Or maybe no need to set up again??

#ifdef ESP_SWD_HAS_LED
    gpio_ll_output_enable(&GPIO, PIN_LED);
    gpio_ll_input_disable(&GPIO, PIN_LED);
    gpio_ll_od_disable(&GPIO, PIN_LED);
    gpio_ll_pullup_en(&GPIO, PIN_LED);
    gpio_ll_pulldown_dis(&GPIO, PIN_LED);
#endif
}

static inline uint32_t RESET_TARGET(void)
{
    return 0; // No need
}

static inline void LED_CONNECTED_OUT(uint32_t bit)
{
#ifdef CONFIG_ESP_SWD_HAS_LED
    if (bit & 1) {
        GPIO.out_w1ts = (1 << PIN_LED);
    } else {
        GPIO.out_w1tc = (1 << PIN_LED);
    }
#endif
}

static inline void LED_RUNNING_OUT(uint32_t bit)
{
#ifdef CONFIG_ESP_SWD_HAS_LED
    (void) 0; // Not supported?
#endif
}

//**************************************************************************************************
