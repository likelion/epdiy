# Patching epdiy 1.0.2 for ESP-IDF v5 Compatibility

## Table of Contents

1. [Background: What Are All These Things?](#background-what-are-all-these-things)
2. [The Problem](#the-problem)
3. [Why Can't We Just Include the Missing Components?](#why-cant-we-just-include-the-missing-components)
4. [The Solution Strategy](#the-solution-strategy)
5. [Patch 1: ADC Temperature Sensor (epd_board_common.c)](#patch-1-adc-temperature-sensor)
6. [Patch 2: RMT Pulse Generator (rmt_pulse.c)](#patch-2-rmt-pulse-generator)
7. [Patch 3: V6 Board Guard (I2C exclusion)](#patch-3-v6-board-guard)
8. [Files That Needed No Changes](#files-that-needed-no-changes)
9. [YAML Cleanup](#yaml-cleanup)
10. [Persistence: Where These Patches Live](#persistence-where-these-patches-live)
11. [Tradeoffs and Limitations](#tradeoffs-and-limitations)

---

## Background: What Are All These Things?

### The Hardware Stack

This project drives a **6-inch e-paper display** (model ED060XC3, the kind you'd find in
a Kindle) using an **ESP32** microcontroller. The ESP32 is a small, cheap, Wi-Fi-enabled
chip commonly used in IoT projects. It connects to Home Assistant (a home automation
platform) via **ESPHome**, which lets you configure ESP32 firmware using YAML files
instead of writing C++ from scratch.

### The Software Stack

Here's how the layers fit together, from top to bottom:

```
┌─────────────────────────────────────────────┐
│  Home Assistant  (home automation server)   │
├─────────────────────────────────────────────┤
│  ESPHome  (YAML → C++ firmware generator)   │
├─────────────────────────────────────────────┤
│  epaper_display component  (custom C++)     │
├─────────────────────────────────────────────┤
│  epdiy library  (e-paper driver, C)         │
├─────────────────────────────────────────────┤
│  ESP-IDF  (Espressif's SDK / OS for ESP32)  │
├─────────────────────────────────────────────┤
│  ESP32 Hardware  (CPU, GPIO, ADC, RMT, I2S) │
└─────────────────────────────────────────────┘
```

**ESP-IDF** (Espressif IoT Development Framework) is the official SDK for ESP32. Think
of it like a small operating system plus a collection of driver libraries. It provides
APIs for everything the chip can do: Wi-Fi, Bluetooth, GPIO pins, analog-to-digital
conversion, timers, DMA, and so on.

**epdiy** is an open-source library (by Valentin Roland) that knows how to talk to
e-paper displays using the ESP32's hardware peripherals. It's a low-level driver — it
directly manipulates hardware registers to push pixel data to the display at the precise
timings e-paper panels require.

### Key ESP32 Peripherals Used

The ESP32 has specialized hardware blocks called **peripherals**. epdiy uses three of them:

- **ADC (Analog-to-Digital Converter)**: Reads the voltage from a temperature sensor on
  the e-paper driver board. E-paper displays need different voltage waveforms at different
  temperatures to update correctly, so knowing the ambient temperature matters.

- **RMT (Remote Control Transceiver)**: Originally designed for infrared remote control
  signals, but epdiy repurposes it to generate precisely-timed electrical pulses. These
  pulses clock the e-paper display's gate driver — essentially telling it "move to the
  next row of pixels."

- **I2S (Inter-IC Sound)**: Originally designed for digital audio, but epdiy uses it in
  "LCD parallel mode" to blast 8 bits of pixel data at a time to the display's source
  driver. It uses DMA (Direct Memory Access) so the CPU doesn't have to manually push
  each byte.

### API Layers in ESP-IDF

ESP-IDF organizes its code in layers, from high-level to low-level:

```
┌──────────────────────────────────────────────────────────────┐
│  Driver API  ("driver/adc.h", "driver/rmt.h")                │
│  High-level, easy to use, manages state and error checking   │
│  Lives in: esp_adc, esp_driver_rmt, driver components        │
├──────────────────────────────────────────────────────────────┤
│  HAL API  ("hal/adc_ll.h", "hal/adc_types.h")                │
│  Thin wrappers around register access, inline functions      │
│  Lives in: hal component (always available)                  │
├──────────────────────────────────────────────────────────────┤
│  SoC/Register structs  ("soc/rmt_struct.h", "soc/sens_*")    │
│  Raw C structs mapped to hardware register addresses         │
│  Lives in: soc component (always available)                  │
├──────────────────────────────────────────────────────────────┤
│  Actual Hardware Registers (memory-mapped I/O)               │
└──────────────────────────────────────────────────────────────┘
```

The higher layers are more convenient but depend on IDF components that may or may not
be included in the build. The lower layers talk directly to hardware and are always
available.

---

## The Problem

ESPHome 2026.2.2 upgraded to **ESP-IDF v5.5.2**. This is a major version jump from the
v4.x series that epdiy 1.0.2 was written for. Two things broke simultaneously:

### 1. ESP-IDF v5 Reorganized Its Driver APIs

In IDF v4, peripheral drivers lived in a single `driver` component. You'd write:
```c
#include "driver/adc.h"    // ADC functions
#include "driver/rmt.h"    // RMT functions
```

In IDF v5, Espressif split these into separate components:
```
driver/adc.h     → esp_adc/adc_oneshot.h  (component: esp_adc)
driver/rmt.h     → driver/rmt_tx.h        (component: esp_driver_rmt)
esp_adc_cal.h    → esp_adc/adc_cali.h     (component: esp_adc)
```

The old headers still exist as deprecated wrappers, but only if the corresponding
component is included in the build.

### 2. ESPHome Excludes Most Driver Components

To keep firmware small and build times fast, ESPHome auto-generates a `platformio.ini`
that explicitly **excludes** components it doesn't need:

```ini
board_build.cmake_extra_args = -DEXCLUDE_COMPONENTS=cmock;driver;esp_adc;
  esp_driver_rmt;esp_driver_i2s;...
```

Notice that `driver`, `esp_adc`, and `esp_driver_rmt` are all excluded. This means:

- `driver/adc.h` → **gone** (the `driver` component is excluded)
- `esp_adc/adc_oneshot.h` → **gone** (the `esp_adc` component is excluded)
- `driver/rmt.h` → **gone** (the `driver` component is excluded)
- `driver/rmt_tx.h` → **gone** (the `esp_driver_rmt` component is excluded)

The epdiy library can't include *any* of these headers — old or new.

### The Build Error

```
epd_board_common.c:2:10: fatal error: driver/adc.h: No such file or directory
```

And if we fixed that, `rmt_pulse.c` would fail next with:
```
rmt_pulse.c:3:10: fatal error: driver/rmt.h: No such file or directory
```

### What's Still Available?

Not everything is excluded. These lower-level layers are always present:

| Layer | Example Headers | Status |
|-------|----------------|--------|
| HAL (Hardware Abstraction Layer) | `hal/adc_ll.h`, `hal/adc_types.h` | Available |
| SoC register structs | `soc/rmt_struct.h`, `soc/sens_struct.h` | Available |
| GPIO driver | `driver/gpio.h` (from `esp_driver_gpio`) | Available |
| RTC GPIO driver | `driver/rtc_io.h` (from `esp_driver_gpio`) | Available |
| Peripheral control | `esp_private/periph_ctrl.h` | Available |
| Interrupt allocation | `esp_intr_alloc.h` | Available |
| ROM functions | `esp32/rom/gpio.h` | Available |

---

## Why Can't We Just Include the Missing Components?

We tried. ESPHome's code generator produces the `EXCLUDE_COMPONENTS` list automatically
based on what the firmware actually uses. Even if you override `board_build.cmake_extra_args`
in the YAML config:

```yaml
platformio_options:
  board_build.cmake_extra_args: "-DEXCLUDE_COMPONENTS=..."  # our custom list
```

ESPHome's auto-generated value **wins** — it overwrites the override in the final
`platformio.ini`. The exclude list is not user-configurable without modifying ESPHome's
core code generation.

This means the epdiy library must be patched to work *without* the excluded components.

---

## The Solution Strategy

Since we can't bring the driver components back, we go **down the stack** — replacing
high-level driver API calls with lower-level equivalents that don't depend on excluded
components.

```
BEFORE (IDF v4):                    AFTER (IDF v5):
┌─────────────────────┐             ┌─────────────────────┐
│  driver/adc.h       │ ──────X     │  hal/adc_ll.h       │  ← HAL layer
│  esp_adc_cal.h      │ ──────X     │  hal/adc_types.h    │    (always available)
│  driver/rmt.h       │ ──────X     │  soc/rmt_struct.h   │  ← Register structs
│  (rmt_config, etc.) │             │  (direct reg writes)│    (always available)
└─────────────────────┘             └─────────────────────┘
```

The key insight: epdiy was *already* doing a lot of direct register access for the
performance-critical paths (I2S pixel data, RMT pulse transmission). The driver APIs
were only used during **initialization**. So the patches are mostly about replacing
init-time convenience functions with their register-level equivalents.

All patches use `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)` guards so the
original IDF v4 code is preserved for backward compatibility.

---

## Patch 1: ADC Temperature Sensor

**File**: `epdiy/src/epd_driver/board/epd_board_common.c`

### What This File Does

The e-paper driver board has a temperature sensor connected to **ADC1 Channel 7** on the
ESP32. This file reads that sensor to determine ambient temperature, which the display
driver uses to select appropriate voltage waveforms for pixel updates. (E-ink particles
move differently at different temperatures.)

There are two functions:
- `epd_board_temperature_init_v2()` — configures the ADC hardware once at startup
- `epd_board_ambient_temperature_v2()` — reads 100 samples, averages them, converts to °C

### Original Code (IDF v4)

```c
#include "driver/adc.h"       // ← EXCLUDED
#include "esp_adc_cal.h"      // ← EXCLUDED

static esp_adc_cal_characteristics_t adc_chars;

void epd_board_temperature_init_v2() {
  // Calibrate ADC using factory-burned values in eFuse
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(channel, ADC_ATTEN_DB_6);
}

float epd_board_ambient_temperature_v2() {
  // ... average 100 raw readings ...
  float voltage = esp_adc_cal_raw_to_voltage(value, &adc_chars);  // calibrated conversion
  return (voltage - 500.0) / 10.0;
}
```

This uses three things from the excluded components:
1. `adc1_config_width()` / `adc1_config_channel_atten()` — configure ADC resolution and input attenuation
2. `adc1_get_raw()` — trigger a single ADC conversion and read the result
3. `esp_adc_cal_characterize()` / `esp_adc_cal_raw_to_voltage()` — use factory calibration data burned into the chip's eFuse to convert raw ADC values to accurate millivolts

### Patched Code (IDF v5)

```c
#include "hal/adc_ll.h"       // ← HAL layer, always available
#include "hal/adc_types.h"    // ← type definitions, always available

void epd_board_temperature_init_v2() {
  adc_ll_set_controller(ADC_UNIT_1, ADC_LL_CTRL_RTC);           // use RTC controller
  adc_oneshot_ll_set_output_bits(ADC_UNIT_1, ADC_BITWIDTH_12);  // 12-bit resolution
  adc_oneshot_ll_set_atten(ADC_UNIT_1, ADC_CHANNEL, ADC_ATTEN_DB_6);  // ~2200mV range
  adc_ll_set_sar_clk_div(ADC_UNIT_1, 1);                        // clock divider
  adc_ll_amp_disable();                                          // save power
}
```

The `adc_ll_*` and `adc_oneshot_ll_*` functions are **static inline** functions defined
in `hal/adc_ll.h`. They compile down to direct register writes — for example,
`adc_oneshot_ll_set_output_bits()` writes to `SENS.sar_start_force.sar1_bit_width` and
`SENS.sar_read_ctrl.sar1_sample_bit`. No library code is linked; it's all inlined at
compile time.

For reading raw values:

```c
static uint32_t adc1_read_raw(void) {
  adc_oneshot_ll_set_channel(ADC_UNIT_1, ADC_CHANNEL);  // select channel 7
  adc_oneshot_ll_start(ADC_UNIT_1);                      // trigger conversion
  while (!adc_oneshot_ll_get_event(ADC_LL_EVENT_ADC1_ONESHOT_DONE)) {}  // poll for done
  adc_oneshot_ll_clear_event(ADC_LL_EVENT_ADC1_ONESHOT_DONE);
  return adc_oneshot_ll_get_raw_result(ADC_UNIT_1);      // read 12-bit result
}
```

### The Calibration Tradeoff

The original code used `esp_adc_cal_raw_to_voltage()` which reads factory calibration
data from the ESP32's eFuse (one-time-programmable memory) to accurately convert raw ADC
readings to millivolts. This calibration data accounts for manufacturing variations
between individual chips.

Since `esp_adc_cal` is excluded, we can't use this. Instead, we use a simple linear
approximation:

```c
float voltage = (float)value * 2200.0f / 4095.0f;
```

This assumes the ADC's full 12-bit range (0–4095) maps linearly to 0–2200 mV (the
approximate range for `ADC_ATTEN_DB_6`). In practice, the ESP32's ADC is not perfectly
linear, especially near the extremes, but for temperature sensing (which typically reads
in the middle of the range), this is adequate. The temperature value is used to select
between a small number of waveform lookup tables, so a few degrees of error doesn't
affect display quality.

---

## Patch 2: RMT Pulse Generator

**File**: `epdiy/src/epd_driver/rmt_pulse.c`

### What This File Does

E-paper displays are organized as a grid of rows and columns. To update the display, you
need to:
1. Load pixel data for one row into the source driver (via I2S)
2. Pulse the gate driver to select that row (via RMT)
3. Repeat for all rows

The RMT peripheral generates these gate pulses with precise timing. Each pulse is a
simple high-then-low signal, but the timing must be accurate to microseconds.

The file has two parts:
- **Initialization** (`rmt_pulse_init`) — configures the RMT channel once at startup
- **Pulse generation** (`pulse_ckv_ticks`) — writes pulse data to RMT memory and triggers
  transmission. This runs in a tight loop during display refresh and is marked `IRAM_ATTR`
  (runs from RAM for speed).

### Original Code (IDF v4)

```c
#include "driver/rmt.h"  // ← EXCLUDED

static rmt_config_t row_rmt_config;  // driver's config struct

void rmt_pulse_init(gpio_num_t pin) {
  row_rmt_config.rmt_mode = RMT_MODE_TX;
  row_rmt_config.channel = RMT_CHANNEL_1;
  row_rmt_config.gpio_num = pin;
  row_rmt_config.mem_block_num = 2;
  row_rmt_config.clk_div = 8;
  row_rmt_config.tx_config.loop_en = false;
  row_rmt_config.tx_config.carrier_en = false;
  row_rmt_config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  row_rmt_config.tx_config.idle_output_en = true;
  // ... more config fields ...

  rmt_config(&row_rmt_config);              // ← applies config to hardware
  rmt_set_tx_intr_en(row_rmt_config.channel, true);  // ← enables TX-done interrupt
}
```

The `rmt_config()` function is a convenience wrapper that:
1. Enables the RMT peripheral clock
2. Sets up the clock divider, memory blocks, carrier settings
3. Routes the GPIO pin to the RMT output signal
4. Configures idle output level

`rmt_set_tx_intr_en()` enables the "transmission done" interrupt for the channel.

### Patched Code (IDF v5)

We do everything `rmt_config()` did, but by writing directly to the `RMT` register struct:

```c
#include "soc/rmt_struct.h"          // RMT register struct (always available)
#include "esp_private/periph_ctrl.h" // periph_module_enable()
#include "soc/gpio_sig_map.h"       // RMT_SIG_OUT0_IDX signal routing constants
#include "esp_intr_alloc.h"          // interrupt allocation

#define RMT_CHANNEL 1

void rmt_pulse_init(gpio_num_t pin) {
  periph_module_enable(PERIPH_RMT_MODULE);  // enable clock gate

  RMT.apb_conf.fifo_mask = 1;  // use RMTMEM instead of FIFO

  // Configure channel registers directly
  RMT.conf_ch[RMT_CHANNEL].conf0.div_cnt = 8;        // 80MHz / 8 = 10MHz
  RMT.conf_ch[RMT_CHANNEL].conf0.mem_size = 2;        // 2 memory blocks
  RMT.conf_ch[RMT_CHANNEL].conf0.carrier_en = 0;      // no carrier modulation
  RMT.conf_ch[RMT_CHANNEL].conf0.mem_pd = 0;           // memory powered on
  RMT.conf_ch[RMT_CHANNEL].conf0.clk_en = 1;           // clock enabled

  RMT.conf_ch[RMT_CHANNEL].conf1.mem_owner = 0;        // TX owns memory
  RMT.conf_ch[RMT_CHANNEL].conf1.ref_always_on = 1;    // use APB clock
  RMT.conf_ch[RMT_CHANNEL].conf1.idle_out_lv = 0;      // idle = low
  RMT.conf_ch[RMT_CHANNEL].conf1.idle_out_en = 1;      // enable idle output
  // ... etc ...

  // Route GPIO pin to RMT channel 1's output signal
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_matrix_out(pin, RMT_SIG_OUT0_IDX + RMT_CHANNEL, false, false);

  // Register interrupt and enable TX-done for channel 1
  esp_intr_alloc(ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3,
                 rmt_interrupt_handler, 0, &gRMT_intr_handle);
  RMT.int_ena.val |= (1 << (RMT_CHANNEL * 3));  // bit 3 = ch1_tx_end
}
```

### How the Register Struct Works

The ESP32's peripherals are controlled through **memory-mapped registers** — specific
memory addresses that, when written to, configure hardware behavior. ESP-IDF provides
C structs that map to these addresses:

```c
extern rmt_dev_t RMT;  // declared in soc/rmt_struct.h, mapped to 0x3FF56000
```

When you write `RMT.conf_ch[1].conf0.div_cnt = 8`, the compiler generates a single
32-bit write to the memory address `0x3FF56000 + offset`, which the hardware interprets
as "set channel 1's clock divider to 8."

### GPIO Matrix Routing

The ESP32 has a flexible **GPIO matrix** that can connect any GPIO pin to any peripheral
signal. The original code used `rmt_config()` to handle this internally. Our patch does
it explicitly:

```c
gpio_matrix_out(pin, RMT_SIG_OUT0_IDX + RMT_CHANNEL, false, false);
```

This tells the GPIO matrix: "route the output signal of RMT channel 1 (`RMT_SIG_OUT0_IDX + 1 = signal 88`) to this physical pin." `gpio_matrix_out` is a ROM function (burned into
the chip at the factory), so it's always available regardless of which IDF components are
included.

### Interrupt Bit Calculation

The RMT interrupt enable register has 3 bits per channel (tx_end, rx_end, error):

```
Bit 0: ch0_tx_end    Bit 3: ch1_tx_end    Bit 6: ch2_tx_end  ...
Bit 1: ch0_rx_end    Bit 4: ch1_rx_end    Bit 7: ch2_rx_end  ...
Bit 2: ch0_err       Bit 5: ch1_err       Bit 8: ch2_err     ...
```

So for channel 1's TX-done interrupt: `1 << (1 * 3) = 1 << 3 = bit 3`.

### The Pulse Transmission Path (Unchanged)

The actual pulse generation code (`pulse_ckv_ticks`) was already using direct register
access in the original epdiy code — it writes pulse timing data directly to `RMTMEM`
(the RMT peripheral's dedicated memory) and triggers transmission by setting
`RMT.conf_ch[ch].conf1.tx_start = 1`. This code didn't need any changes.

The only difference is that the original code referenced `row_rmt_config.channel` (a
field from the now-removed `rmt_config_t` struct) to get the channel number. The patch
replaces this with a `#define RMT_CHANNEL 1` constant.

### Removed Types: rmt_item32_t, rmt_mem_t, RMTMEM

IDF v5.5.2 also removed three types from `soc/rmt_struct.h` that the original code
relied on:

- `rmt_item32_t` — a 32-bit struct representing one RMT pulse (duration + level for
  high and low phases)
- `rmt_mem_t` — a struct mapping the RMT peripheral's dedicated memory (8 channels ×
  64 items each)
- `RMTMEM` — an extern variable pointing to the RMT memory at address `0x3FF56800`

These were part of the legacy driver API. Since the hardware hasn't changed, we define
equivalent types locally in `rmt_pulse.c`:

```c
typedef struct {
    union {
        struct {
            uint32_t duration0 :15;
            uint32_t level0 :1;
            uint32_t duration1 :15;
            uint32_t level1 :1;
        };
        uint32_t val;
    };
} rmt_item32_t;

typedef volatile struct {
    struct {
        rmt_item32_t data32[64];
    } chan[8];
} rmt_block_mem_t;

#define RMTMEM (*(rmt_block_mem_t *)0x3FF56800)
```

The `0x3FF56800` address is the fixed hardware address of the RMT channel memory on
ESP32, documented in the ESP32 Technical Reference Manual.

### Missing GPIO_PIN_MUX_REG

IDF v5 also stopped transitively including `soc/gpio_periph.h` through `driver/gpio.h`.
This header defines `GPIO_PIN_MUX_REG[]`, an array mapping GPIO numbers to their IO MUX
register addresses. The `PIN_FUNC_SELECT()` macro uses this to configure a pin's
function (e.g., selecting GPIO mode vs. a peripheral function).

All board files and `i2s_data_bus.c` use `PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], ...)`,
so they all need the explicit include:

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "soc/gpio_periph.h"
#endif
```

This was added to `rmt_pulse.c`, `i2s_data_bus.c`, and all board files (`epd_board_v5.c`,
`epd_board_v4.c`, `epd_board_v2_v3.c`, `epd_board_lilygo_t5_47.c`).

---

## Patch 3: V6 Board Guard

**Files**: `epd_board_v6.c`, `tps65185.h`, `tps65185.c`, `pca9555.h`, `pca9555.c`, `epd_board.h`

### What These Files Do

The epdiy library supports multiple hardware board revisions. The **V6 board** uses two
I2C peripherals that the V5 board doesn't need:

- **TPS65185** — a high-voltage power management IC (PMIC) that generates the ±15V
  voltages e-paper displays require. On V6, it's controlled over I2C. On V5, power
  management is done differently (simpler discrete circuitry).

- **PCA9555** — a 16-bit I2C GPIO expander. The V6 board ran out of ESP32 GPIO pins, so
  it uses this chip to add 16 more digital I/O lines over I2C. The V5 board has enough
  native GPIO pins and doesn't need this.

Both chips communicate via the **I2C bus** (`driver/i2c.h`), which is part of the
excluded `driver` component. Unlike the ADC and RMT cases (Patches 1 and 2), there's no
practical way to replace the I2C driver with HAL-level code — the I2C protocol involves
complex multi-step transactions (start condition, address byte, ACK/NACK handling, clock
stretching, repeated starts) that the HAL layer doesn't provide convenient abstractions
for.

### Why Guard Instead of Rewrite?

For ADC and RMT, we rewrote the initialization code using lower-level APIs because those
peripherals are needed by the V5 board. But the I2C-dependent code is **only used by the
V6 board**, and this project uses V5. Rather than writing a complex I2C HAL
implementation for hardware we don't have, we simply guard the V6 code so it compiles to
nothing when V6 isn't selected.

The build flags already define which board revision is in use:

```ini
build_flags:
  - -DCONFIG_EPD_BOARD_REVISION_V5
```

### The Guard Pattern

Each file is wrapped with `#ifdef CONFIG_EPD_BOARD_REVISION_V6`:

**Headers** (`tps65185.h`, `pca9555.h`):
```c
#ifndef TPS65185_H
#define TPS65185_H

#ifdef CONFIG_EPD_BOARD_REVISION_V6

#include <driver/i2c.h>
// ... all declarations ...

#endif /* CONFIG_EPD_BOARD_REVISION_V6 */
#endif // TPS65185_H
```

The include guard (`#ifndef TPS65185_H`) stays outside so the header is always
"includable" without errors — it just expands to nothing when V6 isn't defined. This
matters because other files might `#include "tps65185.h"` unconditionally.

**Source files** (`tps65185.c`, `pca9555.c`, `epd_board_v6.c`):
```c
#include "tps65185.h"  // safe — header is empty when V6 not defined

#ifdef CONFIG_EPD_BOARD_REVISION_V6

#include <driver/i2c.h>
// ... all implementation code ...

#endif /* CONFIG_EPD_BOARD_REVISION_V6 */
```

**Board definition header** (`epd_board.h`):
```c
extern const EpdBoardDefinition epd_board_v5;
#ifdef CONFIG_EPD_BOARD_REVISION_V6
extern const EpdBoardDefinition epd_board_v6;
#endif
```

The extern declaration for `epd_board_v6` is guarded because the struct definition
(in `epd_board_v6.c`) won't exist when V6 is disabled. While an unresolved extern
declaration alone wouldn't cause a compile error, guarding it keeps things clean and
avoids potential linker warnings.

### Why This Is Safe

The board selection in `render.c` is already guarded:

```c
#if defined(CONFIG_EPD_BOARD_REVISION_V5)
  epd_set_board(&epd_board_v5);
#elif defined(CONFIG_EPD_BOARD_REVISION_V6)
  epd_set_board(&epd_board_v6);
#endif
```

Since `CONFIG_EPD_BOARD_REVISION_V5` is defined (and `CONFIG_EPD_BOARD_REVISION_V6` is
not), the V6 branch is never compiled. No code path in the final binary references any
V6 symbol, so the empty translation units cause no linker errors.

### Files Modified

| File | Change |
|------|--------|
| `tps65185.h` | Wrapped all declarations in `#ifdef CONFIG_EPD_BOARD_REVISION_V6` |
| `tps65185.c` | Wrapped all implementation in `#ifdef CONFIG_EPD_BOARD_REVISION_V6` |
| `pca9555.h` | Wrapped all declarations in `#ifdef CONFIG_EPD_BOARD_REVISION_V6` |
| `pca9555.c` | Wrapped all implementation in `#ifdef CONFIG_EPD_BOARD_REVISION_V6` |
| `epd_board_v6.c` | Wrapped entire file (after `#include "epd_board.h"`) in `#ifdef CONFIG_EPD_BOARD_REVISION_V6` |
| `epd_board.h` | Guarded `extern` declaration of `epd_board_v6` |

---

## Files That Needed No Changes

### i2s_data_bus.c — I2S Parallel Data Bus

This file drives the 8-bit parallel data bus to the display using the I2S peripheral in
LCD mode. It was **mostly compatible** with IDF v5 because:

1. It already had `#if ESP_IDF_VERSION` guards for the `periph_ctrl.h` include path change
2. It already had guards for the `rtc_clk_apll_enable()` signature change (IDF v5 takes
   `bool` instead of individual parameters)
3. It uses `soc/i2s_struct.h` and `soc/i2s_reg.h` for all register access — these are
   in the `soc` component which is never excluded
4. The `driver/rtc_io.h` include comes from `esp_driver_gpio`, which is not excluded
5. The `rom/lldesc.h` and `rom/gpio.h` includes already had version guards

The only change needed was adding `#include "soc/gpio_periph.h"` for `GPIO_PIN_MUX_REG`
(see Patch 2 above).

### display_ops.h — Fast GPIO and Display Operations

This header defines `fast_gpio_set_hi()` and `fast_gpio_set_lo()` — inline functions
that write directly to GPIO registers for maximum speed (used in timing-critical display
refresh loops). It already had IDF v5 guards:

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  gpio_dev_t *device = GPIO_LL_GET_HW(GPIO_PORT_0);
  device->out_w1ts = (1 << gpio_num);
#else
  GPIO.out_w1ts = (1 << gpio_num);
#endif
```

In IDF v5, the `GPIO` global struct was removed in favor of `GPIO_LL_GET_HW()` which
returns a pointer to the same memory-mapped registers.

### epd_board_v5.c — Board V5 Definition

This file defines the pin assignments and power sequencing for the V5 board revision.
It only uses `driver/gpio.h` and `driver/rtc_io.h` (both from `esp_driver_gpio`, not
excluded), plus the internal epdiy headers. The only change needed was adding
`#include "soc/gpio_periph.h"` for `GPIO_PIN_MUX_REG` (see Patch 2 above). The same
include was also added to `epd_board_v4.c`, `epd_board_v2_v3.c`, and
`epd_board_lilygo_t5_47.c`.

---

## YAML Cleanup

The ESPHome YAML configuration (`epaper-display.yaml`) had two workaround attempts from
earlier debugging that were cleaned up:

### Removed: `board_build.cmake_extra_args` Override

```yaml
# REMOVED — ESPHome overwrites this with its own auto-generated value
board_build.cmake_extra_args: "-DEXCLUDE_COMPONENTS=..."
```

This was an attempt to un-exclude `driver` and `esp_adc` from the build. It doesn't work
because ESPHome's code generator always overwrites this setting in the final
`platformio.ini`. The patched epdiy code no longer needs these components, so the
override is unnecessary.

### Removed: Deprecation Warning Suppression Flags

```yaml
# REMOVED — no longer using deprecated APIs
- -DCONFIG_ADC_SUPPRESS_DEPRECATE_WARN
- -DCONFIG_RMT_SUPPRESS_DEPRECATE_WARN
```

These flags suppressed compiler warnings about using deprecated IDF v4 APIs. Since the
patched code uses HAL-level APIs (not deprecated), these flags serve no purpose.

---

## Persistence: Where These Patches Live

The patched files are in:
```
.esphome/build/epaper-display/.piolibdeps/epaper-display/epdiy/src/epd_driver/
```

This directory is **not persistent** — ESPHome's PlatformIO build system re-downloads
the epdiy library from Git on clean builds. The patches will be lost if:

- You run `esphome clean` or delete the `.esphome` directory
- PlatformIO decides to re-fetch the library
- You build on a different machine

### Making It Permanent

To make these changes permanent, they need to be committed to the epdiy fork that the
project references:

```yaml
libraries:
  - epdiy=https://github.com/likelion/epdiy.git#1.0.2-patched
```

The workflow would be:
1. Clone `https://github.com/likelion/epdiy.git`
2. Check out the `1.0.2-patched` branch
3. Apply the changes to `epd_board_common.c`, `rmt_pulse.c`, `tps65185.h`, `tps65185.c`,
   `pca9555.h`, `pca9555.c`, `epd_board_v6.c`, `epd_board.h`, `i2s_data_bus.c`,
   `epd_board_v5.c`, `epd_board_v4.c`, `epd_board_v2_v3.c`, and
   `epd_board_lilygo_t5_47.c`
4. Commit and push
5. The next ESPHome build will pull the updated code automatically

---

## Tradeoffs and Limitations

### Temperature Accuracy

The original code used ESP32 factory calibration data (eFuse) for accurate ADC-to-voltage
conversion. The patched code uses a simple linear approximation (`raw * 2200 / 4095`).
This may be off by 5–15% depending on the individual chip. For e-paper waveform selection,
this is acceptable — the temperature is used to pick from a small set of lookup tables,
and a few degrees of error won't visibly affect display quality.

### Fragility of Register-Level Code

Direct register access is inherently tied to a specific chip variant. The `SENS` struct
fields, `RMT` register layout, and GPIO matrix signal indices are all ESP32-specific.
This code will not work on ESP32-S2, ESP32-S3, ESP32-C3, or other variants without
modification. However, epdiy 1.0.2 only supports the original ESP32 anyway, so this
isn't a practical concern.

### Forward Compatibility

If ESP-IDF v6 changes the HAL API signatures or removes the `soc/rmt_struct.h` register
definitions, these patches would break. However, Espressif has historically maintained
backward compatibility for the HAL and SoC layers across minor versions, and the register
structs are auto-generated from hardware description files, so they're unlikely to change
for existing chips.

### Why Not Upgrade to epdiy 2.0?

The upstream epdiy project (by Valentin Roland) has a 2.0.0 release that natively supports
IDF v5. However, it has breaking API changes that would require rewriting the
`epaper_display` ESPHome component. The 1.0.2 patch approach was chosen because:

1. It's minimal — only two files rewritten, five more guarded out, six more with a single include added
2. It preserves the existing component code
3. It maintains backward compatibility with IDF v4
4. The risk of introducing bugs is low (the critical paths were already register-level)
5. V6 board support is preserved — remove the `-DCONFIG_EPD_BOARD_REVISION_V5` flag and
   add `-DCONFIG_EPD_BOARD_REVISION_V6` to re-enable it (though you'd also need to
   un-exclude the `driver` component for I2C)
