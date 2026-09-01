// ========================================================================
// ESP32 Egg Incubator Controller - Professional Version
// ========================================================================
// الوظائف: 
//   - عد تنازلي للدورة (24 ساعة - FIX: كان التعليق يذكر خطأ "21 دقيقة"
//     رغم أن TARGET_SEC = 24*60*60 ثانية فعلياً)
//   - تحكم في السخان (Hysteresis Control)
//   - تحكم في المقلب (60s OFF / 15s ON)
//   - مروحة تعمل باستمرار
//   - قراءة حساسات DS18B20 (2 حساس)
//   - قراءة حساس DHT22 (رطوبة + حرارة)
//   - شاشة LCD (16x2)
//   - Watchdog Timer (8 ثوانٍ)
//   - حفظ في NVS (كل 10 ثوانٍ)
//   - Safe Mode (عند فشل الحساسات)
//   - Emergency Mode (عند ارتفاع الحرارة فوق 39.8°C)
//   - كشف عطل الريلاي (Relay Stuck)
//   - أمر Reset عبر Serial ("RESET")
// ========================================================================

#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
// #include <LiquidCrystal_I2C.h>
#include <LiquidCrystal_I2C.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <Arduino.h>
#include <Wire.h>
#include "diag_io.h"   // Phase 4+ : cloud I/O, predictive, cycle archive, Telegram
// #include "email_reporter.h"
#include "diagnostics.h"
#include "telegram_alerts.h"
// Firmware identity (Serial / support)
#define FIRMWARE_VERSION     "2.0.1"
#define FIRMWARE_NAME        "ESP32 Egg Incubator"

// ========================================================================
// 1. التعريفات الثابتة (Configuration)
// ========================================================================

// ---- الأزمنة ----
#define WDT_TIMEOUT         30      // Task WDT (s) — after safe reconfigure
#define SENSOR_INTERVAL      2000    // قراءة الحساسات كل 2 ثانية
#define TURNER_OFF_TIME      60000   // 60 ثانية واقف
#define TURNER_ON_TIME       15000   // 15 ثانية حركة
#define MAX_HEATER_RUNTIME   30000   // 30 ثا\\\نية كحد أقصى لتشغيل السخان
#define NVS_SAVE_INTERVAL    60      // حفظ في NVS كل 60 ثانية (FIX: كان التعليق يقول خطأ "10 ثوانٍ")

// ---- درجات الحرارة ----
#define TEMP_TARGET          37.7    // درجة الحرارة المستهدفة
#define TEMP_LOW             (TEMP_TARGET - 0.3)   // 37.2°C
#define TEMP_HIGH            (TEMP_TARGET + 0.1)   // 37.6°C
#define TEMP_CRITICAL        39.5    // حد الخطر (يطفئ السخان)
#define TEMP_EMERGENCY       39.8    // حد الطوارئ (يدخل Emergency Mode)
#define TEMP_SAFE_EXIT       TEMP_HIGH           // درجة الخروج من Emergency Mode
#define RELAY_STUCK_MARGIN   2.0     // هامش كشف عطل الريلاي

// ---- الـ GPIO Pins ----
#define PIN_ONE_WIRE         4       // DS18B20
#define PIN_DHT              15      // DHT22
#define PIN_FAN              13      // المروحة
#define PIN_TURNER           14      // المقلب
#define PIN_HEATER           27      // السخان
#define PIN_SENSOR_FAILED    5       // LED عطل الحساسات
// #define TARGET_SEC (24UL * 60UL * 60UL)   // 86400 ثانية

// ---- أنواع الحساسات ----
#define DHT_TYPE             DHT22

// ---- عدد الحساسات (قابل للتوسع) ----
#define MAX_DS18B20_SENSORS  2       // حالياً 2 حساس (يمكن زيادتها لاحقاً)

// ---- Temperature Smoothing (EMA) ----
#define TEMP_SMOOTHING_ALPHA  0.25   // 0.1=بطيء (ثبات عالي) .. 0.5=سريع
float rawTemperature = 0;            // القيمة الخام - تُستخدم في فحوصات الطوارئ
bool  smootherInitialized = false;

#define EMERGENCY_EXIT_STABLE_TIME  10000       // لازم تفضل مستقرة 10 ثواني قبل الخروج
// ========================================================================
// 2. تعريف الهياكل (Structs) لإدارة البيانات
// ========================================================================
// ---- إعادة محاولة DHT التلقائية ----
#define DHT_RETRY_INTERVAL  60000   // 1  

#define TEMP_ABSOLUTE_MAX TEMP_EMERGENCY  // absolute cutoff = emergency threshold (single source)


// ============================================================
// Dynamic Configuration (NVS)
// ============================================================
struct SystemConfig {
    float targetTemp;           // 37.7
    float tempLow;              // target - 0.3
    float tempHigh;             // target + 0.1
    float tempCritical;         // 39.5
    float tempEmergency;        // 39.8
    unsigned long turnerOffTime; // 60000
    unsigned long turnerOnTime;  // 15000
    unsigned long heaterMaxRuntime; // 30000
    unsigned long nvsSaveInterval;  // 60 (ثواني)
};

SystemConfig config;
Preferences prefConfig;

// قيم افتراضية (تُستخدم عند أول تشغيل)
void setDefaultConfig() {
    config.targetTemp        = 37.7f;
    config.tempLow           = 37.4f;
    config.tempHigh          = 37.8f;
    config.tempCritical      = 39.5f;
    config.tempEmergency     = 39.8f;
    config.turnerOffTime     = 60000UL;
    config.turnerOnTime      = 15000UL;
    config.heaterMaxRuntime  = 30000UL;
    config.nvsSaveInterval   = 60UL;
}
// ============================================================
// ============================================================
// Finite State Machine – Safety-Critical Design
// ============================================================
typedef enum {
    STATE_BOOT = 0,
    STATE_NORMAL,
    STATE_SAFE,
    STATE_EMERGENCY
} SystemState;

SystemState currentState = STATE_BOOT;
unsigned long stateEnterMs = 0;
const char* currentStateName = "BOOT";

void printStateChange(uint8_t newState) {          // ← غيّرنا لـ uint8_t
    const char* names[] = {"BOOT", "NORMAL", "SAFE", "EMERGENCY"};
    if (newState > 3) newState = 0;
    currentStateName = names[newState];
    Serial.printf("🔄 STATE → %s (uptime %lu s)\n", currentStateName, millis() / 1000UL);
}

void changeState(uint8_t newState) {               // ← غيّرنا لـ uint8_t
    if (currentState == (SystemState)newState) return;
    currentState = (SystemState)newState;
    stateEnterMs = millis();
    printStateChange(newState);
}
extern unsigned long cycleCount; // مُعرَّف في الأسفل

// ---- إدارة حساسات الرطوبة (قابلة للتوسيع) ----
#define MAX_HUMIDITY_SENSORS 1   // حالياً واحد، يمكن زيادتها
#define DHT_SENSOR_COUNT MAX_HUMIDITY_SENSORS  // أو احذف DHT_SENSOR_COUNT تماماً
int dhtPins[MAX_HUMIDITY_SENSORS] = {PIN_DHT};
// FIX: تمت إزالة تعريف struct HumiditySensor المكرر هنا - أصبح
// يُستورد من diagnostics.h (المصدر الوحيد الآن، مُضمَّن عبر
// diag_io.h) بدلاً من نسخة محلية منفصلة.
HumiditySensor humiditySensors[MAX_HUMIDITY_SENSORS];

// ---- إدارة المرطب (Evaporator) ----
struct EvaporatorController {
    bool state;                     // حالة التشغيل الفعلية
    unsigned long lastToggleTime;   // آخر تغيير للحالة
    unsigned long onDuration;       // مدة التشغيل في الوضع الآمن (ms)
    unsigned long offDuration;      // مدة الإيقاف في الوضع الآمن (ms)
    bool useSafeTimedMode;          // هل نستخدم الدورة الزمنية الآمنة؟
};

EvaporatorController evaporator = {
    false, 0, 300000UL, 600000UL, false
};
////////////////////////////
bool waitingForConfirm = false;
unsigned long confirmTimeout = 0;

// ---- FIX: تمت إزالة تعريف struct Actuator المكرر هنا - أصبح
// يُستورد من diagnostics.h (المصدر الوحيد الآن).

// DSTemperatureSensor: defined once in diagnostics.h

// ========================================================================
// 3. تعريف المصفوفات والكائنات
// ========================================================================
#define PIN_EVAPORATOR 12

#define ACTUATOR_COUNT 4
// Index map (must match diagnostics.cpp diagSwitchCount / REPORT):
// 0=Fan, 1=Turner, 2=Heater, 3=Evaporator
Actuator actuators[ACTUATOR_COUNT] = {
    {PIN_FAN,     false, true,  "Fan",     false},
    {PIN_TURNER,  false, false, "Turner",  false},
    {PIN_HEATER,  false, false, "Heater",  true},
    {PIN_EVAPORATOR, false, false, "Evaporator", false}
};

// ---- حساسات DS18B20 ----
DSTemperatureSensor dsSensors[MAX_DS18B20_SENSORS];

// ---- حساسات DHT ----

// ---- كائنات المكتبات ----
LiquidCrystal_I2C lcd(0x27, 16, 2);
OneWire oneWire(PIN_ONE_WIRE);
DallasTemperature ds18b20(&oneWire);
Preferences pref;

// ========================================================================
// 4. المتغيرات العامة
// ========================================================================

// ---- إدارة شاشة LCD ----
bool lcdInitialized = false;

// ---- المقلب (Turner) ----
unsigned long lastTurnerTime = 0;
bool turnerState = false;

// ---- السخان (Heater) ----
bool heaterState = false;
unsigned long heaterOnStartTime = 0;
bool heaterRuntimeAlert = false;

// ---- العد التنازلي ----
unsigned long elapsedSec = 0;
unsigned long lastMillis = 0;
unsigned long cycleCount = 0;

// ---- قراءة الحساسات ----
unsigned long lastSensorTime = 0;
float currentTemperature = 0;      // درجة الحرارة المستخدمة للتحكم
bool hasValidTemperature = false;
bool avgValid = false;

// ---- السلامة (Safety) ----
bool safeMode = false;
bool emergencyMode = false;
unsigned long emergencyStartTime = 0;
unsigned long emergencyStableSince = 0;              // ← جديد
int sensorFailCounter = 0;
const int MAX_SENSOR_FAIL = 3;

// ---- كشف عطل الريلاي ----
unsigned long relayStuckTimer = 0;
bool relayStuckFault = false;

// ---- إحصائيات الرطوبة للتشخيص (يتم ملؤها في readSensors) ----
float avgHumidity = 0.0f;
uint32_t humidityFailCount = 0;
uint32_t humiditySamples = 0;
// ---- Per-cycle temperature average (reset each cycle) ----
float    cycle_temp_sum     = 0.0f;
uint32_t cycle_sample_count = 0;
// ---- أوامر Serial ----
char serialBuffer[64] = "";
uint8_t serialIdx = 0;


void loadConfig() {
    prefConfig.begin("inc_config", true); // read-only أولاً

    // إذا لم تكن المساحة موجودة → قيم افتراضية
    if (!prefConfig.isKey("targetTemp")) {
        prefConfig.end();
        setDefaultConfig();
        saveConfig(); // حفظ القيم الافتراضية مرة واحدة
        Serial.println("⚙️ Config: defaults loaded & saved");
        return;
    }

    config.targetTemp       = prefConfig.getFloat("targetTemp", 37.7f);
    config.tempLow          = prefConfig.getFloat("tempLow", 37.4f);
    config.tempHigh         = prefConfig.getFloat("tempHigh", 37.8f);
    config.tempCritical     = prefConfig.getFloat("tempCritical", 39.5f);
    config.tempEmergency    = prefConfig.getFloat("tempEmergency", 39.8f);
    config.turnerOffTime    = prefConfig.getULong("turnerOff", 60000UL);
    config.turnerOnTime     = prefConfig.getULong("turnerOn", 15000UL);
    config.heaterMaxRuntime = prefConfig.getULong("heaterMax", 30000UL);
    config.nvsSaveInterval  = prefConfig.getULong("nvsSave", 60UL);

    prefConfig.end();
    Serial.println("⚙️ Config loaded from NVS");
}

void saveConfig() {
    prefConfig.begin("inc_config", false);
    prefConfig.putFloat("targetTemp", config.targetTemp);
    prefConfig.putFloat("tempLow", config.tempLow);
    prefConfig.putFloat("tempHigh", config.tempHigh);
    prefConfig.putFloat("tempCritical", config.tempCritical);
    prefConfig.putFloat("tempEmergency", config.tempEmergency);
    prefConfig.putULong("turnerOff", config.turnerOffTime);
    prefConfig.putULong("turnerOn", config.turnerOnTime);
    prefConfig.putULong("heaterMax", config.heaterMaxRuntime);
    prefConfig.putULong("nvsSave", config.nvsSaveInterval);
    prefConfig.end();
    Serial.println("💾 Config saved to NVS");
}
// ========================================================================
// 5. دوال مساعدة (Helper Functions)
// ========================================================================

// ---- تشغيل/إيقاف مشغل ----
void setActuator(int index, bool state) {
    // C8: مصدر واحد للحقيقة — مزامنة heaterState/turnerState مع actuators[]
    if (index < 0 || index >= ACTUATOR_COUNT) return;
    if (actuators[index].state == state) return;
    actuators[index].state = state;
    digitalWrite(actuators[index].pin, state ? HIGH : LOW);
    if (index == 2) heaterState = state;
    if (index == 1) turnerState = state;
    Serial.printf("⚡ %s -> %s\n", actuators[index].name, state ? "ON" : "OFF");
    diag_actuator_event(index);
}
// ---- إطفاء جميع السخانات ----
void forceAllHeatersOff() {
    for (int i = 0; i < ACTUATOR_COUNT; i++) {
        if (actuators[i].isHeater) {
            setActuator(i, false);
        }
    }
    heaterState = false;
}

// ---- تفعيل وضع الأمان (Safe Mode) ----
void enterSafeMode(const char* reason) {
    if (!safeMode) {
        safeMode = true;
        forceAllHeatersOff();
        setActuator(0, true);  // تشغيل المروحة
        setActuator(1, false); // إيقاف المقلب
        
        Serial.println("⚠️⚠️⚠️ SAFE MODE ACTIVATED ⚠️⚠️⚠️");
        Serial.print("Reason: ");
        Serial.println(reason);
        pref.putBool("safe_mode", true);
        {
            char logBuf[96];
            snprintf(logBuf, sizeof(logBuf), "Safe Mode: %s", reason);
            logEvent(logBuf);
        }
        addFault(FAULT_SAFE_MODE_ENTER, FAULT_EMERGENCY, 0);

    }
}

// ---- الخروج من وضع الأمان ----
void exitSafeMode() {
    
    if (safeMode) {
        safeMode = false;
        pref.putBool("safe_mode", false);
        Serial.println("✅ Safe mode cleared");
        logEvent("Safe Mode cleared");
    }
}

// ---- تفعيل وضع الطوارئ (Emergency Mode) ----
void enterEmergencyMode() {
    if (!emergencyMode) {
        emergencyMode = true;
        emergencyStartTime = millis();
        emergencyStableSince = 0;  // restart stability window on every entry
        forceAllHeatersOff();
        setActuator(0, true);  // تشغيل المروحة
        setActuator(1, false); // إيقاف المقلب
        
        Serial.println("🚨🚨🚨 EMERGENCY MODE ACTIVATED! 🚨🚨🚨");
        Serial.print("Temperature: ");
        Serial.print(rawTemperature);
        Serial.println("°C >= 39.8°C");
        
        pref.putBool("emergency", true);
        pref.putULong("emergency_time", millis());
        {
            char logBuf[64];
            snprintf(logBuf, sizeof(logBuf), "Emergency Mode: Temp %.1f°C", rawTemperature);
            logEvent(logBuf);
        }
        addFault(FAULT_EMERGENCY_TEMP, FAULT_EMERGENCY, rawTemperature);
    }
}

// ---- الخروج من وضع الطوارئ ----
void exitEmergencyMode() {
    if (emergencyMode) {
        emergencyMode = false;
        pref.putBool("emergency", false);
        Serial.println("✅ Emergency mode cleared");
        logEvent("Emergency Mode cleared");
    }
}

// ---- تسجيل الأحداث ----
void logEvent(const char* message) {
    unsigned long now = millis();
    unsigned long totalSec = now / 1000;
    unsigned long days = totalSec / 86400;
    unsigned long hours = (totalSec % 86400) / 3600;
    unsigned long mins = (totalSec % 3600) / 60;
    unsigned long secs = totalSec % 60;
    
    Serial.print("[");
    Serial.print(days);
    Serial.print("d ");
    Serial.print(hours);
    Serial.print("h ");
    Serial.print(mins);
    Serial.print("m ");
    Serial.print(secs);
    Serial.print("s] ");
    Serial.println(message);
}

// ---- فحص معقولية درجة الحرارة ----
bool isPlausibleTemperature(float temp) {
    return (temp > 0.0 && temp < 50.0);
}

// ---- فحص القفزات المفاجئة ----
bool isTemperatureJumpValid(float temp, float lastTemp) {
    const float MAX_JUMP = 5.0;
    if (lastTemp == 0) return true;
    return abs(temp - lastTemp) <= MAX_JUMP;
}
// ---- تنعيم قراءة الحرارة (EMA) ----
float applyTempSmoothing(float newRawTemp) {
    if (!smootherInitialized) {
        currentTemperature = newRawTemp;
        smootherInitialized = true;
    } else {
        currentTemperature = (TEMP_SMOOTHING_ALPHA * newRawTemp)
                            + ((1.0 - TEMP_SMOOTHING_ALPHA) * currentTemperature);
    }
    return currentTemperature;
}
// ========================================================================
// 6. دالة كشف عطل الريلاي
// ========================================================================

void checkRelayStuckFault() {
     // لا داعي لفحص عطل الريلاي أثناء Emergency/Safe Mode - السخان مقفول عمداً بقرار النظام
    if (emergencyMode || safeMode) {
        relayStuckTimer = 0;
        return;
    }
    
    // إذا كان السخان مطفأ برمجياً لكن الحرارة مرتفعة جداً
if (!heaterState && (rawTemperature >= (config.tempHigh + RELAY_STUCK_MARGIN))) {        if (relayStuckTimer == 0) {
            relayStuckTimer = millis();
        } else {
            // Already returned if emergency/safe; fixed 30s window for stuck-on
            unsigned long requiredDuration = 30000UL;
            if (millis() - relayStuckTimer >= requiredDuration) {
                relayStuckFault = true;
                enterSafeMode("RELAY STUCK-ON DETECTED! HARDWARE FAILURE!");
            }
        }
    } else {
        relayStuckTimer = 0;
    }
}

// ========================================================================
// 7. دالة تهيئة الحساسات
// ========================================================================

void initSensors() {
    // ---- تهيئة DS18B20 ----
    ds18b20.begin();
    int deviceCount = ds18b20.getDeviceCount();
    Serial.print("Found ");
    Serial.print(deviceCount);
    Serial.println(" DS18B20 sensors.");
    
    for (int i = 0; i < MAX_DS18B20_SENSORS; i++) {
        dsSensors[i].temperature = 0;
        dsSensors[i].valid = false;
        dsSensors[i].lastValidTemp = 0;
        dsSensors[i].failCount = 0;
    }
    // ---- تهيئة حساسات الرطوبة ----
    for (int i = 0; i < MAX_HUMIDITY_SENSORS; i++) {
        humiditySensors[i].dht = new DHT(dhtPins[i], DHT_TYPE);
        humiditySensors[i].dht->begin();
        humiditySensors[i].humidity = 0;
        humiditySensors[i].temperature = 0;
        humiditySensors[i].valid = false;
        humiditySensors[i].failCount = 0;
        humiditySensors[i].bypassed = false;
        humiditySensors[i].bypassStartTime = 0;
    }
    // // ---- تهيئة DHT ----
    // for (int i = 0; i < DHT_SENSOR_COUNT; i++) {
    //     dhtSensors[i].dht = new DHT(dhtPins[i], DHT_TYPE);
    //     dhtSensors[i].dht->begin();
    //     dhtSensors[i].temperature = 0;
    //     dhtSensors[i].humidity = 0;
    //     dhtSensors[i].valid = false;
    // }
}

// ---- قراءة DHT مع مهلة زمنية (Timeout) ----
// ---- إعادة تهيئة DHT عند الفشل ----
void resetHumiditySensor(int index) {
    if (index >= 0 && index < MAX_HUMIDITY_SENSORS) {
        Serial.printf("Resetting Humidity[%d]...\n", index);
        // Re-begin only – avoid delete/new heap fragmentation
        if (humiditySensors[index].dht) {
            humiditySensors[index].dht->begin();
        }
        humiditySensors[index].valid = false;
        humiditySensors[index].failCount = 0;
        humiditySensors[index].bypassed = false;
        humiditySensors[index].bypassStartTime = 0;
        {
            unsigned long t0 = millis();
            while ((millis() - t0) < 50UL) { esp_task_wdt_reset(); delay(1); }
        }
        { char logBuf[48]; snprintf(logBuf, sizeof(logBuf), "Humidity[%d] reset", index); logEvent(logBuf); }
    }
}
void controlEvaporator() {
    // ---- 1. السلامة أولاً ----
    if (emergencyMode || safeMode) {
        if (evaporator.state) {
            evaporator.state = false;
            setActuator(3, false);
        }
        return;
    }

    // ---- 2. تحقق من صحة حساس الرطوبة ----
    bool hasValidHumidity = false;
    float currentHumidity = 0;

    for (int i = 0; i < MAX_HUMIDITY_SENSORS; i++) {
        // الحساس يعتبر صالحاً فقط إذا كان غير متجاوز (bypassed) و valid
        if (humiditySensors[i].valid && !humiditySensors[i].bypassed) {
            hasValidHumidity = true;
            currentHumidity = humiditySensors[i].humidity;
            break;
        }
    }

    // ---- 3. تحكم طبيعي (باستخدام الرطوبة) ----
    if (hasValidHumidity) {
        evaporator.useSafeTimedMode = false;
        // شغّل عند 45%، أوقف عند 55%
        if (currentHumidity < 45.0 && !evaporator.state) {
            evaporator.state = true;
            setActuator(3, true);
            Serial.println("Evaporator ON (Normal)");
        } else if (currentHumidity > 55.0 && evaporator.state) {
            evaporator.state = false;
            setActuator(3, false);
            Serial.println("Evaporator OFF (Normal)");
        }
        return;
    }

    // ---- 4. وضع آمن (دورة زمنية) عند فقدان الرطوبة ----
    // هنا نستخدم المنطق: إذا لم توجد قراءة صالحة (بسبب 3 فشلات متتالية)،
    // نعتمد على "آخر حالة معروفة" أو دورة زمنية آمنة بدلاً من التوقف المفاجئ.
    if (!hasValidHumidity) {
        evaporator.useSafeTimedMode = true;
        unsigned long now = millis();

        if (!evaporator.state) {
            // إذا كان المرطب مطفأ ومر وقت كافٍ → شغله
            if (now - evaporator.lastToggleTime >= evaporator.offDuration) {
                evaporator.state = true;
                setActuator(3, true);
                evaporator.lastToggleTime = now;
                Serial.println("Evaporator ON (Safe timed)");
            }
        } else {
            // إذا كان المرطب شغال ومر وقت كافٍ → أوقفه
            if (now - evaporator.lastToggleTime >= evaporator.onDuration) {
                evaporator.state = false;
                setActuator(3, false);
                evaporator.lastToggleTime = now;
                Serial.println("Evaporator OFF (Safe timed)");
            }
        }
    }
}// ========================================================================
// ========================================================================

void readSensors() {
    // =====================================================================
    // 1. قراءة حساسات DS18B20 (مع مهلة 750 مللي)
    // =====================================================================
    ds18b20.setWaitForConversion(false);
    ds18b20.requestTemperatures();

    unsigned long startDS = millis();
    const unsigned long DS_TIMEOUT = 750;  // أقصى وقت لتحويل 12-bit

    // انتظار اكتمال التحويل مع مهلة
    while (!ds18b20.isConversionComplete()) {
        if (millis() - startDS >= DS_TIMEOUT) {
            esp_task_wdt_reset();  // ← أضف هذا السطر
            Serial.println("⚠️ DS18B20 conversion timeout!");
            break;
        }
        delay(1);
        esp_task_wdt_reset();
    }

    float tempSum = 0;
    int validCount = 0;

    // قراءة جميع حساسات DS18B20
    for (int i = 0; i < MAX_DS18B20_SENSORS; i++) {
        float temp = ds18b20.getTempCByIndex(i);
        dsSensors[i].temperature = temp;

        bool isConnected = (temp != DEVICE_DISCONNECTED_C);
        bool isPlausible = isPlausibleTemperature(temp);
        bool jumpValid = isTemperatureJumpValid(temp, dsSensors[i].lastValidTemp);
        bool isTimeout = (millis() - startDS >= DS_TIMEOUT);

        if (isConnected && isPlausible && jumpValid && !isTimeout) {
            dsSensors[i].valid = true;
            dsSensors[i].lastValidTemp = temp;
            dsSensors[i].failCount = 0;
            tempSum += temp;
            validCount++;
        } else {
            dsSensors[i].valid = false;
            dsSensors[i].failCount++;
            if (!isConnected) {
                Serial.printf("⚠️ DS18B20[%d] disconnected\n", i);
            } else if (!isPlausible) {
                Serial.printf("⚠️ DS18B20[%d] implausible: %.1f°C\n", i, temp);
            } else if (!jumpValid) {
                Serial.printf("⚠️ DS18B20[%d] jump: %.1f -> %.1f\n", i, dsSensors[i].lastValidTemp, temp);
            } else if (isTimeout) {
                Serial.printf("⚠️ DS18B20[%d] conversion timeout\n", i);
            }
        }
    }

    // حساب متوسط الحرارة (المستخدم في التحكم)
    if (validCount > 0) {
        rawTemperature = tempSum / validCount;
        applyTempSmoothing(rawTemperature);
        hasValidTemperature = true;
        avgValid = true;
    } else {
        rawTemperature = 0;
        hasValidTemperature = false;
        avgValid = false;
        // لا نمسح smootherInitialized للحفاظ على القيمة الناعمة
    }

    // =====================================================================
    // 2. قراءة حساسات DHT (طريقة آمنة - مرة واحدة فقط)
    // =====================================================================
        // =====================================================================
    // 2. قراءة حساسات DHT (مع إعادة محاولة تلقائية دورية)
    // =====================================================================
// 2. قراءة حساسات DHT (مع منطق Debounce - مثل DS18B20 تماماً)
    // =====================================================================
    // =====================================================================
    // 2. قراءة حساسات DHT (مع منطق Debounce - مثل DS18B20 تماماً)
    // =====================================================================
    for (int i = 0; i < MAX_HUMIDITY_SENSORS; i++) {
        // ---- التحقق من التجاوز وإعادة المحاولة الدورية ----
        if (humiditySensors[i].bypassed) {
            if (millis() - humiditySensors[i].bypassStartTime >= DHT_RETRY_INTERVAL) {
                humiditySensors[i].bypassed = false;
                humiditySensors[i].failCount = 0; // إعادة تعيين العداد
                Serial.printf("🔄 Humidity[%d] retry attempt\n", i);
            } else {
                humiditySensors[i].valid = false;
                humiditySensors[i].humidity = 0;
                humiditySensors[i].temperature = 0;
                continue;
            }
        }

        // ---- قراءة الحساس بطريقة آمنة (تمنع تعليق النظام) ----
        unsigned long t0 = millis();
        
        esp_task_wdt_reset(); // إطعام المؤقت قبل القراءة
        float h = humiditySensors[i].dht->readHumidity();
        float t = humiditySensors[i].dht->readTemperature();
        esp_task_wdt_reset(); // إطعام المؤقت بعد القراءة

        // **حماية قاتلة:** إذا استغرقت المكتبة أكثر من 50ms، فهي معلقة
        if (millis() - t0 > 100UL) {
            h = NAN;
            t = NAN;
        }

        // ---- نجاح القراءة ----
        if (!isnan(h) && !isnan(t) && h != 0.0 && t != 0.0) {
            humiditySensors[i].humidity = h;
            humiditySensors[i].temperature = t;
            humiditySensors[i].valid = true;
            
            // منطق الـ Debounce: زيادة عداد النجاح
            if (humiditySensors[i].failCount > 0) {
                humiditySensors[i].failCount--; // نقص عداد الفشل تدريجياً
            }
            
            if (humiditySensors[i].failCount == 0) {
                // إذا عاد العداد للصفر، الحساس سليم تماماً
                if (humiditySensors[i].bypassed) {
                    humiditySensors[i].bypassed = false;
                    Serial.printf("✅ Humidity[%d] reconnected!\n", i);
                    logEvent("Humidity reconnected");
                }
            }
        } 
        // ---- فشل القراءة ----
        else {
            humiditySensors[i].valid = false;
            humiditySensors[i].humidity = 0;
            humiditySensors[i].temperature = 0;
            humiditySensors[i].failCount++;

            // منطق الـ Debounce: إذا فشل 3 مرات متتالية -> يُعتبر مفقوداً مؤقتاً
            if (humiditySensors[i].failCount >= 3) {
                humiditySensors[i].bypassed = true;
                humiditySensors[i].bypassStartTime = millis();
                Serial.printf("⛔ Humidity[%d] bypassed for %d s\n", i, DHT_RETRY_INTERVAL / 1000);
                logEvent("Humidity bypassed");
            }
        }
    }
    // ---- إحصائيات الرطوبة للتشخيص (توسع مستقبلي) ----
    float humSum = 0; int humCount = 0;
    for (int i = 0; i < MAX_HUMIDITY_SENSORS; i++) {
        if (humiditySensors[i].valid && !humiditySensors[i].bypassed) {
            humSum += humiditySensors[i].humidity;
            humCount++;
        }
        if (!humiditySensors[i].valid) humidityFailCount++;
    }
    if (humCount > 0) avgHumidity = humSum / humCount;
    humiditySamples += humCount;
}
// ========================================================================
// 9. دالة تحديث شاشة LCD
// ========================================================================
void reinitLCD() {
    // تأكد من أن الـ I2C يستجيب قبل التهيئة
    Wire.beginTransmission(0x27);
    if (Wire.endTransmission() != 0) {
        Serial.println("❌ LCD still not responding, aborting reinit.");
        lcdInitialized = false;
        return;
    }
    esp_task_wdt_reset();
    lcd.init();
    esp_task_wdt_reset();

    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Reinit OK");
    // C7: لا delay خام — غذّ WDT أثناء الانتظار
    {
        unsigned long t0 = millis();
        while (millis() - t0 < 200UL) {
            esp_task_wdt_reset();
            delay(10);
            esp_task_wdt_reset();  // بعد كل delay طويل

        }
    }
    lcdInitialized = true;
    Serial.println("✅ LCD reinitialized successfully.");
}

void updateLCD() {
    bool dhtOk = false;
    float humidity = 0;
    float dhtTemp = 0;
    if (!lcdInitialized) return;

    for (int i = 0; i < MAX_HUMIDITY_SENSORS; i++) {
        if (humiditySensors[i].valid && !humiditySensors[i].bypassed) {
            dhtOk = true;
            humidity = humiditySensors[i].humidity;
            dhtTemp = humiditySensors[i].temperature;
            break;
        }
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    
    // ---- السطر الأول: درجة الحرارة والرطوبة ----
    if (avgValid) {
        lcd.print("Avg:");
        lcd.print(currentTemperature, 1);
        lcd.print("C ");
        if (dhtOk) {
            lcd.print("H:");
            lcd.print(humidity, 0);
            lcd.print("%");
        } else {
            lcd.print("H:--");
        }
    } else if (dsSensors[0].valid) {
        lcd.print("T1:");
        lcd.print(dsSensors[0].temperature, 1);
        lcd.print("C ");
        if (dhtOk) {
            lcd.print("H:");
            lcd.print(humidity, 0);
            lcd.print("%");
        } else {
            lcd.print("H:--");
        }
    } else if (dsSensors[1].valid) {
        lcd.print("T2:");
        lcd.print(dsSensors[1].temperature, 1);
        lcd.print("C ");
        if (dhtOk) {
            lcd.print("H:");
            lcd.print(humidity, 0);
            lcd.print("%");
        } else {
            lcd.print("H:--");
        }
    } else {
        lcd.print("Sensor Error!");
    }
    
    // ---- السطر الثاني: الوقت المتبقي + حالة الطوارئ ----
    unsigned long remainingSec = 0;
    if (TARGET_SEC > elapsedSec) {
        remainingSec = TARGET_SEC - elapsedSec;
    }
    
    unsigned long remMin = remainingSec / 60;
    unsigned long remSec = remainingSec % 60;
    
    lcd.setCursor(0, 1);
    lcd.print("R:");
    if (remMin < 10) lcd.print("0");
    lcd.print(remMin);
    lcd.print(":");
    if (remSec < 10) lcd.print("0");
    lcd.print(remSec);
    
    // عرض حالة الطوارئ أو الأمان
    lcd.setCursor(11, 1);
    if (emergencyMode) {
        lcd.print("EMG");
    } else if (safeMode) {
        lcd.print("SAF");
    } else {
        lcd.print("   ");
    }
}

// ========================================================================
// 10. دالة التحكم في السخان
// ========================================================================

void controlHeater() {
    if (safeMode || emergencyMode) {
        forceAllHeatersOff();
        return;
    }

    if (!hasValidTemperature) {
        forceAllHeatersOff();
        return;
    }

    // استخدام الإعدادات الديناميكية
    if (rawTemperature >= config.tempEmergency) {
        enterEmergencyMode();
        return;
    }

    if (rawTemperature >= config.tempCritical) {
        forceAllHeatersOff();
        return;
    }

    // Hysteresis باستخدام config
    if (!heaterState && currentTemperature <= config.tempLow) {
        setActuator(2, true);
        heaterOnStartTime = millis();
        heaterRuntimeAlert = false;
        logEvent("Heater ON");
    }
    else if (heaterState && currentTemperature >= config.tempHigh) {
        setActuator(2, false);
        heaterRuntimeAlert = false;
        logEvent("Heater OFF");
    }
}
// ========================================================================
// 11. دالة مراقبة وقت تشغيل السخان
// ========================================================================

void checkHeaterRuntime() {
    if (!heaterState) {
        heaterRuntimeAlert = false;
        return;
    }

    if (millis() - heaterOnStartTime >= config.heaterMaxRuntime) {
        if (!heaterRuntimeAlert) {
            heaterRuntimeAlert = true;
            Serial.println("⚠️ Warning: Heater running too long! Check door/insulation.");
            logEvent("Heater runtime warning");
            // يمكن إرسال تنبيه تيليجرام هنا لاحقاً
        }
    }
}
// ========================================================================
// 12. دالة تحديث العداد التنازلي
// ========================================================================

void updateTimer() {
    unsigned long now = millis();

    unsigned long delta = (now - lastMillis) / 1000;

    if (delta > 0 && elapsedSec < TARGET_SEC) {
        elapsedSec += delta;
        lastMillis = now;
        
        // FIX: استخدام الإعداد الديناميكي بدلاً من #define
        if (elapsedSec % config.nvsSaveInterval == 0) {
            pref.putULong("elapsed_sec", elapsedSec);
            Serial.println("💾 Saved to NVS");
        }
        
        // عند الوصول للهدف
        if (elapsedSec >= TARGET_SEC) {
            elapsedSec = 0;
            lastMillis = millis();
            cycleCount++;
            pref.putULong("elapsed_sec", 0);
            pref.putULong("cycle_count", cycleCount);
            Serial.printf("✅ Cycle #%lu completed!\n", cycleCount);
            { char logBuf[48]; snprintf(logBuf, sizeof(logBuf), "Cycle #%lu completed", cycleCount); logEvent(logBuf); }

            {
                float avgTemp = (cycle_sample_count > 0)
                    ? (cycle_temp_sum / (float)cycle_sample_count)
                    : currentTemperature;
                cycle_archive_onCycleComplete(cycleCount, getHealthScore(),
                                              avgTemp, getCycleDutyCycle(), getCycleAvgOvershoot());
            }
            cycle_temp_sum = 0.0f;
            cycle_sample_count = 0;
            diag_cycleReset();
        }
    }
}
// ========================================================================
// 13. دالة معالجة أوامر Serial
// ========================================================================
void handleSerialCommands() {
    if (!Serial.available()) return;

    char c = Serial.read();

    if (c == '\n' || c == '\r') {
        // ---- أمر RESET ----
        if (strcmp(serialBuffer, "RESET") == 0 || strcmp(serialBuffer, "reset") == 0) {
            elapsedSec = 0;
            lastMillis = millis();
            pref.putULong("elapsed_sec", 0);

            if (safeMode) {
                exitSafeMode();
                Serial.println("✅ Safe Mode cleared by RESET");
            }
            if (emergencyMode) {
                exitEmergencyMode();
                Serial.println("✅ Emergency Mode cleared by RESET");
            }

            Serial.println("🔄 Reset Done!");
            logEvent("Manual reset via Serial");
        }
        // ---- أمر STATUS ----
        else if (strcmp(serialBuffer, "STATUS") == 0 || strcmp(serialBuffer, "status") == 0) {
            Serial.println("===== System Status =====");
            Serial.printf("Cycle: %lu / %lu seconds\n", elapsedSec, TARGET_SEC);
            Serial.printf("Temperature (raw): %.1f°C\n", rawTemperature);
            Serial.printf("Temperature (smoothed): %.1f°C\n", currentTemperature);
            Serial.printf("Heater: %s\n", heaterState ? "ON" : "OFF");
            Serial.printf("Turner: %s\n", turnerState ? "ON" : "OFF");
            Serial.printf("Safe Mode: %s\n", safeMode ? "ACTIVE" : "OFF");
            Serial.printf("Emergency: %s\n", emergencyMode ? "ACTIVE" : "OFF");
            Serial.printf("Cycle Count: %lu\n", cycleCount);
            Serial.printf("Sensor Valid: %s\n", hasValidTemperature ? "YES" : "NO");
            Serial.println("=========================");
        }
        // ---- أمر EXITSAFE ----
        else if (strcmp(serialBuffer, "EXITSAFE") == 0 || strcmp(serialBuffer, "exitsafe") == 0) {
            if (safeMode) {
                exitSafeMode();
                setActuator(0, true);
                Serial.println("✅ Safe Mode cleared manually");
            } else {
                Serial.println("ℹ️ Safe Mode is not active");
            }
        }
        // ---- أمر EXITEMG ----
        else if (strcmp(serialBuffer, "EXITEMG") == 0 || strcmp(serialBuffer, "exitemg") == 0) {
            if (emergencyMode) {
                exitEmergencyMode();
                Serial.println("✅ Emergency Mode cleared manually");
            } else {
                Serial.println("ℹ️ Emergency Mode is not active");
            }
        }
        // ---- أمر RESETDHT ----
        else if (strcmp(serialBuffer, "RESETDHT") == 0 || strcmp(serialBuffer, "resetdht") == 0) {
            for (int i = 0; i < MAX_HUMIDITY_SENSORS; i++) {
                resetHumiditySensor(i);
            }
            Serial.println("🔄 All humidity sensors have been reset!");
        }
        // ---- أمر REPORT ----
        else if (strcmp(serialBuffer, "REPORT") == 0 || strcmp(serialBuffer, "report") == 0) {
            printFullReport();
        }
        // ---- أمر HEALTH ----
        else if (strcmp(serialBuffer, "HEALTH") == 0 || strcmp(serialBuffer, "health") == 0) {
            printHealthSummary();
        }
        // ---- أمر DIAG ----
        else if (strcmp(serialBuffer, "DIAG") == 0 || strcmp(serialBuffer, "diag") == 0) {
            printDiagStatus();
        }
        else if (strcmp(serialBuffer, "FAULTS") == 0 || strcmp(serialBuffer, "faults") == 0) {
            printFaultRing();
        }
        // ---- أمر اختبار التيليجرام ----
                // ---- أوامر مراقبة التيليجرام التلقائية ----
        // ---- أوامر مراقبة التيليجرام التلقائية ----
        else if (strcmp(serialBuffer, "MONITORON") == 0) {
            telegram_set_monitoring(true);
            Serial.println("✅ Periodic monitoring ENABLED (every 5 min)"); // C5
        }
        else if (strcmp(serialBuffer, "MONITOROFF") == 0) {
            telegram_set_monitoring(false);
            Serial.println("⏹️ Periodic monitoring DISABLED");
        }else if (strcmp(serialBuffer, "TESTTG") == 0) {
            Serial.println("📲 Sending test Telegram...");
            // استخدام رقم عشوائي ككود عطل مختلف في كل مرة
            telegram_send_fault(random(1, 255), 1, 0.0f); 
        }
        // ---- أمر CLEARDIAG (بدء عملية المسح) ----
        else if (strcmp(serialBuffer, "CLEARDIAG") == 0 || strcmp(serialBuffer, "cleardiag") == 0) {
            if (waitingForConfirm) {
                Serial.println("⚠️ Confirmation already pending. Type 'CONFIRM' or wait.");
            } else {
                Serial.println("⚠️ WARNING: This will erase ALL diagnostics data (history, faults, stats).");
                Serial.println("⚠️ Type 'CONFIRM' within 5 seconds to proceed.");
                waitingForConfirm = true;
                confirmTimeout = millis() + 5000;
            }
        }
        // ---- أمر FORECAST (Phase 7) ----
        else if (strcmp(serialBuffer, "FORECAST") == 0 || strcmp(serialBuffer, "forecast") == 0) {
            cycle_archive_printForecast();
        }
        // ---- أمر HEAP (free SRAM diagnostic) ----
        else if (strcmp(serialBuffer, "HEAP") == 0 || strcmp(serialBuffer, "heap") == 0) {
            Serial.printf("🧠 Heap free: %u bytes | min ever: %u bytes | firmware %s\n",
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)ESP.getMinFreeHeap(),
                          FIRMWARE_VERSION);
        }
        // ---- أمر VERSION ----
        else if (strcmp(serialBuffer, "VERSION") == 0 || strcmp(serialBuffer, "version") == 0) {
            Serial.printf("%s v%s\n", FIRMWARE_NAME, FIRMWARE_VERSION);
            Serial.printf("  Chip: %s  Rev: %d  CPU: %u MHz\n",
                          ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
        }        // ---- SET_TEMP ----
        else if (strncmp(serialBuffer, "SET_TEMP ", 9) == 0) {
            float val = atof(serialBuffer + 9);
            if (val >= 30.0f && val <= 42.0f) {
                config.targetTemp = val;
                config.tempLow  = val - 0.3f;
                config.tempHigh = val + 0.1f;
                saveConfig();
                Serial.printf("✅ Target Temp set to %.1f°C (Low=%.1f High=%.1f)\n",
                              config.targetTemp, config.tempLow, config.tempHigh);
            } else {
                Serial.println("❌ SET_TEMP range: 30.0 – 42.0");
            }
        }
        // ---- SET_CRITICAL ----
        else if (strncmp(serialBuffer, "SET_CRITICAL ", 13) == 0) {
            float val = atof(serialBuffer + 13);
            if (val >= 38.0f && val <= 45.0f) {
                config.tempCritical = val;
                saveConfig();
                Serial.printf("✅ Critical Temp set to %.1f°C\n", config.tempCritical);
            } else {
                Serial.println("❌ SET_CRITICAL range: 38.0 – 45.0");
            }
        }
        // ---- SET_EMERGENCY ----
        else if (strncmp(serialBuffer, "SET_EMERGENCY ", 14) == 0) {
            float val = atof(serialBuffer + 14);
            if (val >= 38.0f && val <= 45.0f && val > config.tempCritical) {
                config.tempEmergency = val;
                saveConfig();
                Serial.printf("✅ Emergency Temp set to %.1f°C\n", config.tempEmergency);
            } else {
                Serial.println("❌ SET_EMERGENCY must be > Critical and within 38–45");
            }
        }
        // ---- SET_TURNER_ON ----
        else if (strncmp(serialBuffer, "SET_TURNER_ON ", 14) == 0) {
            int sec = atoi(serialBuffer + 14);
            if (sec >= 5 && sec <= 60) {
                config.turnerOnTime = (unsigned long)sec * 1000UL;
                saveConfig();
                Serial.printf("✅ Turner ON time set to %d s\n", sec);
            } else {
                Serial.println("❌ SET_TURNER_ON range: 5 – 60 seconds");
            }
        }
        // ---- SET_TURNER_OFF ----
        else if (strncmp(serialBuffer, "SET_TURNER_OFF ", 15) == 0) {
            int sec = atoi(serialBuffer + 15);
            if (sec >= 30 && sec <= 600) {
                config.turnerOffTime = (unsigned long)sec * 1000UL;
                saveConfig();
                Serial.printf("✅ Turner OFF time set to %d s\n", sec);
            } else {
                Serial.println("❌ SET_TURNER_OFF range: 30 – 600 seconds");
            }
        }
        // ---- SHOW_CONFIG ----
        else if (strcmp(serialBuffer, "SHOW_CONFIG") == 0 || strcmp(serialBuffer, "show_config") == 0) {
            Serial.println("========== CURRENT CONFIG ==========");
            Serial.printf("  Target Temp     : %.2f °C\n", config.targetTemp);
            Serial.printf("  Temp Low        : %.2f °C\n", config.tempLow);
            Serial.printf("  Temp High       : %.2f °C\n", config.tempHigh);
            Serial.printf("  Temp Critical   : %.2f °C\n", config.tempCritical);
            Serial.printf("  Temp Emergency  : %.2f °C\n", config.tempEmergency);
            Serial.printf("  Turner ON       : %lu s\n", config.turnerOnTime / 1000UL);
            Serial.printf("  Turner OFF      : %lu s\n", config.turnerOffTime / 1000UL);
            Serial.printf("  Heater Max Run  : %lu s\n", config.heaterMaxRuntime / 1000UL);
            Serial.printf("  NVS Save Interv : %lu s\n", config.nvsSaveInterval);
            Serial.println("====================================");
        }
        // ---- أمر HELP ----
        else if (strcmp(serialBuffer, "HELP") == 0 || strcmp(serialBuffer, "help") == 0 ||
                 strcmp(serialBuffer, "?") == 0) {
            Serial.println("Available commands:");
            Serial.println("  STATUS    - Live system status");
            Serial.println("  REPORT    - Full reliability report");
            Serial.println("  HEALTH    - Short health line");
            Serial.println("  DIAG      - Internal diagnostics counters");
            Serial.println("  FAULTS    - Fault ring (last events)");
            Serial.println("  FORECAST  - Health degradation projection");
            Serial.println("  HEAP      - Free / min free heap");
            Serial.println("  VERSION   - Firmware and chip info");
            Serial.println("  RESET     - Reset timer; clear Safe + Emergency");
            Serial.println("  EXITSAFE  - Exit Safe Mode only");
            Serial.println("  EXITEMG   - Exit Emergency Mode only");
            Serial.println("  RESETDHT  - Re-begin humidity sensor(s)");
            Serial.println("  CLEARDIAG - Wipe diagnostics (then CONFIRM within 5s)");
            Serial.println("  HELP      - This list");
            Serial.println("  MONITORON - Send Telegram report every 5 min");
            Serial.println("  MONITOROFF - Stop periodic Telegram reports");
        }
        else if (strcmp(serialBuffer, "PUBLISH") == 0) {
            publishAllFeeds();
        }        // ---- أوامر غير معروفة ----
        else {
            Serial.println("❌ Unknown command. Type HELP for the full list.");
        }

        // مسح المخزن المؤقت بعد معالجة الأمر
        serialBuffer[0] = '\0';
        serialIdx = 0;
    } else {
        // تجميع الحروف مع حد أقصى
        if (serialIdx < sizeof(serialBuffer) - 1) {
            serialBuffer[serialIdx++] = c;
            serialBuffer[serialIdx] = '\0';
        }
    }
}

// ============================================================
// دالة اختبار الـ Core Dump (تُسبب انهياراً متعمداً)
// ============================================================
// void testCoreDump() {
//     Serial.println("🔥 Crashing immediately with null pointer dereference...");
//     // تأخير قصير جداً (مثل 100 مللي) أو بدون تأخير
//     delay(100); 
    
//     // طريقة مضمونة للانهيار الفوري (بدون تعطيل الـ Watchdog)
//     volatile int* p = nullptr; 
//     volatile int crash = *p; // محاولة قراءة من عنوان 0x00000000
    
//     Serial.println(crash); // لن تصل هنا أبداً
// }
// تعريف bootCount كمتغير عام (في أعلى Memory10_2.ino قبل setup)
uint8_t bootCount = 0;

// ============================================================
// Task WDT — safe for Arduino-ESP32 core 3.x
// Call BEFORE any delay / NTP / long work in setup()
// ============================================================
static void setupWatchdog() {
    esp_task_wdt_config_t cfg = {
        .timeout_ms = (uint32_t)WDT_TIMEOUT * 1000UL,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };

    // Core 3.x often already initialized TWDT → reconfigure instead of double init
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        // Not initialized yet
        err = esp_task_wdt_init(&cfg);
    } else if (err != ESP_OK) {
        esp_task_wdt_deinit();
        err = esp_task_wdt_init(&cfg);
    }

    esp_err_t addErr = esp_task_wdt_add(NULL);  // subscribe loopTask
    Serial.printf("WDT: config=%s add=%s timeout=%ds\n",
                  esp_err_to_name(err), esp_err_to_name(addErr), WDT_TIMEOUT);
}

void setup() {
    Serial.begin(115200);
    // Serial.println("Working");
    delay(50);
    Wire.setTimeout(50);

    // 1) Watchdog FIRST — prevents "task not found" and failed mid-setup init
    setupWatchdog();
    esp_task_wdt_reset();

    // ---- Preferences ----
    pref.begin("inc_timer", false);
        // ---- تحميل الإعدادات الديناميكية ----
    loadConfig();

    // NTP optional (fails offline quickly). Only after WDT is registered.
    // if (syncTime()) {
    //     Serial.println(String("✅ Time synchronized: ") + getTimeString());
    // } else {
    //     Serial.println("⚠️ Failed to sync time. Using default.");
    // }
    esp_task_wdt_reset();

    // ---- قراءة وزيادة bootCount ----
    bootCount = pref.getUChar("bootCount", 0);
    bootCount++;
    pref.putUChar("bootCount", bootCount);
    Serial.printf("Boot #%d\n", bootCount);

    Serial.println("System Starting...");
    Serial.printf("%s v%s\n", FIRMWARE_NAME, FIRMWARE_VERSION);
    lastMillis = millis();
    Serial.printf("🔍 Last reset reason raw code: %d\n", esp_reset_reason());

    // ---- Restore safety / timer state from NVS ----
    elapsedSec = pref.getULong("elapsed_sec", 0);
    cycleCount = pref.getULong("cycle_count", 0);
    safeMode = pref.getBool("safe_mode", false);
    emergencyMode = pref.getBool("emergency", false);

    if (elapsedSec >= TARGET_SEC) {
        elapsedSec = 0;
        pref.putULong("elapsed_sec", 0);
        Serial.println("⚠️ Invalid NVS value, reset to 0");
    }

    // WDT already configured in setupWatchdog() — do NOT call esp_task_wdt_init again
    esp_task_wdt_reset();

// ---- تهيئة المشغلات ----
    for (int i = 0; i < ACTUATOR_COUNT; i++) {
        pinMode(actuators[i].pin, OUTPUT);
        digitalWrite(actuators[i].pin, LOW);
        actuators[i].state = false;
    }
    pinMode(PIN_SENSOR_FAILED, OUTPUT);
    digitalWrite(PIN_SENSOR_FAILED, LOW);
    
    // ---- تهيئة الحساسات ----
    initSensors();
    
    // ---- تهيئة الشاشة ----
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("System Ready!");
    {
        unsigned long t0 = millis();
        while ((millis() - t0) < 1000UL) {
            esp_task_wdt_reset();
            delay(10);
            esp_task_wdt_reset();  // بعد كل delay طويل

        }
    }
    lcd.clear();    
    lcdInitialized = true;
    
    // ---- استعادة حالة السلامة ----
    if (safeMode) {
        Serial.println("⚠️ System starting in SAFE MODE");
        setActuator(0, true);
        setActuator(1, false);
        setActuator(2, false);
    }
    
    if (emergencyMode) {
        Serial.println("🚨 System starting in EMERGENCY MODE");
        setActuator(0, true);
        setActuator(1, false);
        setActuator(2, false);
    }
    
    logEvent("System started");
    
    Serial.println("✅ System Ready!");
    Serial.printf("🧠 Free heap at boot: %u bytes\n", (unsigned)ESP.getFreeHeap());
    diag_init();
    diag_io_init();
    
    // Boot diagnostics log (NVS) + optional Telegram if online
    update_last_reset_reason();
    esp_task_wdt_reset();

    // DetailedLog log;
    // log.id = nextLogId;
    // log.type = LOG_TYPE_RESET;
    // snprintf(log.reason, sizeof(log.reason), "%s", last_reset_reason);
    // {
    //     String ts = getTimeString();
    //     snprintf(log.timestamp, sizeof(log.timestamp), "%s", ts.c_str());
    // }
    // log.uptimeSec = millis() / 1000UL;
    // log.value = 0.0f;
    // log.code = 0;
    // saveLog(log);

    // Serial.println("===== BOOT LOG =====");
    // Serial.printf("Time: %s", log.timestamp);
    // Serial.printf("Uptime: %lu sec", log.uptimeSec);
    // Serial.printf("Reason: %s", log.reason);

    if (wifiConnected) {
        char bootMsg[200];
        // snprintf(bootMsg, sizeof(bootMsg),
                //  "Boot Report Time: %s Uptime: %lu s Reason: %s",
                //  log.timestamp, log.uptimeSec, log.reason);
        // snprintf(bootMsg, sizeof(bootMsg),
            //  "Boot Report Time: %s Uptime: %lu s Reason: %s",
            //  log.timestamp, log.uptimeSec, log.reason);

        esp_task_wdt_reset();
        tgSend(bootMsg);
        esp_task_wdt_reset();
    } else {
        Serial.println("📡 Boot TG skipped (offline)");
    }

    esp_task_wdt_reset();
}

// ========================================================================
// 15. دالة loop()
// ========================================================================

// ============================================================
// Finite State Machine – Safety-Critical Design
// ESP32 Egg Incubator – Industrial Grade
// ============================================================



// ============================================================
// loop() – Precision Timing + Safety-Critical FSM
// ============================================================
void loop() {
    // --------------------------------------------------------
    // 1. Watchdog – أول سطر بدون أي شرط (أمان أقصى)
    // --------------------------------------------------------
    esp_task_wdt_reset();

    const unsigned long now = millis();

    // --------------------------------------------------------
    // 2. مهام فائقة السرعة (كل دورة ≈ 1–5 ms)
    // --------------------------------------------------------
    handleSerialCommands();

    if (waitingForConfirm) {
        if (Serial.available()) {
            char confirm[16] = {0};
            size_t n = 0;
            while (Serial.available() && n < sizeof(confirm) - 1) {
                char ch = Serial.read();
                if (ch == '\n' || ch == '\r') break;
                confirm[n++] = ch;
            }
            if (strcmp(confirm, "CONFIRM") == 0 || strcmp(confirm, "confirm") == 0) {
                clearAllDiagnostics();
                Serial.println("✅ All diagnostics reset to zero.");
                waitingForConfirm = false;
            } else {
                Serial.println("❌ Confirmation cancelled.");
                waitingForConfirm = false;
            }
        } else if ((int32_t)(now - confirmTimeout) >= 0) {
            Serial.println("❌ Confirmation timeout.");
            waitingForConfirm = false;
        }
    }

    // --------------------------------------------------------
    // 3. المروحة دائمًا شغالة في كل الحالات (Failsafe)
    // --------------------------------------------------------
    if (!actuators[0].state) {
        setActuator(0, true);
    }

    // --------------------------------------------------------
    // 4. Hardware Fallback للسخان
    // FIX: يبقى الشرط شاملاً currentState أو emergencyMode لمنع أي نافذة زمنية
    // --------------------------------------------------------
    if (currentState == STATE_EMERGENCY || emergencyMode) {
        if (actuators[2].state || heaterState) {
            digitalWrite(PIN_HEATER, LOW);
            actuators[2].state = false;
            heaterState = false;
        }
    }

    // --------------------------------------------------------
    // 5. المهمة المتوسطة – كل 2 ثانية
    //    قراءة الحساسات + LCD + تشخيص تعمل في كل الحالات
    // --------------------------------------------------------
    static unsigned long lastSensorTime = 0;
    if (now - lastSensorTime >= SENSOR_INTERVAL) {
        lastSensorTime = now;

        // قراءة الحساسات تعمل في كل الحالات
        readSensors();

        // ----------------------------------------------------
        // التحكم الخطير فقط في STATE_NORMAL
        // ----------------------------------------------------
        if (currentState == STATE_NORMAL) {
            controlHeater();

            // المقلب
                        // المقلب – يستخدم الإعدادات الديناميكية
            if (!isPredictiveModeActive()) {
                if (!turnerState) {
                    if (now - lastTurnerTime >= config.turnerOffTime) {
                        setActuator(1, true);
                        turnerState = true;
                        lastTurnerTime = now;
                        Serial.println("Turner ON");
                    }
                } else {
                    if (now - lastTurnerTime >= config.turnerOnTime) {
                        setActuator(1, false);
                        turnerState = false;
                        lastTurnerTime = now;
                        Serial.println("Turner OFF");
                    }
                }
            }

            // FIX: مراقبة وقت السخان وكشف التصاق الريلاي فقط في التشغيل الطبيعي
            checkHeaterRuntime();
            checkRelayStuckFault();

            // FIX: العداد التنازلي يعمل فقط أثناء التشغيل الطبيعي
            updateTimer();
        }

        // FIX: تأكيد إطفاء المقلب قسراً في SAFE و EMERGENCY
        if (currentState != STATE_NORMAL) {
            if (turnerState || actuators[1].state) {
                setActuator(1, false);
                turnerState = false;
            }
        }

        // المرطب: يعمل في NORMAL و SAFE، ويُطفأ في EMERGENCY
        if (currentState != STATE_EMERGENCY) {
            controlEvaporator();
        } else {
            // FIX: تأكيد إطفاء المرطب فوراً في الطوارئ (Hardware + Software)
            if (evaporator.state || actuators[3].state) {
                evaporator.state = false;
                setActuator(3, false);
            }
        }

        // التشخيص وقراءة البيانات تعمل في كل الحالات
        diag_sample();

        if (hasValidTemperature) {
            cycle_temp_sum += currentTemperature;
            cycle_sample_count++;
        }

        // LED عطل الحساسات
        bool allSensorsFailed = true;
        for (int i = 0; i < MAX_DS18B20_SENSORS; i++) {
            if (dsSensors[i].valid) {
                allSensorsFailed = false;
                break;
            }
        }
        digitalWrite(PIN_SENSOR_FAILED, allSensorsFailed ? HIGH : LOW);

        updateLCD();

        // طباعة حالة مختصرة (throttle) – تبقى خارج الـ switch
        static uint8_t statusPrintSkip = 0;
        if ((++statusPrintSkip & 1) == 0) {
            float hum = 0, dhtTemp = 0;
            for (int i = 0; i < MAX_HUMIDITY_SENSORS; i++) {
                if (humiditySensors[i].valid && !humiditySensors[i].bypassed) {
                    hum = humiditySensors[i].humidity;
                    dhtTemp = humiditySensors[i].temperature;
                    break;
                }
            }
            Serial.printf("T1: %.2f | T2: %.2f | Avg: %.1f | DHT: %.1f | Hum: %.1f | Heater: %s | Turner: %s | State: %s\n",
                          dsSensors[0].temperature,
                          dsSensors[1].temperature,
                          currentTemperature,
                          dhtTemp,
                          hum,
                          heaterState ? "ON" : "OFF",
                          turnerState ? "ON" : "OFF",
                          currentStateName);
        }
    }

    // --------------------------------------------------------
    // 6. المهمة البطيئة – كل 10 ثوانٍ
    // --------------------------------------------------------
    static unsigned long lastDiagPeriodic = 0;
    if ((uint32_t)(now - lastDiagPeriodic) >= 10000UL) {
        lastDiagPeriodic = now;
        diag_periodic();
    }

    // --------------------------------------------------------
    // 7. المهمة الخلفية – كل 100 مللي
    // --------------------------------------------------------
    static unsigned long lastIoLoop = 0;
    if ((uint32_t)(now - lastIoLoop) >= 100UL) {
        lastIoLoop = now;
        esp_task_wdt_reset();
        diag_io_loop();
        telegram_monitor_loop();
        esp_task_wdt_reset();
    }

    // --------------------------------------------------------
    // 8. بوابة أوامر التيليجرام (كل 3 ثوانٍ)
    // --------------------------------------------------------
    static unsigned long lastTelegramPoll = 0;
    if (wifiConnected && (now - lastTelegramPoll >= 3000UL)) {
        lastTelegramPoll = now;
        esp_task_wdt_reset();
        checkTelegramCommands();
        esp_task_wdt_reset();
    }

    // --------------------------------------------------------
    // 9. فحص LCD كل 5 ثوانٍ
    // --------------------------------------------------------
    static unsigned long lastLCDCheck = 0;
    if (now - lastLCDCheck >= 5000UL) {
        lastLCDCheck = now;
        Wire.beginTransmission(0x27);
        if (Wire.endTransmission() != 0) {
            if (lcdInitialized) {
                Serial.println("⚠️ LCD lost connection");
                lcdInitialized = false;
            }
        } else if (!lcdInitialized) {
            reinitLCD();
        }
    }

    // ============================================================
    // 10. آلة الحالات – الانتقالات
    // ============================================================
    switch (currentState) {
        case STATE_BOOT:
            if (emergencyMode) {
                changeState(STATE_EMERGENCY);
            } else if (safeMode) {
                changeState(STATE_SAFE);
            } else {
                changeState(STATE_NORMAL);
            }
            break;

        case STATE_NORMAL:
            if (emergencyMode) {
                changeState(STATE_EMERGENCY);
            } else if (safeMode) {
                changeState(STATE_SAFE);
            }
            break;

        case STATE_SAFE:
            if (emergencyMode) {
                changeState(STATE_EMERGENCY);
            } else if (!safeMode) {
                changeState(STATE_NORMAL);
            }
            break;

        case STATE_EMERGENCY:
            // منطقة ميتة 10 ثوانٍ تتم داخل exitEmergencyMode()
            if (!emergencyMode) {
                if (safeMode) {
                    changeState(STATE_SAFE);
                } else {
                    changeState(STATE_NORMAL);
                }
            }
            break;
    }
}