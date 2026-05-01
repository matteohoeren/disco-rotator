# ESP32-S3 Mini Disco Ball Rotator Reference

## Overview
This document updates the disco-ball rotator design for an **ESP32-S3-Mini board with integrated OLED display**, together with a BTT TMC2209-v1.2 stepper driver, a 17HS10-0704S pancake stepper, a USB-C PD trigger board, and a Mini-360 buck converter.[cite:118][cite:121][cite:21][cite:104]

The ESP32-S3 platform is a good fit here because it combines 2.4 GHz Wi‑Fi and Bluetooth LE on one chip, while also offering enough GPIO and peripheral flexibility to drive a TMC2209 and an OLED-based status UI at the same time.[cite:121][cite:124]

## Datasheets and key specs

### ESP32-S3-MINI module
The ESP32-S3-MINI-1 module is based on the ESP32-S3 family and supports 2.4 GHz Wi‑Fi plus Bluetooth LE, with rich peripherals including UART, I2C, SPI, PWM, RMT, and USB, which makes it well suited for wireless motion control.[cite:118][cite:121]

- Primary module datasheet: [ESP32-S3-MINI-1 / MINI-1U Datasheet](https://documentation.espressif.com/esp32-s3-mini-1_mini-1u_datasheet_en.pdf) [cite:118]
- SoC family datasheet: [ESP32-S3 Series Datasheet](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf) [cite:121]
- Product overview: [ESP32-S3 Wi‑Fi & BLE 5 SoC](https://www.espressif.com/en/products/socs/esp32-s3) [cite:124]

### Integrated OLED display
The exact OLED wiring depends on the specific ESP32-S3-Mini carrier board, not just the Espressif module itself, because the MINI module datasheet does not define the attached display pins on third-party dev boards.[cite:118]

In practice, these small integrated OLED boards usually connect the OLED over I2C, commonly using an SSD1306-compatible controller at address 0x3C, and board vendors often publish the exact SDA and SCL pins separately from the ESP32-S3 module datasheet.[cite:123]

Because the board model was described only as an ESP-S3-Mini with integrated OLED display, the OLED section in this document assumes an I2C OLED and advises checking the seller page or silkscreen for the final SDA and SCL mapping before firmware is written.[cite:123][cite:129]

### TMC2209 stepper driver
The TMC2209 is a low-noise bipolar stepper driver that supports a motor supply voltage from 4.75 V to 29 V and provides UART configuration, StealthChop2, and microstep interpolation for smooth, quiet rotation.[cite:21][cite:101]

- Primary datasheet: [TMC2209 Datasheet (Analog Devices)](https://www.analog.com/media/en/technical-documentation/data-sheets/tmc2209_datasheet_rev1.09.pdf) [cite:21]
- Alternate mirror: [TMC2209 Datasheet (DigiKey mirror)](https://media.digikey.com/pdf/Data%20Sheets/Trinamic%20Motion%20Control%20PDFs/TMC2209_Rev.1.08.pdf) [cite:101]

### Mini-360 buck converter
The Mini-360 buck converter is suitable for converting a 9 V or 12 V PD rail down to 5 V for the ESP32-S3-Mini dev board, because it is specified for roughly 4.75 V to 23 V input and adjustable lower output voltage.[cite:104][cite:112]

- Datasheet / reference: [MINI-360 PDF](https://www.matts-electronics.com/wp-content/uploads/2018/06/MINI-360.pdf) [cite:104]
- Module overview: [Mini360 1.8A DC-DC Adjustable Buck Converter](https://components101.com/modules/mini360-dc-dc-buck-converter-module) [cite:112]

### USB-C PD trigger board
Your PD trigger board class negotiates a fixed USB-C PD voltage such as 9 V, 12 V, 15 V, or 20 V from a compatible source, but the delivered rail still depends on which PDOs the charger or power bank actually offers.[cite:84][cite:107][cite:110]

- Your attached PDF: USB-C PD/QC trigger board product sheet from the chat attachment.[cite:84]
- Similar module reference: [PDC004-PD USB Power Delivery Decoy Module](https://hubtronics.in/pdc004-pd-usb-power-delivery-decoy-module) [cite:110]
- Similar product page: [USB PD Trigger board module](https://punoscho.in/product/usb-pd-trigger-board/) [cite:107]

### 17HS10-0704S pancake stepper
A manufacturer datasheet for the exact 17HS10-0704S part was not clearly identified in the available results, so the safest approach is to verify the motor’s two coil pairs with a multimeter and start the TMC2209 at a conservative current limit.[cite:21]

## Wiring diagram
The clean wiring approach is to derive a main 9 V or 12 V rail from the USB-C PD trigger board, feed that rail into the TMC2209 motor supply and the Mini-360 input, and then use the Mini-360 to generate 5 V for the ESP32-S3-Mini board.[cite:84][cite:21][cite:104]

```text
USB-C PD Power Bank / Charger
            │
            ▼
   PD Trigger Board (9V or 12V)
      V+ ---------------+---------------------> TMC2209 VMOT
      GND --------------+---------------------> TMC2209 GND
                        |
                        +--> Mini-360 IN+
                        +--> Mini-360 IN-

Mini-360 OUT+ (set to 5.0V) ------------------> ESP32-S3-Mini VIN / 5V
Mini-360 OUT- --------------------------------> ESP32-S3-Mini GND

ESP32-S3 GPIOx --------------------------------> TMC2209 STEP
ESP32-S3 GPIOy --------------------------------> TMC2209 DIR
ESP32-S3 GPIOz --------------------------------> TMC2209 EN
ESP32-S3 UART TX/RX ---------------------------> TMC2209 PDN_UART
ESP32-S3 GND ----------------------------------> TMC2209 GND

TMC2209 A1/A2 ---------------------------------> Stepper coil A
TMC2209 B1/B2 ---------------------------------> Stepper coil B

ESP32-S3 I2C SDA ------------------------------> OLED SDA  (if not already onboard-wired)
ESP32-S3 I2C SCL ------------------------------> OLED SCL  (if not already onboard-wired)
```

All grounds must be common across the PD trigger board, Mini-360, ESP32-S3-Mini, and TMC2209 so that power and logic signals share the same reference.[cite:21][cite:104] The TMC2209 should also have a bulk capacitor close to VMOT and GND, because motor drivers can inject supply transients into the rail.[cite:21]

### Suggested GPIO allocation
Because the exact integrated-OLED board was not identified by vendor part number, these GPIOs should be treated as a **safe starting template**, not final board-specific pin assignments.[cite:129][cite:118]

| Function | Suggested ESP32-S3 signal | Connected device | Notes |
|---|---|---|---|
| Step pulse | GPIO4 | TMC2209 STEP | Any safe output GPIO can work.[cite:121] |
| Direction | GPIO5 | TMC2209 DIR | Invert in software if motion is reversed.[cite:121] |
| Enable | GPIO6 | TMC2209 EN | Useful for reducing idle power.[cite:21] |
| UART TX/RX | GPIO17 / GPIO18 | TMC2209 PDN_UART | Used for current and driver configuration.[cite:21][cite:121] |
| OLED SDA | Board-specific | Integrated OLED | Usually already wired on the board.[cite:123] |
| OLED SCL | Board-specific | Integrated OLED | Usually already wired on the board.[cite:123] |
| Ground | GND | Common ground | Must be shared.[cite:21] |

Avoid assigning motor-control signals to any pins already hardwired to the onboard OLED, USB, or boot strapping functions until the carrier board pinout is confirmed.[cite:118][cite:129]

## Stepper-control libraries
For an ESP32-S3 Arduino-based firmware stack, the most practical motion stack remains **AccelStepper** for step generation and **TMCStepper** for TMC2209 configuration over UART.[cite:11][cite:21]

### AccelStepper
AccelStepper is widely used to generate non-blocking step pulses and supports target speed changes, acceleration, stopping, and reversing, which matches your disco-ball control goals well.[cite:41][cite:4]

### TMCStepper
TMCStepper is the standard Arduino library used to configure Trinamic drivers such as the TMC2209, including RMS current, microstepping, and driver mode changes over UART.[cite:11][cite:21]

### BLE and Wi‑Fi libraries
The ESP32-S3 supports both Wi‑Fi and Bluetooth LE, so you can use either a BLE control path or a Wi‑Fi API without changing the hardware architecture.[cite:121][cite:124]

For BLE on Arduino, the most common choices are the ESP32 BLE stack or NimBLE-based libraries, while a small HTTP or WebSocket service is a straightforward option if Wi‑Fi control turns out to be simpler in practice.[cite:121]

### OLED libraries
If the integrated display is SSD1306-compatible over I2C, common Arduino libraries include **Adafruit_SSD1306** with **Adafruit_GFX**, or **U8g2** if you want more font and layout flexibility.[cite:123]

These libraries are useful for showing the current direction, speed, wireless status, and battery state directly on the board without needing a phone connection.[cite:123]

## Practical software stack
A sensible ESP32-S3-Mini implementation is:

- Arduino core for ESP32-S3
- BLE or Wi‑Fi command interface
- AccelStepper for motion control
- TMCStepper for TMC2209 UART setup
- Adafruit_SSD1306 or U8g2 for the integrated OLED

This split cleanly separates communication, motion, driver configuration, and display rendering, which makes the firmware easier to maintain and test.[cite:21][cite:121][cite:123]

## Implementation notes
Because your wireless changes happen infrequently, the OLED can act as a local status display while the radio stays in a lower-duty communication pattern most of the time.[cite:121][cite:123] The TMC2209 enable pin remains valuable in this updated design because it lets the motor driver reduce idle power when the disco ball is stationary.[cite:21]

Running the system from either 9 V or 12 V remains acceptable for the TMC2209 input range and for the Mini-360 input range, so the previous uncertainty around 9 V versus 12 V from the PD trigger board still does not block a first prototype.[cite:21][cite:104][cite:84]
