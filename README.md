# DTS Peripheral Driver

This module provides an abstraction layer for the **digital temperature sensor** (`DTS`) of STM32 MCUs: the measurement of the die
temperature in degrees Celsius with the factory calibration applied, the software, the periodic and the hardware triggered
measurements, the low and the high **temperature thresholds** with the interrupt and the wake-up from the low-power modes. It is a
low-level MCAL module: it owns the sensor hardware, the application and the other modules never touch the registers.

The public interface (`Dts_Port.h`, `Dts_Types.h`) is **identical for all supported families**. The implementation
for a family lives in its own branch (`Dev/<Family>` for development, `Releases/<Family>` for the released versions), so
a module or an application written against the interface does not change when the MCU family changes.

This README is the landing page and the **specification of the interface** (master). The family branches contain their
own README with everything that is specific to the family: the instances of the device, their settings and the status of
the implementation.

> **Status: design.** No implementation exists yet, the interface below is a proposal. Stories and branches follow.

---

## Intent and purpose

- The die temperature protects the product (a thermal shutdown of a motor driver, a throttling of the clock), compensates the sensors and the
  oscillators (the drift of the RC oscillators and of the analog front end with the temperature) and logs the operating conditions. Older
  families measure it through an internal channel of the ADC; the newest ones have a **dedicated sensor** that works without the ADC, in the low-power
  modes, and can wake the MCU when the temperature crosses a threshold.
- The sensor is a ring oscillator whose frequency depends on the temperature; the hardware counts the oscillator periods during a number of the
  reference clock periods, and the **count is converted to the temperature with the calibration data** stored in the device at the factory
  (the frequency at the reference temperature and the ramp coefficient). The module reads the calibration, runs the measurement and returns the
  temperature in milli-degrees Celsius (`dts_Temperature_mC_t`), so the application never sees the counts.
- The measurement can be started by the software (once, `Dts_Set_MeasurementStart`), periodically by the module (`Dts_Set_PeriodicActive`, a period in ms) or by
  a hardware trigger (a low-power timer output or an external signal). With the **quick** mode from the 32.768 kHz clock the sensor works in the
  low-power modes and the threshold event wakes the MCU.
- The module hides the differences between the families: the dedicated sensor against the ADC internal channel, the calibration data and
  their location, the reference clock and the trigger sources differ, the interface stays the same.

What the module is **not**: it does not measure the temperature of the board or the ambient (it is the die temperature, a few degrees above
the ambient and dependent on the load), and it does not compensate other peripherals (the application or a module does that using the value).

---

## Concept

| Item | Meaning |
|---|---|
| **Reference clock** | `DTS_CLK_SRC_PCLK` (a fast and accurate measurement while running) or `DTS_CLK_SRC_LSE` (the quick measurement in the low-power modes). |
| **Sampling time** | The number of the reference periods of the measurement; a longer time gives a finer resolution and a longer measurement. |
| **Trigger** | `DTS_TRIGGER_SOFTWARE` or a hardware source (`DTS_TRIGGER_LPTIM1_OUT`, `DTS_TRIGGER_EXTI` ... - only the sources of the device). |
| **Thresholds** | The low and the high threshold in milli-degrees; the events: temperature below the low, above the high. |
| **Calibration** | The factory frequency at the reference temperature and the ramp coefficient read from the device; the module uses them to convert a count. |

Rules:

1. `Dts_Init` reads the calibration (an error when it is missing or invalid - the temperature would be wrong), configures the clock, the
   sampling time, the trigger and the thresholds and leaves the sensor idle. The calibration data live in the one-time-programmable / system memory
   area of the device, which must not be read through the instruction cache: the module reads them through the Flash module (with the Cache module's
   suspend where the family needs it).
2. `Dts_Set_MeasurementStart` starts one measurement; the result is available when `Dts_Get_State` is `DTS_STATE_READY` or by the `MeasurementCallback`;
   `Dts_Get_Temperature( &temperature_mC )` returns the converted value. A read while a measurement is running returns an error.
3. The periodic measurement uses the trigger of the configuration or the module's own timing in `Dts_Task`; each result raises the callback.
4. The threshold events are reported by the `ThresholdCallback` with the direction (`DTS_THRESHOLD_LOW` / `DTS_THRESHOLD_HIGH`); the wake-up from the
   low-power mode works with the LSE reference only.
5. The sensor needs a **start-up and a settling time** after the enable; the module waits for the ready flag with a timeout.
6. All parameters are checked before a register is written; the thresholds must satisfy `low < high`.

---

## Supported families

Every family gets its own story, its own tasks and its own branches `Dev/<Family>` and `Releases/<Family>` cut from this
master.

| Family | Branch | Sensor | Status |
|---|---|---|---|
| STM32H5 | `Dev/STM32H5` | `DTS`: software and four hardware triggers, programmable sampling time, the counter synchronized to the LSE or the PCLK, the low and the high temperature threshold with the interrupt, the asynchronous wake-up (LSE mode), the quick measurement from the LSE, factory calibration | planned (first) |
| STM32U5 | `Dev/STM32U5` | device dependent (the internal temperature channel of the `Adc` module on the lines without the sensor) | planned |
| STM32H7 | `Dev/STM32H7` | `DTS` on the lines that have it | planned |
| STM32U3 | `Dev/STM32U3` | device dependent | planned |
| STM32F4, STM32F7, STM32G4, STM32L4 | `Dev/STM32F4` ... | no dedicated sensor (the temperature is an internal channel of the Adc module) - the module is a stub that returns `DTS_REQUEST_ERROR` | planned |

A function without a meaning in a family stays in the interface, returns `DTS_REQUEST_ERROR` and suppresses the unused
parameters (for example the wake-up threshold on a device without it).

---

## Public interface in short (proposal)

| Group | Functions |
|---|---|
| Module management | `Dts_Get_ModuleVersion`, `Dts_Init`, `Dts_Deinit`, `Dts_Task`, `Dts_Get_DefaultConfig` |
| Measurement | `Dts_Set_MeasurementStart`, `Dts_Get_State`, `Dts_Get_Temperature`, `Dts_Set_PeriodicActive`, `Dts_Set_PeriodicInactive` |
| Settings | `Dts_Set_ClkSrc` / `Get`, `Dts_Set_SamplingTime` / `Get`, `Dts_Set_Trigger` / `Get` |
| Thresholds | `Dts_Set_Thresholds`, `Dts_Get_Thresholds`, `Dts_Set_ThresholdIrqActive`, `Dts_Set_ThresholdIrqInactive` |
| Interrupts | `Dts_Set_IrqPriority` / `Get`, the callbacks are a part of the configuration |

```c
dts_ModuleVersion_t Dts_Get_ModuleVersion        ( void );

dts_RequestState_t  Dts_Init                     ( const dts_Config_t * const dtsConfig );
dts_RequestState_t  Dts_Deinit                   ( void );
void                Dts_Task                     ( void );
dts_RequestState_t  Dts_Get_DefaultConfig        ( dts_Config_t * const dtsConfig );

dts_RequestState_t  Dts_Set_MeasurementStart     ( void );
dts_RequestState_t  Dts_Get_State                ( dts_State_t * const dtsState );
dts_RequestState_t  Dts_Get_Temperature          ( dts_Temperature_mC_t * const temperature );
dts_RequestState_t  Dts_Set_PeriodicActive       ( dts_Period_ms_t period );

dts_RequestState_t  Dts_Set_Thresholds           ( dts_Temperature_mC_t lowThreshold, dts_Temperature_mC_t highThreshold );
dts_RequestState_t  Dts_Set_ThresholdIrqActive   ( void );
```

Every function returns `DTS_REQUEST_OK` / `DTS_REQUEST_ERROR` and checks its parameters and the state of the module
before it touches a register.

| Type | Meaning |
|---|---|
| `dts_Config_t` | Reference clock, sampling time, trigger, thresholds, the periodic period, callbacks, interrupt priority |
| `dts_Temperature_mC_t` | Temperature in milli-degrees Celsius (signed) |
| `dts_State_t` | `DTS_STATE_DISABLED`, `DTS_STATE_STARTING`, `DTS_STATE_IDLE`, `DTS_STATE_MEASURING`, `DTS_STATE_READY` |
| Callbacks | `MeasurementCallback( temperature )`, `ThresholdCallback( direction )` |

---

## Usage

### One measurement

```c
#include "Dts_Port.h"

dts_Config_t         dtsConfig;
dts_Temperature_mC_t temperature = 0;
dts_State_t          dtsState    = DTS_STATE_DISABLED;

(void)Dts_Get_DefaultConfig( &dtsConfig );
(void)Dts_Init( &dtsConfig );

(void)Dts_Set_MeasurementStart();

for( uint32_t tryIdx = 0u; ( DTS_STATE_READY != dtsState ) && ( 10000u > tryIdx ); tryIdx++ )
{
    (void)Dts_Get_State( &dtsState );
}

(void)Dts_Get_Temperature( &temperature );               /* e.g. 41250 = 41.25 degrees Celsius */
```

### An over-temperature wake-up in the Stop mode

```c
dtsConfig.ClkSrc = DTS_CLK_SRC_LSE;
(void)Dts_Init( &dtsConfig );
(void)Dts_Set_Thresholds( -10000, 85000 );                /* -10 ... 85 degrees Celsius */
(void)Dts_Set_ThresholdIrqActive();
(void)Dts_Set_PeriodicActive( 1000u );                    /* the quick LSE measurement every second */
```

---

## Behaviour and limits

- **Accuracy.** The accuracy is that of the calibration (a few degrees over the whole range); the dedicated sensor is not a precision thermometer.
  A single measurement is noisy - average several of them for a control loop.
- **Die temperature.** The value follows the dissipation of the chip; the temperature just after the wake-up from the Stop mode is not the
  temperature of the run mode.
- **Calibration data.** A device without valid calibration data cannot report the temperature; the module returns an error instead of a wrong value.
- **Threshold wake-up.** It works with the LSE reference and the periodic measurement; the power cost is the periodic activity of the sensor.
- **Not part of the first version:** the temperature compensation of other peripherals, the logging of the temperature history, the
  two-point recalibration by the user.

---

## Dependencies

- **Rcc** - peripheral clock, the LSE / PCLK reference, reset
- **Flash** - the read of the factory calibration data (the Cache module suspends the instruction cache around it)
- **Nvic** and **Exti** - interrupt and the wake-up line of the threshold
- **LpTim** - an optional hardware trigger source
- **RAL (Register Abstraction Layer)** - used internally to access the registers

All mandatory RCC, NVIC and EXTI configurations are handled internally.

---

## Testing

- **Unit tests** (host, Unity + CMock + RegMem): the conversion of the counts to the temperature for the calibration values (the ends of the range, the
  negative temperatures), the invalid or the missing calibration, the measurement state machine, the thresholds ordering, the periodic
  mode, the callbacks, the rejected requests.
- **Integration tests** (target, probe-rs): the temperature read-out compared with an independent sensor or the ADC channel at room temperature and
  after a self-heating load, the threshold interrupt, the wake-up from the Stop mode.

---

## Interface conventions

The module follows the common rules of the MCAL layer, so that it behaves like the other modules:

- **Lists instead of free members.** Everything that the hardware restricts (the pins, the kernel clock sources, the DMA
  channels, the triggers and the other selectable sources) is a `typedef enum` list with exactly the items that are valid
  for the device line; a pin is an item `DTS_<SIGNAL>_PIN_<INSTANCE>_P<port><pin>` and there is an `*_UNUSED` item. A choice
  that is not in the list cannot be written.
- **Own peripheral only.** The module calls only the registers / LL functions of its own peripheral. The clocks, the
  interrupts, the pins, the DMA and the EXTI lines go through the API of the Rcc, Nvic, Gpio, Gpdma and Exti modules; a
  missing function is added to that module, it is not bypassed.
- **Init verifies itself.** `Dts_Init` checks the result at the end; when it is not as required, the module is
  deinitialized and an error is returned. The waiting for the hardware is a loop on the register state with a timeout,
  never a blind delay.
- **Transfer modes.** Where the module moves data, the POLL (served by `Dts_Task`), ISR and DMA modes have the same
  configuration structure, the same request and the same callbacks (error, half transfer, transfer complete).
- **Flags and states are enums of the module** (`dts_FunctionState_t`, `dts_FlagState_t`), the limits and the constants are
  named, there are no magic numbers in the interface.
- **Unsupported functions stay.** A function without a meaning in a family keeps its signature, suppresses the unused
  parameters and returns `DTS_REQUEST_ERROR`.
- **Documentation.** Every function and type has a Doxygen description, the main header has the `\defgroup`.

---

## Repository layout and branches

The master holds this README and the license. The family branches (`Dev/<Family>`, `Releases/<Family>`) hold:

| Path | Content |
|---|---|
| `README.md` | README of the family: the instances of the device, specifics, errata, status |
| `Dts_Port.h`, `Dts_Types.h` | Public interface (identical in all families) |
| `Dts*.c`, `Dts.h` | Implementation of the family |
| `CMakeLists.txt` | Library `Dts_Lib` of the EmBi platform |
| `Tests/UnitTests` | Unit tests (Unity + CMock + RegMem) |
| `Tests/IntegrationTests` | Integration tests on the target (probe-rs) |

A change of the public interface is made in all families.

---

## CMake integration

```cmake
target_link_libraries( User_Lib PRIVATE Dts_Lib )
```

1. Link `Dts_Lib` to your CMake library.
2. Include `Dts_Port.h` in your project.
3. Configure the module as needed for your hardware.

---

## License

This project is licensed under the **Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**.

You are free to use, modify, and share this work for **non-commercial purposes**, provided appropriate credit is given.

See [LICENSE.md](LICENSE.md) for full terms or visit [creativecommons.org/licenses/by-nc/4.0](https://creativecommons.org/licenses/by-nc/4.0/).

---

## Authors

- **Mr.Nobody** - [embedbits.com](https://embedbits.com)

Contributions are welcome! Please open a pull request.
