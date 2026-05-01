/**
 * Disco Ball Rotator Firmware
 * Board  : ESP32-C3 Mini with 0.42" OLED (SSD1306-compat, 72x40, I2C)
 * Driver : BTT TMC2209 v1.2 — standalone STEP/DIR mode (no UART)
 *          MS1/MS2 floating or GND → 8 microsteps (1600 steps/rev)
 *
 * BLE Service: "DiscoRotator"
 *   Speed     (write) : uint16 LE  steps/sec [50..20000]
 *   Direction (write) : uint8      0=CW  1=CCW
 *   Enable    (write) : uint8      0=stop  1=run
 *   Auto      (write) : uint8      0=manual  1=auto (random speed/dir every 45-180s)
 *   Status    (notify): ASCII string e.g. "cw:400" or "stopped"
 */

#include <Arduino.h>
#include <AccelStepper.h>
#include <NimBLEDevice.h>

// ─── Pin assignments ──────────────────────────────────────────────────────────
#define PIN_LED       8   // active LOW
#define PIN_STEP      0
#define PIN_DIR       1
#define PIN_EN        2   // active LOW on TMC2209

// ─── Motor config ─────────────────────────────────────────────────────────────
#define SPEED_MIN       10
#define SPEED_MAX       3000
#define SPEED_DEFAULT   400
#define ACCEL_RAMP          200   // steps/sec² — manual mode ramp
#define AUTO_ACCEL_RAMP     3     // steps/sec² — auto mode: very slow, ambient ramp

// ─── BLE UUIDs ───────────────────────────────────────────────────────────────
#define BLE_SERVICE_UUID    "12345678-1234-5678-1234-56789abcdef0"
#define BLE_SPEED_UUID      "12345678-1234-5678-1234-56789abcdef1"
#define BLE_DIR_UUID        "12345678-1234-5678-1234-56789abcdef2"
#define BLE_ENABLE_UUID     "12345678-1234-5678-1234-56789abcdef3"
#define BLE_STATUS_UUID     "12345678-1234-5678-1234-56789abcdef4"
#define BLE_AUTO_UUID       "12345678-1234-5678-1234-56789abcdef5"
#define BLE_AUTO_MIN_UUID   "12345678-1234-5678-1234-56789abcdef6"
#define BLE_AUTO_MAX_UUID   "12345678-1234-5678-1234-56789abcdef7"

// ─── Globals ─────────────────────────────────────────────────────────────────
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);

volatile uint16_t targetSpeed  = SPEED_DEFAULT;
volatile bool     motorEnabled = false;
volatile bool     directionCCW = false;
volatile bool     autoMode     = false;
volatile uint16_t autoSpeedMin = 10;
volatile uint16_t autoSpeedMax = 299;

float currentSpeed = 0.0f;

bool  pendingDirFlip  = false;
bool  pendingDirValue = false;

NimBLEServer*         bleServer    = nullptr;
NimBLECharacteristic* charStatus   = nullptr;
bool                  bleConnected = false;

unsigned long lastStatusUpdate = 0;
unsigned long lastAutoChange   = 0;   // unused in new auto logic, kept for compat
uint32_t      autoIntervalMs   = 60000;
unsigned long lastDirChange    = 0;   // auto mode: direction cooldown

#define STATUS_INTERVAL_MS  500
#define RAMP_INTERVAL_US   5000

// ─── Motor ramp ───────────────────────────────────────────────────────────────
void applyMotorSpeed() {
    if (!motorEnabled || currentSpeed < 1.0f) {
        stepper.setSpeed(0);
        if (!motorEnabled && currentSpeed < 1.0f) digitalWrite(PIN_EN, HIGH);
        return;
    }
    digitalWrite(PIN_EN, LOW);
    stepper.setSpeed(directionCCW ? -currentSpeed : currentSpeed);
}

void updateRamp() {
    static unsigned long lastUs = 0;
    unsigned long nowUs = micros();
    if (nowUs - lastUs < (unsigned long)RAMP_INTERVAL_US) return;
    float dt = (nowUs - lastUs) * 1e-6f;
    lastUs = nowUs;

    float ramp = autoMode ? (float)AUTO_ACCEL_RAMP : (float)ACCEL_RAMP;
    float step = ramp * dt;

    if (pendingDirFlip) {
        if (currentSpeed <= step) {
            currentSpeed   = 0.0f;
            directionCCW   = pendingDirValue;
            pendingDirFlip = false;
        } else {
            currentSpeed -= step;
        }
    } else {
        float target = motorEnabled ? (float)targetSpeed : 0.0f;
        if (fabsf(currentSpeed - target) <= step) currentSpeed = target;
        else if (currentSpeed < target)            currentSpeed += step;
        else                                       currentSpeed -= step;
    }
    applyMotorSpeed();
}

void requestDirection(bool ccw) {
    if (ccw == directionCCW) return;
    if (currentSpeed < 1.0f) { directionCCW = ccw; return; }
    pendingDirFlip  = true;
    pendingDirValue = ccw;
}

// ─── Auto mode ────────────────────────────────────────────────────────────────
// Ramps between random speed targets using AUTO_ACCEL_RAMP (3 sps²).
// Speed range is set via BLE (autoSpeedMin / autoSpeedMax).
// Speed is biased toward lower values: two random draws, take the min.
// Direction may flip only after a 2-minute cooldown.
#define AUTO_DIR_COOLDOWN   120000UL  // 2 minutes in ms

void autoModeUpdate() {
    if (!autoMode || !motorEnabled) return;

    // When currentSpeed has settled at targetSpeed, pick a new target.
    if (fabsf(currentSpeed - (float)targetSpeed) < 1.0f) {
        uint16_t lo = autoSpeedMin;
        uint16_t hi = autoSpeedMax;
        if (lo >= hi) lo = (hi > 1) ? hi - 1 : 0;
        // Two draws, take the min → biased toward lower speeds
        uint16_t a = (uint16_t)random(lo, hi + 1);
        uint16_t b = (uint16_t)random(lo, hi + 1);
        targetSpeed = min(a, b);

        // Flip direction only if cooldown has elapsed
        if (millis() - lastDirChange >= AUTO_DIR_COOLDOWN) {
            if (random(2) == 0) {
                requestDirection(!directionCCW);
                lastDirChange = millis();
                Serial.printf("[AUTO] dir flip → %s\n", !directionCCW ? "CCW" : "CW");
            }
        }
        Serial.printf("[AUTO] new target=%u sps (range %u-%u)\n", targetSpeed, lo, hi);
    }
}

// ─── BLE callbacks ────────────────────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override {
        bleConnected = true;
        digitalWrite(PIN_LED, LOW);
        Serial.println("BLE connected");
    }
    void onDisconnect(NimBLEServer*) override {
        bleConnected = false;
        digitalWrite(PIN_LED, HIGH);
        NimBLEDevice::startAdvertising();
        Serial.println("BLE disconnected");
    }
};

class SpeedCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 2) {
            uint16_t val = 0;
            memcpy(&val, c->getValue().data(), 2);
            targetSpeed = constrain(val, (uint16_t)SPEED_MIN, (uint16_t)SPEED_MAX);
            Serial.printf("Speed: %u sps\n", targetSpeed);
        }
    }
};

class DirCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 1) {
            requestDirection(c->getValue()[0] != 0);
            Serial.printf("Dir: %s\n", directionCCW ? "CCW" : "CW");
        }
    }
};

class EnableCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 1) {
            motorEnabled = (c->getValue()[0] != 0);
            if (motorEnabled) digitalWrite(PIN_EN, LOW);
            Serial.printf("Motor: %s\n", motorEnabled ? "ON" : "OFF");
        }
    }
};

class AutoCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 1) {
            autoMode = (c->getValue()[0] != 0);
            if (autoMode) {
                lastAutoChange = millis();
                lastDirChange  = millis();  // reset dir cooldown on enable
                autoIntervalMs = (uint32_t)random(45000, 180001);
            }
            Serial.printf("Auto: %s\n", autoMode ? "ON" : "OFF");
        }
    }
};

class AutoMinCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 2) {
            uint16_t val = 0;
            memcpy(&val, c->getValue().data(), 2);
            val = constrain(val, (uint16_t)SPEED_MIN, (uint16_t)(SPEED_MAX - 1));
            autoSpeedMin = val;
            if (autoSpeedMin >= autoSpeedMax) autoSpeedMax = autoSpeedMin + 1;
            Serial.printf("Auto min: %u sps\n", autoSpeedMin);
        }
    }
};

class AutoMaxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (c->getDataLength() >= 2) {
            uint16_t val = 0;
            memcpy(&val, c->getValue().data(), 2);
            val = constrain(val, (uint16_t)(SPEED_MIN + 1), (uint16_t)SPEED_MAX);
            autoSpeedMax = val;
            if (autoSpeedMax <= autoSpeedMin) autoSpeedMin = autoSpeedMax - 1;
            Serial.printf("Auto max: %u sps\n", autoSpeedMax);
        }
    }
};

static void addDescription(NimBLECharacteristic* c, const char* desc) {
    NimBLEDescriptor* d = c->createDescriptor("2901", NIMBLE_PROPERTY::READ, strlen(desc) + 1);
    d->setValue(desc);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setupStepper() {
    pinMode(PIN_STEP, OUTPUT);
    pinMode(PIN_DIR,  OUTPUT);
    pinMode(PIN_EN,   OUTPUT);
    digitalWrite(PIN_EN, HIGH);
    stepper.setMaxSpeed(SPEED_MAX);
    stepper.setAcceleration(ACCEL_RAMP);  // used only if switching to run() API
    stepper.setSpeed(0);
    Serial.println("Stepper ready");
}

void setupBLE() {
    NimBLEDevice::init("DiscoRotator");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(false, false, false);
    NimBLEDevice::deleteAllBonds();

    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    NimBLEService* svc = bleServer->createService(BLE_SERVICE_UUID);

    auto* cSpeed = svc->createCharacteristic(BLE_SPEED_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    cSpeed->setCallbacks(new SpeedCallback());
    addDescription(cSpeed, "Speed (steps/sec, 50-20000)");
    uint16_t defSpd = SPEED_DEFAULT; cSpeed->setValue(defSpd);

    auto* cDir = svc->createCharacteristic(BLE_DIR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    cDir->setCallbacks(new DirCallback());
    addDescription(cDir, "Direction (0=CW, 1=CCW)");
    uint8_t defDir = 0; cDir->setValue(defDir);

    auto* cEnable = svc->createCharacteristic(BLE_ENABLE_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    cEnable->setCallbacks(new EnableCallback());
    addDescription(cEnable, "Enable (0=stop, 1=run)");
    uint8_t defEn = 0; cEnable->setValue(defEn);

    auto* cAuto = svc->createCharacteristic(BLE_AUTO_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    cAuto->setCallbacks(new AutoCallback());
    addDescription(cAuto, "Auto mode (0=manual, 1=auto)");
    uint8_t defAuto = 0; cAuto->setValue(defAuto);

    auto* cAutoMin = svc->createCharacteristic(BLE_AUTO_MIN_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    cAutoMin->setCallbacks(new AutoMinCallback());
    addDescription(cAutoMin, "Auto min speed (steps/sec)");
    uint16_t defAutoMin = 10; cAutoMin->setValue(defAutoMin);

    auto* cAutoMax = svc->createCharacteristic(BLE_AUTO_MAX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::READ);
    cAutoMax->setCallbacks(new AutoMaxCallback());
    addDescription(cAutoMax, "Auto max speed (steps/sec)");
    uint16_t defAutoMax = 299; cAutoMax->setValue(defAutoMax);

    charStatus = svc->createCharacteristic(BLE_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
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
    digitalWrite(PIN_LED, HIGH);
    setupStepper();
    setupBLE();
    Serial.println("Boot complete");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void sendBLEStatus() {
    if (!bleConnected || !charStatus) return;
    char buf[32];
    int len;
    if (!motorEnabled)
        len = snprintf(buf, sizeof(buf), "stopped");
    else
        len = snprintf(buf, sizeof(buf), "%s%s:%u",
            autoMode ? "auto:" : "",
            directionCCW ? "ccw" : "cw",
            (unsigned)currentSpeed);
    // Pass explicit length so NimBLE doesn't send the full buffer as garbage
    charStatus->setValue((uint8_t*)buf, len);
    charStatus->notify();
}

void loop() {
    updateRamp();
    if (currentSpeed >= 1.0f) stepper.runSpeed();
    autoModeUpdate();
    if (millis() - lastStatusUpdate >= STATUS_INTERVAL_MS) {
        lastStatusUpdate = millis();
        sendBLEStatus();
    }
}
