/**
 * Disco Ball Rotator Firmware
 * Board  : ESP32-C3 Mini with 0.42" OLED (SSD1306-compat, 72x40, I2C)
 * Driver : BTT TMC2209 v1.2
 * Motor  : 17HS10-0704S pancake stepper
 *
 * Confirmed pin assignments (researched from 01Space board schematics):
 *   OLED SDA = GPIO5, OLED SCL = GPIO6
 *   Onboard LED = GPIO8 (active LOW)
 *   Boot button = GPIO9
 *
 * BLE Service: "DiscoRotator"
 *   Speed     (write) : uint16 LE  steps/sec [50..3000]
 *   Direction (write) : uint8      0=CW  1=CCW
 *   Enable    (write) : uint8      0=stop  1=run
 *   Status    (notify): ASCII string e.g. "cw:400" or "stopped"
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <AccelStepper.h>
#include <TMCStepper.h>
#include <NimBLEDevice.h>

// ─── Pin assignments ──────────────────────────────────────────────────────────
// OLED – confirmed for 01Space ESP32-C3 0.42" board
#define OLED_SDA      5
#define OLED_SCL      6

// Onboard LED (active LOW)
#define PIN_LED       8

// TMC2209 step/dir/enable
#define PIN_STEP      0   // GPIO0
#define PIN_DIR       1   // GPIO1
#define PIN_EN        2   // GPIO2  (active LOW on TMC2209)

// TMC2209 UART – half-duplex on UART1
// Connect GPIO3 → TMC2209 PDN_UART via 1kΩ resistor
#define TMC_UART_TX   3
#define TMC_UART_RX   3
#define TMC_UART_PORT Serial1
#define TMC_ADDR      0

// ─── OLED config ─────────────────────────────────────────────────────────────
// The 0.42" SSD1306 on this board has a 72x40 physical display but reports
// as 128x64 internally. Use offsets to address the visible region.
// Use U8G2_SSD1306_128X64 constructor (no native 72x40 constructor needed
// since U8g2 >= 2.28 has U8G2_SSD1306_72X40_ER but HW I2C works fine this way)
#define OLED_X_OFFSET 30   // (128 - 72) / 2 = 28, but 30 works best in practice
#define OLED_Y_OFFSET 12   // (64  - 40) / 2 = 12
#define OLED_W        72
#define OLED_H        40

// Use no-pin constructor — pins are set via Wire.begin() below
// Matches the working example exactly
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, /* reset= */ U8X8_PIN_NONE);

// ─── Motor / driver config ────────────────────────────────────────────────────
#define MOTOR_RMS_MA        500    // 17HS10-0704S is 0.7A/phase; start conservative
#define MICROSTEPS          8      // 1600 steps/rev
#define STEPS_PER_REV       (200 * MICROSTEPS)

#define SPEED_MIN           50
#define SPEED_MAX           3000
#define SPEED_DEFAULT       400
#define ACCEL_DEFAULT       200

// ─── BLE UUIDs ───────────────────────────────────────────────────────────────
#define BLE_SERVICE_UUID    "12345678-1234-5678-1234-56789abcdef0"
#define BLE_SPEED_UUID      "12345678-1234-5678-1234-56789abcdef1"
#define BLE_DIR_UUID        "12345678-1234-5678-1234-56789abcdef2"
#define BLE_ENABLE_UUID     "12345678-1234-5678-1234-56789abcdef3"
#define BLE_STATUS_UUID     "12345678-1234-5678-1234-56789abcdef4"

// ─── Globals ─────────────────────────────────────────────────────────────────
TMC2209Stepper driver(&TMC_UART_PORT, 0.11f, TMC_ADDR);
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);

volatile uint16_t targetSpeed  = SPEED_DEFAULT;
volatile bool     motorEnabled = false;
volatile bool     directionCCW = false;
bool              driverOk     = false;

NimBLEServer*         bleServer  = nullptr;
NimBLECharacteristic* charStatus = nullptr;
bool                  bleConnected = false;

unsigned long lastOledUpdate = 0;
#define OLED_INTERVAL_MS 500

// ─── Motor state application ──────────────────────────────────────────────────
void applyMotorState() {
    if (!driverOk || !motorEnabled) {
        stepper.stop();
        stepper.setSpeed(0);
        digitalWrite(PIN_EN, HIGH);
        if (motorEnabled && !driverOk) {
            Serial.println("Motor enable ignored: TMC2209 not detected");
        }
    } else {
        digitalWrite(PIN_EN, LOW);
        float speed = (float)targetSpeed;
        if (directionCCW) speed = -speed;
        stepper.setMaxSpeed(fabsf(speed));
        stepper.setAcceleration(ACCEL_DEFAULT);
        stepper.setSpeed(speed);
    }
}

// ─── OLED ─────────────────────────────────────────────────────────────────────
void updateOled() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);

    // Row 1 (y=8): title
    u8g2.drawStr(0, 8, "DISCO BALL");

    // Row 2 (y=18): BLE status
    u8g2.drawStr(0, 18, bleConnected ? "BLE: CONN" : "BLE: WAIT");

    // Row 3 (y=28): motor / driver state
    if (!driverOk) {
        u8g2.drawStr(0, 28, "DRV: NONE");
    } else if (!motorEnabled) {
        u8g2.drawStr(0, 28, "STOPPED");
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%s %usps", directionCCW ? "CCW" : "CW ", (unsigned)targetSpeed);
        u8g2.drawStr(0, 28, buf);
    }

    // Row 4 (y=38): speed bar when running
    if (driverOk && motorEnabled) {
        int barLen = map(targetSpeed, SPEED_MIN, SPEED_MAX, 0, OLED_W - 2);
        u8g2.drawFrame(0, 31, OLED_W, 7);
        u8g2.drawBox(1, 32, barLen, 5);
    }

    u8g2.sendBuffer();
}

// ─── BLE callbacks ────────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override {
        bleConnected = true;
        Serial.println("BLE connected");
        digitalWrite(PIN_LED, LOW);
        updateOled();
    }
    void onDisconnect(NimBLEServer*) override {
        bleConnected = false;
        Serial.println("BLE disconnected – restarting advertising");
        digitalWrite(PIN_LED, HIGH);
        NimBLEDevice::startAdvertising();
        updateOled();
    }
};

class SpeedCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 2) {
            uint16_t val = 0;
            memcpy(&val, c->getValue().data(), 2);
            targetSpeed = constrain(val, (uint16_t)SPEED_MIN, (uint16_t)SPEED_MAX);
            applyMotorState();
            Serial.printf("Speed: %u sps\n", targetSpeed);
        }
    }
};

class DirCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 1) {
            directionCCW = (c->getValue()[0] != 0);
            applyMotorState();
            Serial.printf("Dir: %s\n", directionCCW ? "CCW" : "CW");
        }
    }
};

class EnableCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 1) {
            motorEnabled = (c->getValue()[0] != 0);
            applyMotorState();
            Serial.printf("Motor: %s\n", motorEnabled ? "ON" : "OFF");
        }
    }
};

// Helper: add a "User Description" descriptor (UUID 0x2901) so nRF Connect
// shows human-readable names instead of "Unknown Characteristic"
static void addDescription(NimBLECharacteristic* c, const char* desc) {
    NimBLEDescriptor* d = c->createDescriptor(
        "2901",
        NIMBLE_PROPERTY::READ,
        strlen(desc) + 1
    );
    d->setValue(desc);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setupOled() {
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!u8g2.begin()) {
        Serial.println("OLED init failed");
    } else {
        u8g2.setBusClock(400000);
        u8g2.setContrast(255);
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(0, 18, "Booting...");
        u8g2.sendBuffer();
        Serial.println("OLED OK");
    }
}

void setupDriver() {
    TMC_UART_PORT.begin(115200, SERIAL_8N1, TMC_UART_RX, TMC_UART_TX);
    delay(100);

    driver.begin();
    delay(50);

    uint8_t ver = driver.version();
    Serial.printf("TMC2209: version register = 0x%02X\n", ver);

    if (ver == 0x21) {
        driver.toff(5);
        driver.rms_current(MOTOR_RMS_MA);
        driver.microsteps(MICROSTEPS);
        driver.en_spreadCycle(false);   // StealthChop2
        driver.pwm_autoscale(true);
        driverOk = true;
        Serial.println("TMC2209: OK");
    } else {
        driverOk = false;
        Serial.println("TMC2209: NOT FOUND (version mismatch) – motor disabled");
        Serial.println("         Check wiring: GPIO3 -> 1k -> PDN_UART");
    }
}

void setupStepper() {
    pinMode(PIN_STEP, OUTPUT);
    pinMode(PIN_DIR,  OUTPUT);
    pinMode(PIN_EN,   OUTPUT);
    digitalWrite(PIN_EN, HIGH);  // keep disabled until driver confirmed
    stepper.setMaxSpeed(SPEED_DEFAULT);
    stepper.setAcceleration(ACCEL_DEFAULT);
    stepper.setSpeed(0);
    Serial.println("Stepper pins configured");
}

void setupBLE() {
    NimBLEDevice::init("DiscoRotator");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // No bonding/pairing — avoids nRF Connect "stale bond" immediate disconnect
    NimBLEDevice::setSecurityAuth(false, false, false);
    NimBLEDevice::deleteAllBonds();

    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    NimBLEService* svc = bleServer->createService(BLE_SERVICE_UUID);

    // Speed: write uint16 LE (steps/sec, 50–3000)
    auto* cSpeed = svc->createCharacteristic(
        BLE_SPEED_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ
    );
    cSpeed->setCallbacks(new SpeedCallback());
    addDescription(cSpeed, "Speed (steps/sec, 50-3000)");
    uint16_t defSpd = SPEED_DEFAULT;
    cSpeed->setValue(defSpd);

    // Direction: write uint8 (0=CW, 1=CCW)
    auto* cDir = svc->createCharacteristic(
        BLE_DIR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ
    );
    cDir->setCallbacks(new DirCallback());
    addDescription(cDir, "Direction (0=CW, 1=CCW)");
    uint8_t defDir = 0;
    cDir->setValue(defDir);

    // Enable: write uint8 (0=stop, 1=run)
    auto* cEnable = svc->createCharacteristic(
        BLE_ENABLE_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ
    );
    cEnable->setCallbacks(new EnableCallback());
    addDescription(cEnable, "Enable (0=stop, 1=run)");
    uint8_t defEn = 0;
    cEnable->setValue(defEn);

    // Status: notify ASCII string
    charStatus = svc->createCharacteristic(
        BLE_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    addDescription(charStatus, "Status (read/notify)");
    charStatus->setValue("stopped");

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();
    Serial.println("BLE advertising as 'DiscoRotator'");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Disco Ball Rotator ===");

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);  // off at boot

    setupOled();
    setupDriver();
    setupStepper();
    setupBLE();

    updateOled();
    Serial.println("Boot complete");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void sendBLEStatus() {
    if (!bleConnected || !charStatus) return;
    char buf[24];
    if (!motorEnabled) {
        snprintf(buf, sizeof(buf), "stopped");
    } else {
        snprintf(buf, sizeof(buf), "%s:%u", directionCCW ? "ccw" : "cw", (unsigned)targetSpeed);
    }
    charStatus->setValue(buf);
    charStatus->notify();
}

void loop() {
    if (driverOk && motorEnabled) {
        stepper.runSpeed();
    }

    unsigned long now = millis();
    if (now - lastOledUpdate >= OLED_INTERVAL_MS) {
        lastOledUpdate = now;
        updateOled();
        sendBLEStatus();
    }
}
