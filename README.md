# Disco Rotator

BLE-controlled stepper motor driver for a disco ball rotator.
A single-file web app connects over Web Bluetooth and lets you control speed, direction, and an auto mode that continuously ramps the motor at slow speeds < 300 steps/s.

**Web controller:** https://matteohoeren.github.io/disco-rotator/

---

## Hardware

| Part | Details |
|------|---------|
| Microcontroller | 01Space ESP32-C3 Mini (0.42" OLED onboard, not used) |
| Stepper driver | BigTreeTech TMC2209 v1.2 — standalone STEP/DIR mode |
| Motor | Any bipolar stepper (NEMA 17 works well) |
| Power | 12–24 V for the motor; 5 V USB for the ESP32 |

---

## Wiring

### ESP32-C3 → TMC2209

| ESP32-C3 GPIO | TMC2209 pin | Notes |
|---------------|-------------|-------|
| GPIO 0 | STEP | Step pulse |
| GPIO 1 | DIR | Direction |
| GPIO 2 | EN | Active LOW — pulled HIGH by firmware when stopped |
| GND | GND | Common ground |

### TMC2209 microstep selection

MS1 and MS2 can be left **floating or tied to GND** — both give **8 microsteps** (1 600 steps/rev for a 200-step motor).

| MS1 | MS2 | Microsteps |
|-----|-----|-----------|
| GND / float | GND / float | 8 |
| VIO | GND | 2 |
| GND / float | VIO | 4 |
| VIO | VIO | 16 |

### TMC2209 motor & power

Connect the motor coils to **A1/A2** and **B1/B2**.  
Connect your motor power supply (12–24 V) to **VM** and **GND**.  
Set the driver current via the onboard potentiometer (start low, increase until torque is sufficient).

> UART pins (PDN_UART) are not used — the driver runs in standalone mode.

---

## Firmware

Built with [PlatformIO](https://platformio.org/).

```
pio run --target upload
```

Dependencies (auto-installed by PlatformIO):
- `h2zero/NimBLE-Arduino`
- `waspinator/AccelStepper`

---

## Web controller

Open `webapp/index.html` (or the GitHub Pages URL above) in **Chrome on desktop or Android**.  
Web Bluetooth requires HTTPS — the GitHub Pages URL works out of the box.

> iOS: use [Bluefy](https://apps.apple.com/app/bluefy/id1492822055) instead of Safari/Chrome.  
> Firefox: not supported (no Web Bluetooth).

### Controls

| Control | Description |
|---------|-------------|
| Speed slider | 10–3 000 steps/sec |
| CW / CCW | Set rotation direction |
| Motor power | Enable / stop the motor |
| Auto mode | Very slowly ramps between random speeds (< 300 sps, biased toward low values). Direction changes at most every 2 minutes |

---

## BLE service

Device name: `DiscoRotator`  
Service UUID: `12345678-1234-5678-1234-56789abcdef0`

| Characteristic | UUID suffix | Type | Description |
|----------------|-------------|------|-------------|
| Speed | `...def1` | uint16 LE (write) | Steps/sec, 10–3 000 |
| Direction | `...def2` | uint8 (write) | 0 = CW, 1 = CCW |
| Enable | `...def3` | uint8 (write) | 0 = stop, 1 = run |
| Status | `...def4` | ASCII (notify) | e.g. `cw:120`, `stopped` |
| Auto | `...def5` | uint8 (write) | 0 = manual, 1 = auto |
