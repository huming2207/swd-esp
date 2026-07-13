# SWD Driver for ESP32-S2/S3

This component is a port of the ARM CMSIS-DAP/DAPLink SWD transport. It lets an
ESP32-S2 or ESP32-S3 act as an SWD host for Cortex-M targets such as STM32
devices.

Two electrical interfaces are supported:

1. a legacy direct connection using one bidirectional ESP GPIO for SWDIO; and
2. a direction-controlled, level-translated interface using two
   SN74AXC2T245 devices, as fitted to Soul Injector Rev 6.

The translated backend can use the ESP32-S3 CPU dedicated-GPIO peripheral for
lower-overhead bit-banging.

## SWD overview

SWD normally uses three target signals:

```text
SWCLK   host-generated clock
SWDIO   half-duplex request, acknowledge and data
nRESET  optional active-low target reset
```

SWDIO is not an open-drain bus. The host and target take turns driving it
push-pull. A normal transfer looks approximately like this:

```text
                   host drives       target drives          host/target data
                +---------------+---------------------+-----------------------+
SWDIO owner     |    REQUEST    | TURN | ACK (3 bits) | READ data or TURN+WRITE
                +---------------+---------------------+-----------------------+

SWCLK           _/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_
```

Only one side may drive SWDIO at a time. Direction changes are therefore part
of the SWD protocol, not merely an ESP32 GPIO optimization.

## Legacy direct-GPIO interface

The direct backend connects one ESP32 GPIO to target SWDIO:

```text
                         target board
                    +-------------------+
ESP32 SWCLK --------| SWCLK             |
                    |                   |
ESP32 SWDIO <------>| SWDIO             |
                    |                   |
ESP32 nRESET -------| nRESET            |
                    +-------------------+
```

During host phases the SWDIO GPIO is a push-pull output. During target phases
the same pin is changed to an input/high-impedance state.

Select this backend with:

```text
CONFIG_ESP_SWD_PHY_DIRECT=y
CONFIG_ESP_SWD_CLK_PIN=<gpio>
CONFIG_ESP_SWD_IO_PIN=<gpio>
CONFIG_ESP_SWD_NRST_PIN=<gpio>
```

## Direction-controlled translated interface

Soul Injector Rev 6 separates host transmit and receive signals and translates
them between ESP32 VDD and target VTREF. `VPP` on that board means target
voltage reference; it is not a high programming voltage.

### Complete logical connection

```text
          ESP32-S3 side             level translation              target side
       (normally 3.3 V)          (VCCA=VDD, VCCB=VTREF)

 HOST_SWDATA_OUT  -----> A1  +--------------------+  B1 ----47R----+
                              |                    |                |
 SWDATA_DIR1     ------> DIR1 | SWDIO level shifter|                +--- TARGET_SWDATA
 SWDATA_nOE      ------> /OE  |                    |                |
 SWDATA_DIR2     ------> DIR2 |                    |  B2 <----------+
 HOST_SWDATA_IN  <-----  A2  +--------------------+
                                                                  |
                                                        VTREF--4.7k+

 HOST_SWCLK      -----> A1  +--------------------+  B1 ----47R-------- TARGET_SWCLK
 HOST_SWBOOT     -----> A2  | CLK/BOOT shifter    |  B2 ---------------- TARGET_BOOT
 SWCLK_nOE       ------> /OE +--------------------+
                              DIR1=high, DIR2=high
```

The two SWDIO translator channels serve different jobs:

```text
channel 1: HOST_SWDATA_OUT -> target SWDIO when the host owns the bus
channel 2: target SWDIO -> HOST_SWDATA_IN when the target owns the bus
```

The translator has one shared `/OE`, so isolating channel 1 also temporarily
disables the receive channel. `HOST_SWDATA_IN` is only valid after the
translator is enabled again.

### SN74AXC2T245 control meanings

Each translator channel uses:

```text
DIR = high  : A -> B
DIR = low   : B -> A
/OE = low   : outputs enabled
/OE = high  : both channels high impedance
```

For the SWDIO translator, channel 2 is always configured B-to-A. Only channel
1 changes direction:

| SWDIO state | `SWDATA_nOE` | `DIR1` | `DIR2` | ESP `HOST_SWDATA_OUT` |
|---|---:|---:|---:|---|
| Isolated | high | any | low | input/high-Z |
| Host drives target | low | high | low | push-pull output |
| Target drives host | low | low | low | input/high-Z |

It is important to change `DIR1` only while `/OE` is high. If channel 1 were
enabled in B-to-A mode while the ESP32 pad was still a push-pull output, the
translator and ESP32 could drive against each other.

## How an SWD turnaround works

The firmware implements ownership switching in `cmsis_dap/SW_DP.c`.

### Host releases SWDIO to the target

After the host sends the request park bit:

```text
                     translator disabled    target receive path enabled
                              |                         |
SWCLK       ‾‾‾‾‾‾‾‾‾\_______|_________________________/‾‾‾‾
                         hold low during control changes

SWDATA_nOE  _____________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\____________
DIR1        ‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\_______________________________
ESP OUT_EN  ‾‾‾‾‾‾‾‾‾‾‾‾\__________________________________
```

The operation order is:

```text
1. Pull SWCLK low.
2. Drive SWDATA_nOE high to isolate the translator.
3. Disable the ESP32 HOST_SWDATA_OUT pad driver.
4. Enable that pad's input path so it is a true high-impedance GPIO.
5. Drive DIR1 low.
6. Keep DIR2 low.
7. Drive SWDATA_nOE low to enable target-to-host translation.
8. Complete the existing SWD turnaround clock cycle.
```

The falling edge used to hold SWCLK low is the start of the protocol's existing
turnaround clock. The code does not insert an extra SWCLK pulse.

### Target releases SWDIO to the host

Before the host writes data or parks the line:

```text
                     translator disabled       host output enabled
                              |                         |
SWCLK       ‾‾‾‾‾‾‾‾‾\_______|_________________________/‾‾‾‾

SWDATA_nOE  _____________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\____________
DIR1        ________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
ESP OUT_EN  _________________________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
SWDOUT      xxxxxxxxxxxxxxxxx[first bit]-------------------
```

The operation order is:

```text
1. Pull SWCLK low.
2. Drive SWDATA_nOE high to isolate the translator.
3. Drive DIR1 high.
4. Keep DIR2 low.
5. Preload HOST_SWDATA_OUT with the first outgoing bit.
6. Disable the pad input path.
7. Enable the ESP32 push-pull output driver.
8. Drive SWDATA_nOE low.
9. Continue the existing SWD clock sequence.
```

Preloading is significant: the outgoing GPIO has a known value before either
the ESP32 driver or translator is allowed to drive the target node.

`CONFIG_ESP_SWD_TURNAROUND_DELAY_US` and
`CONFIG_ESP_SWD_TURNAROUND_DELAY_NS` set the total busy-wait guard as whole
microseconds plus a `0..999 ns` remainder. The first guard runs after translator `/OE`
is raised and before `DIR1` changes. During the handoff to the target, the
ESP32 output is disabled before this guard because the translator is already
isolated. The second guard runs after translator `/OE` is lowered and before
SWD clocking resumes.
The total is rounded up to CPU cycles and waited using the per-core cycle
counter while SWCLK remains low; no extra clock cycle is created. Setting both
fields to zero compiles out both guards.

## ACK and data direction

Every transfer begins with a host-generated request followed by a target ACK:

```text
                  REQUEST        TA       ACK
owner             HOST                    TARGET
SWDIO          [8 request bits] [Z] [OK / WAIT / FAULT]
```

What happens next depends on the request:

```text
Read:
  HOST request -> TARGET ACK -> TARGET 32-bit data + parity -> HOST idle

Write:
  HOST request -> TARGET ACK -> HOST 32-bit data + parity -> HOST idle
```

For write transfers, the first data bit is calculated before host drive is
enabled. For WAIT/FAULT handling, a configured dummy write phase preloads zero;
otherwise the host parks SWDIO high.

## SWCLK and BOOT0 translator

The SWCLK/BOOT translator is always A-to-B. Firmware initializes both host values before lowering its
shared `/OE`:

```text
HOST_SWCLK  = low
HOST_SWBOOT = low
SWCLK_nOE   = low only after both GPIOs are configured
```

Because SWCLK and BOOT0 share `/OE`, disabling the SWCLK interface also makes
BOOT0 high impedance. Conversely, using SWCLK necessarily drives whatever
level is present on `HOST_SWBOOT`. Normal SWD operation therefore holds BOOT0
low.

## Reset circuit

Rev 6 reset is not a direct nRESET output. It is an N-channel MOSFET pull-down:

```text
                           target board

ESP HOST_SW_RST --100R--+---- gate
                        |       Q2 AO3400A
                       100k       drain -------- TARGET_RST
                        |         source
                       GND          |
                                   GND

TARGET_RST ---------------- target reset input and target-side pull-up
```

Therefore the ESP-side polarity is inverted relative to the target signal:

```text
HOST_SW_RST low  -> Q2 off -> TARGET_RST released/high
HOST_SW_RST high -> Q2 on  -> TARGET_RST asserted/low
```

The translated backend hides this inversion behind the CMSIS-DAP nRESET GPIO
hooks.

## ESP32-S3 dedicated GPIO

Enable the fast backend with:

```text
CONFIG_ESP_SWD_PHY_AXC2T245=y
CONFIG_ESP_SWD_USE_DEDICATED_GPIO=y
```

The component creates:

```text
output bundle: [HOST_SWCLK, HOST_SWDATA_OUT]
input bundle:  [HOST_SWDATA_IN]
```

The hot path uses the ESP32-S3 instructions wrapped by:

```c
dedic_gpio_cpu_ll_write_mask(...);
dedic_gpio_cpu_ll_read_in();
```

Generated object code has been verified to contain:

```text
ee.wr_mask_gpio_out
ee.get_gpio_in
```

The driver queries each bundle's allocated channel offset. It does not assume
that dedicated channel zero is available.

Dedicated GPIO belongs to the CPU core which allocated it. Every subsequent
operation must run on that same core. In Soul Injector, the flasher task is
pinned to core 1 when this backend is enabled. `swd_esp_port_init()` rejects an
unpinned caller instead of allowing intermittent failures after task migration.

Once a pad is routed to dedicated GPIO, regular `GPIO.out_w1ts/out_w1tc` writes
are not used for its output value. The ESP pad output-enable bit is still
changed during SWDIO turnaround to make `HOST_SWDATA_OUT` high impedance.

SWCLK pacing uses the per-core CPU cycle counter. This is required for the
dedicated backend because an empty C delay loop can be removed by the compiler,
leaving dedicated GPIO clock writes back-to-back regardless of the configured
frequency. The generated slow path should contain `rsr.ccount` instructions on
both sides of each SWCLK edge.

Setting `CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=-1` enables the fast transfer path.
`CONFIG_ESP_SWD_FAST_DELAY_NOPS` inserts a compile-time number of NOP
instructions after each SWCLK transition in that path. Set it to zero for
genuinely back-to-back clock writes. The setup sequences retain their minimum
cycle-counter delay so JTAG-to-SWD entry is not made as aggressive as the
transfer hot path. Runtime CMSIS-DAP clock commands can still select a paced
transfer rate. Fast-path padding does not replace or remove the separate
translator turnaround guards.

The NOP count is a tuning input, not a guaranteed time in nanoseconds. Verify
the resulting pulse widths at the target-side SWCLK pin. Dedicated GPIO can
produce pulses too short for an external link when the value is zero.

`CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS` adds `SWDATA_nOE`,
`SWDATA_DIR1`, and `SWDATA_DIR2` to the same dedicated output bundle as SWCLK
and host SWDIO. This replaces translator level writes in each ownership change
with dedicated-GPIO instructions. `SWCLK_nOE` remains ordinary GPIO because it
is only changed during setup and shutdown. The option is off by default and
should be compared with identical clock, turnaround, idle, and profiler
settings.

## Rev 6 configuration

The top-level Rev 6 demo defaults are:

```text
CONFIG_ESP_SWD_PHY_AXC2T245=y
CONFIG_ESP_SWD_CLK_PIN=6
CONFIG_ESP_SWD_DATA_OUT_PIN=8
CONFIG_ESP_SWD_DATA_IN_PIN=18
CONFIG_ESP_SWD_DATA_NOE_PIN=15
CONFIG_ESP_SWD_DATA_DIR1_PIN=17
CONFIG_ESP_SWD_DATA_DIR2_PIN=16
CONFIG_ESP_SWD_CLK_NOE_PIN=4
CONFIG_ESP_SWD_NRST_PIN=7
CONFIG_ESP_SWD_BOOT_PIN=5
CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=-1
CONFIG_ESP_SWD_FAST_DELAY_NOPS=4
CONFIG_ESP_SWD_TURNAROUND_DELAY_US=0
CONFIG_ESP_SWD_TURNAROUND_DELAY_NS=250
CONFIG_ESP_SWD_USE_DEDICATED_GPIO=y
```

The top-level Soul Injector project does not yet have an `SI_HW_REV=rev6`
choice or `sdkconfig.defaults.rev6`. Add those separately rather than replacing
the Rev 5 defaults.

## Initialization and shutdown

The translated interface starts with both translators isolated:

```text
SWDATA_nOE      high
SWCLK_nOE       high
HOST_SWCLK      low
HOST_SWDATA_OUT high
SWDATA_DIR1     high
SWDATA_DIR2     low
HOST_SWBOOT     low
HOST_SW_RST     low (reset released)
```

GPIO output latches are preloaded before the pins become outputs. Internal
pull-ups and pull-downs are disabled on translated SWD pins.

`swd_off()` keeps the dedicated GPIO routing allocated but safely disconnects
the target:

```text
SWCLK held low
both level shifters disabled
HOST_SWDATA_OUT high impedance
BOOT0 low
target reset released
```

It must not call `gpio_reset_pin()` on SWCLK or SWDOUT because doing so would
remove their dedicated-GPIO matrix routing.

## API example

The following example connects to an STM32 target, reads its DP IDCODE, and
writes 8 KiB to RAM:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <swd_host.h>

void app_main(void)
{
    static const char *TAG = "main";

    if (!swd_init_debug()) {
        ESP_LOGE(TAG, "SWD initialization failed");
        return;
    }

    uint32_t idcode = 0;
    if (!swd_read_idcode(&idcode)) {
        ESP_LOGE(TAG, "DP IDCODE read failed");
        return;
    }
    ESP_LOGI(TAG, "DP IDCODE: 0x%08" PRIx32, idcode);

    uint8_t *buf = malloc(8192);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Allocation failed");
        return;
    }
    memset(buf, 0x5a, 8192);

    const int64_t start_us = esp_timer_get_time();
    const uint8_t ok = swd_write_memory(0x20000000, buf, 8192);
    ESP_LOGI(TAG, "RAM write: ok=%u, elapsed=%" PRId64 " us",
             ok, esp_timer_get_time() - start_us);

    free(buf);
}
```

When dedicated GPIO is enabled, call the SWD API from the pinned task that
creates the bundles. Do not initialize it on one core and perform transfers on
another.

## Bring-up recommendations

Begin with the 500 kHz default. Before increasing speed, verify on the target
side of the translators:

1. SWCLK starts low and has no enable glitch.
2. BOOT0 remains low during ordinary SWD connection.
3. SWDIO is never driven simultaneously by host and target.
4. ACK values are valid and DP IDCODE reads repeatedly.
5. Reset is asserted by an ESP high level and released by an ESP low level.
6. WAIT, FAULT, parity errors and reconnects are handled consistently.

Then validate progressively at 1 MHz, 4 MHz and 8 MHz. Test the `-1` fast mode
only after the highest paced setting is stable, beginning with nonzero
fast-path padding. Translator bandwidth alone does not determine the usable SWD
rate; cable length, target timing, layout, damping, loading and firmware
turnaround all contribute.

## Current validation status

The following software checks have passed with ESP-IDF 6.0.2:

- full legacy direct-GPIO firmware build;
- translated regular-GPIO demo build;
- translated plus dedicated-GPIO component build;
- translated main-component build with the pinned flasher task; and
- disassembly verification of the dedicated-GPIO and CPU-cycle delay
  instructions.

Both translated GPIO backends are reported to communicate on Rev 6 after the
CPU-cycle pacing fix. A zero turnaround guard fails during RAM reading, while
250 ns works. The 250 ns guard compiles to 60 cycles at 240 MHz. A logic-
analyzer capture of the fast path with zero NOP padding showed only 4 ns and
5 ns observable SWCLK low pulses before the waveform stopped decoding; the
target did not reply. Four NOPs per half-cycle subsequently completed 100
verified 8 KiB write/read iterations, but the faster request cadence increased
the target WAIT rate enough that throughput did not improve.

## License

MIT
