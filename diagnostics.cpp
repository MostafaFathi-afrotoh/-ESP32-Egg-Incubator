// diagnostics.cpp
// Phase 1 – Foundation: Welford, bandTime, counters, REPORT, NVS persistence
// Observational only – never modifies control logic.

#include "diagnostics.h"
#include <Preferences.h>
#include <esp_system.h>
#include "telegram_alerts.h"  
#include <math.h>
#include <string.h>
// #include "diag_io.h"
// ---- FIX: تمت إزالة تعريف struct Actuator المكرر هنا - أصبح
// يُستورد من diagnostics.h (المصدر الوحيد الآن) بدلاً من نسخة
// محلية منفصلة كانت قد تنحرف عن نسخة Memory5.ino بصمت.
extern float avgHumidity;
extern uint32_t humidityFailCount;
extern uint32_t humiditySamples;
// ---- إعلانات مسبقة للدوال ----
bool loadDiagnosticsBlob();
void saveDiagnosticsBlob();

// ============================================================
// تعريف الهياكل الأساسية (في النطاق العام)
// ============================================================

// ThermalStats: Welford online mean/variance
struct ThermalStats
{
    uint32_t n;
    float mean;
    float m2; // sum of squared differences from mean
    float minT;
    float maxT;
};


// ============================================================
// المرحلة النهائية: المصفوفات اليومية وحساب التدهور
// ============================================================
static float dayDeltaAvg[30] = {0};
static float dayDuty[30] = {0};
static float dayOverAvg[30] = {0};
static float dayMeanDev[30] = {0};
static uint8_t dayHealth[30] = {0};
static uint8_t dayIdx = 0;           // اليوم الحالي (0-29)
static uint8_t dayCountFilled = 0;   // grows to 30 then stays (sliding window size)
static unsigned long lastDayStart = 0; // وقت بداية اليوم الحالي (millis)

// ---- عدادات المشغلات (Actuator) ----
static uint32_t diagSwitchCount[4] = {0, 0, 0, 0};  // 0=Fan, 1=Turner, 2=Heater, 3=Evaporator
static uint32_t diagOnMs[4] = {0, 0, 0, 0};
static unsigned long lastOnStart[4] = {0, 0, 0, 0};

// ---- إحصائيات الرطوبة (للتوسع المستقبلي) ----
// تم نقل التعريفات إلى Memory4.ino واستخدامها عبر extern

// ============================================================
// هيكل التخزين المزدوج مع CRC (Dual‑blob)
// ============================================================
#pragma pack(push, 1)
struct DiagBlob {
    uint32_t magic;          // 0xABADBABE
    uint32_t version;        // 1
    uint32_t timestamp;      // وقت الحفظ (millis)
    uint16_t crc;            // CRC16 للتحقق

    // ---- الإحصائيات الأساسية ----
    ThermalStats thermal;
    float bandTime[6];
    uint32_t totalSamples;
    uint32_t heaterCycles;
    uint32_t diagUptimeSec;

    // ---- عدادات إعادة التشغيل ----
    uint32_t resetUnexpected;
    uint32_t resetWdt;
    uint32_t resetBrownout;

    // ---- انقطاع الحساسات ----
    uint32_t sensorDisconnectCount;

    // ---- التجاوز (Overshoot) ----
    uint32_t overCount;
    float overSum;
    float overMax;
    uint32_t overHist[20];

    // ---- فرق الحساسات (Delta) ----
    uint32_t deltaN;
    float deltaSum;
    float deltaMax;

    // ---- أداء السخان ----
    uint32_t heaterOnTotalMs;
    uint32_t onTimeSumMs;
    uint32_t onTimeCount;
    uint32_t invalidReads;
    uint32_t tempHist[150];

    // ---- المصفوفات اليومية (30 يوم) ----
    float dayDeltaAvg[30];
    float dayDuty[30];
    float dayOverAvg[30];
    float dayMeanDev[30];
    uint8_t dayHealth[30];
    uint8_t dayIdx;
    uint32_t lastDayStart;

    // ---- آخر 8 أعطال للحفظ ----
    FaultRec savedFaults[8];
};
#pragma pack(pop)

static DiagBlob blob;
static bool blobDirty = false;
static uint8_t activeBlobIdx = 0; // 0 أو 1
static const char* BLOB_KEYS[2] = {"diag_blk_a", "diag_blk_b"};
// ============================================================
static unsigned long lastSaveTime = 0;
// إعلان الدوال الخارجية (معرفة في Memory3.ino)
// ============================================================
void logEvent(const char* message);
// ============================================================
// تعريف الهيكل المستخدم من Memory3.ino (نسخة مطابقة)
// ============================================================
// DSTemperatureSensor defined once in diagnostics.h
extern DSTemperatureSensor dsSensors[2];
// ============================================================
// 1. Data structures & state (static, internal to this module)
// ============================================================

// تعريف المتغير العام thermal (يستخدم في كل مكان)
static ThermalStats thermal = {0, 0.0f, 0.0f, 999.0f, -999.0f};
// ============================================================
// Phase 2: Overshoot Tracker & Sensor Delta
// ============================================================

// Heater Overshoot State Machine
struct HeaterOvershoot
{
    bool armed;
    unsigned long offMs;
    float offTemp;
    float peak;
    uint32_t samples;
};
static HeaterOvershoot overshoot = {false, 0, 0.0f, 0.0f, 0};

// Overshoot histogram (bins of 0.05°C, from 0.0 to 1.0°C)
static uint32_t overHist[20] = {0};

// Sensor Delta stats (difference between DS18B20 sensors)
struct SensorDelta
{
    uint32_t n;
    float sum;
    float max;
};
static SensorDelta sensorDelta = {0, 0.0f, 0.0f};

// Running stats for overshoot
static uint32_t overCount = 0;
static float overSum = 0.0f;
static float overMax = 0.0f;
// Temperature bands (seconds spent in each):
// 0: <36.0, 1: 36.0-37.0, 2: 37.0-37.5, 3: 37.5-38.0, 4: 38.0-38.5, 5: >38.5
static float bandTime[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// Counters
static uint32_t totalSamples = 0;
static uint32_t heaterCycles = 0; // counts OFF transitions only
static uint32_t diagUptimeSec = 0;

// Reset reason counters (accumulated since last clear)
static uint32_t resetUnexpected = 0;
static uint32_t resetWdt = 0;
static uint32_t resetBrownout = 0;

// Internal flags
static bool heaterWasOn = false; // for edge detection
// تم استبدال dirty بـ blobDirty (يُستخدم في saveDiagnosticsBlob)
static const unsigned long SAVE_INTERVAL_MS = 10000UL; // 10 ثواني 

// ============================================================
// Phase 3: Sensor Disconnect Counter
// ============================================================
uint32_t sensorDisconnectCount = 0;
static bool lastSensorDisconnectState[2] = {false, false};

// ============================================================
// Phase 3: Fault Ring Buffer (32 events)
// ============================================================
// Phase 3.5: متغيرات إضافية (Duty Cycle, P95, Invalid Reads)
// ============================================================

// رسم بياني للحرارة (150 خانة من 30.0 إلى 45.0 درجة، بدقة 0.1)
static uint32_t tempHist[150] = {0};

// عدادات السخان
static uint32_t heaterOnTotalMs = 0;       // إجمالي وقت تشغيل السخان (مللي ثانية)
static uint32_t onTimeSumMs = 0;           // مجموع أوقات تشغيل السخان في كل دورة (مللي ثانية)
static uint32_t onTimeCount = 0;           // عدد دورات تشغيل السخان
static unsigned long currentOnStartMs = 0; // وقت بدء التشغيل الحالي

// عداد القراءات غير الصالحة (لحساسات DS18B20 فقط)
static uint32_t invalidReads = 0;
// ============================================================
// FIX (Phase 7 precision): متتبّع "الدورة الحالية فقط" عبر لقطة
// (baseline) من العدادات التراكمية الموجودة أصلاً، ثم الفرق.
// لا يلمس منطق diag_sample() الحالي إطلاقاً - فقط يقرأ عدادات
// تراكمية موجودة أصلاً (heaterOnTotalMs, diagUptimeSec, overCount,
// overSum) ويحسب الفرق منذ آخر بداية دورة.
// ============================================================
static uint32_t cycleBaselineHeaterOnMs   = 0;
static unsigned long cycleBaselineUptimeSec = 0;
static uint32_t cycleBaselineOverCount    = 0;
static float    cycleBaselineOverSum      = 0.0f;
// ============================================================
#define FAULT_RING_SIZE 32
// #define FAULT_CRITICAL 2
// #define FAULT_EMERGENCY 3

// تعريف FaultRec مستخدم هنا (من النطاق العام أعلاه)
FaultRec faultRing[FAULT_RING_SIZE];
uint8_t faultHead = 0;
static uint32_t faultCount = 0;

// Fault codes
#define FAULT_OVERSHOOT_HIGH 0x05
#define FAULT_EMERGENCY_TEMP 0x08
#define FAULT_SENSOR_DISCONNECT 0x01
#define FAULT_SAFE_MODE_ENTER 0x10
#define FAULT_SENSOR_DELTA_HIGH 0x02
#define FAULT_WDT_RESET 0x0B
#define FAULT_BROWNOUT_RESET 0x0C

// Preferences object (kept open)
static Preferences prefDiag;
static Preferences prefLog;

// ============================================================
// 2. Helper: get band index from temperature
// ============================================================
static int getBandIndex(float temp)
{
    if (temp < 36.0f)
        return 0;
    if (temp < 37.0f)
        return 1;
    if (temp < 37.5f)
        return 2;
    if (temp < 38.0f)
        return 3;
    if (temp < 38.5f)
        return 4;
    return 5;
}


// ============================================================
// CRC16 (CCITT) بسيطة وسريعة
// ============================================================
static uint16_t calculateCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

// ============================================================
// 3. diag_init – call once in setup()
// ============================================================
// تم إزالة الكود القديم المعلق (يحتفظ بالتعليقات)
// ============================================================
// تتبع المشغلات (تُستدعى من setActuator)
// ============================================================
void diag_actuator_event(int idx) {
    if (idx < 0 || idx >= 4) return; // 0=Fan, 1=Turner, 2=Heater, 3=Evaporator
    
    bool newState = actuators[idx].state;
    unsigned long now = millis();
    
    if (newState) {
        // بداية تشغيل
        lastOnStart[idx] = now;
    } else {
        // نهاية تشغيل → احسب المدة
        if (lastOnStart[idx] != 0) {
            diagOnMs[idx] += (now - lastOnStart[idx]);
            lastOnStart[idx] = 0;
        }
        diagSwitchCount[idx]++;
        blobDirty = true;
    }
}
void diag_init() {
    prefDiag.begin("inc_diag", false);

    // ---- محاولة استعادة البيانات من النسخة المزدوجة ----
    if (loadDiagnosticsBlob()) {
        // استعادة كل المتغيرات من الـ blob
        thermal = blob.thermal;
        memcpy(bandTime, blob.bandTime, sizeof(bandTime));
        totalSamples = blob.totalSamples;
        heaterCycles = blob.heaterCycles;
        diagUptimeSec = blob.diagUptimeSec;

        resetUnexpected = blob.resetUnexpected;
        resetWdt = blob.resetWdt;
        resetBrownout = blob.resetBrownout;

        sensorDisconnectCount = blob.sensorDisconnectCount;

        overCount = blob.overCount;
        overSum = blob.overSum;
        overMax = blob.overMax;
        memcpy(overHist, blob.overHist, sizeof(overHist));

        sensorDelta.n = blob.deltaN;
        sensorDelta.sum = blob.deltaSum;
        sensorDelta.max = blob.deltaMax;

        heaterOnTotalMs = blob.heaterOnTotalMs;
        onTimeSumMs = blob.onTimeSumMs;
        onTimeCount = blob.onTimeCount;
        invalidReads = blob.invalidReads;
        memcpy(tempHist, blob.tempHist, sizeof(tempHist));

        memcpy(dayDeltaAvg, blob.dayDeltaAvg, sizeof(dayDeltaAvg));
        memcpy(dayDuty, blob.dayDuty, sizeof(dayDuty));
        memcpy(dayOverAvg, blob.dayOverAvg, sizeof(dayOverAvg));
        memcpy(dayMeanDev, blob.dayMeanDev, sizeof(dayMeanDev));
        memcpy(dayHealth, blob.dayHealth, sizeof(dayHealth));
        dayIdx = blob.dayIdx;
        lastDayStart = blob.lastDayStart;
        // Estimate filled days without expanding DiagBlob layout
        dayCountFilled = 0;
        for (int di = 0; di < 30; di++) {
            if (dayHealth[di] != 0 || dayDuty[di] != 0.0f) dayCountFilled++;
        }
        if (dayCountFilled < dayIdx) dayCountFilled = dayIdx;
        if (dayCountFilled > 30) dayCountFilled = 30;

        // Restore last-8 faults; use persisted faultCount when available
        uint8_t restored = 0;
        for (int i = 0; i < 8; i++) {
            if (blob.savedFaults[i].ts != 0 || blob.savedFaults[i].code != 0) {
                uint8_t idx = restored % FAULT_RING_SIZE;
                faultRing[idx] = blob.savedFaults[i];
                restored++;
            }
        }
        uint32_t storedCount = prefDiag.getULong("faultCount", restored);
        if (storedCount > FAULT_RING_SIZE) storedCount = FAULT_RING_SIZE;
        // Prefer the larger of restored slots vs stored count (both are lower bounds)
        faultCount = (storedCount > restored) ? storedCount : restored;
        if (faultCount > FAULT_RING_SIZE) faultCount = FAULT_RING_SIZE;
        faultHead = (uint8_t)(faultCount % FAULT_RING_SIZE);
        blobDirty = false;
    } else {
        // ---- لا توجد بيانات سابقة → تهيئة من الصفر ----
        thermal.n = 0; thermal.mean = 0; thermal.m2 = 0; thermal.minT = 999; thermal.maxT = -999;
        for (int i = 0; i < 6; i++) bandTime[i] = 0;
        totalSamples = 0; heaterCycles = 0; diagUptimeSec = 0;
        resetUnexpected = 0; resetWdt = 0; resetBrownout = 0;
        sensorDisconnectCount = 0;
        overCount = 0; overSum = 0; overMax = 0; memset(overHist, 0, sizeof(overHist));
        sensorDelta.n = 0; sensorDelta.sum = 0; sensorDelta.max = 0;
        heaterOnTotalMs = 0; onTimeSumMs = 0; onTimeCount = 0; invalidReads = 0; memset(tempHist, 0, sizeof(tempHist));
        memset(dayDeltaAvg, 0, sizeof(dayDeltaAvg)); memset(dayDuty, 0, sizeof(dayDuty));
        memset(dayOverAvg, 0, sizeof(dayOverAvg)); memset(dayMeanDev, 0, sizeof(dayMeanDev));
        memset(dayHealth, 0, sizeof(dayHealth)); dayIdx = 0; dayCountFilled = 0; lastDayStart = millis();
        memset(faultRing, 0, sizeof(faultRing)); faultHead = 0; faultCount = 0;
        blobDirty = true;
    }

    // ---- قراءة سبب إعادة التشغيل وزيادة العدادات ----
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_WDT: resetWdt++; blobDirty = true; addFault(FAULT_WDT_RESET, FAULT_CRITICAL, 0); break;
        case ESP_RST_BROWNOUT: resetBrownout++; blobDirty = true; addFault(FAULT_BROWNOUT_RESET, FAULT_CRITICAL, 0); break;
        case ESP_RST_POWERON: break;
        default: resetUnexpected++; blobDirty = true; break;
    }
    diagUptimeSec = prefDiag.getULong("diag_uptime", 0);


    lastSaveTime = millis();

    // FIX: تهيئة لقطة الدورة الحالية عند الإقلاع
    diag_cycleReset();
}
// ============================================================
// Phase 2: Getters for Overshoot and Delta
// ============================================================

float getAvgOvershoot()
{
    return (overCount > 0) ? (overSum / overCount) : 0.0f;
}

float getMaxOvershoot()
{
    return overMax;
}

float getP95Overshoot()
{
    // Compute P95 from histogram (20 bins, width 0.05)
    uint32_t total = 0;
    for (int i = 0; i < 20; i++)
        total += overHist[i];
    if (total == 0)
        return 0.0f;

    uint32_t cum = 0;
    uint32_t target = (uint32_t)(total * 0.95f);
    for (int i = 0; i < 20; i++)
    {
        cum += overHist[i];
        if (cum >= target)
        {
            return (float)(i + 1) * 0.05f; // upper edge of bin
        }
    }
    return 1.0f; // > 1.0°C
}

float getAvgDelta()
{
    return (sensorDelta.n > 0) ? (sensorDelta.sum / sensorDelta.n) : 0.0f;
}

float getMaxDelta()
{
    return sensorDelta.max;
}

// ============================================================
// Phase 3: Track Sensor Disconnections
// ============================================================
void diag_trackSensorDisconnect()
{
    extern DSTemperatureSensor dsSensors[];
    for (int i = 0; i < 2; i++)
    {
        bool currentlyDisconnected = !dsSensors[i].valid;
        if (!lastSensorDisconnectState[i] && currentlyDisconnected)
        {
            sensorDisconnectCount++;
            prefDiag.putULong("sensorDisconn", sensorDisconnectCount);
            blobDirty = true;
            addFault(FAULT_SENSOR_DISCONNECT, FAULT_WARNING, i);
            Serial.printf("📊 Sensor[%d] disconnected! Total disconnects: %lu\n", i, sensorDisconnectCount);
        }
        lastSensorDisconnectState[i] = currentlyDisconnected;
    }
}
// ============================================================
// Phase 3.5: Getters للإضافات المتقدمة
// ============================================================

float getP95Temp()
{
    uint32_t total = 0;
    for (int i = 0; i < 150; i++)
        total += tempHist[i];
    if (total == 0)
        return 0.0f;

    uint32_t cum = 0;
    uint32_t target = (uint32_t)(total * 0.95f);
    for (int i = 0; i < 150; i++)
    {
        cum += tempHist[i];
        if (cum >= target)
        {
            return 30.0f + (float)(i + 1) * 0.1f; // الحافة العليا للخانة
        }
    }
    return 45.0f;
}

uint32_t getInvalidReads()
{
    return invalidReads;
}

float getDutyCycle()
{
    // إذا لم يمر وقت كافٍ، ارجع 0
    if (diagUptimeSec == 0)
        return 0.0f;

    float totalOnSec = (float)heaterOnTotalMs / 1000.0f;
    float duty = (totalOnSec / (float)diagUptimeSec) * 100.0f;
    
    // حماية من الأخطاء المنطقية
    if (duty > 100.0f) duty = 100.0f;
    if (duty < 0.0f)   duty = 0.0f;
    
    return duty;
}
float getAvgOnTime()
{
    if (onTimeCount == 0)
        return 0.0f;
    return ((float)onTimeSumMs / (float)onTimeCount) / 1000.0f; // بالثواني
}

// ============================================================
// FIX (Phase 7 precision): إحصائيات خاصة بالدورة الحالية فقط
// بدلاً من القيم التراكمية منذ الإقلاع التي كانت تُمرَّر سابقاً
// إلى cycle_archive_onCycleComplete() - ما كان يجعل قيم الدورات
// المتتالية شبه متطابقة (لأنها لقطات من نفس المتوسط التراكمي
// البطيء)، ويُضعف فعلياً كشف التدهور بين الدورات.
// ============================================================
void diag_cycleReset()
{
    cycleBaselineHeaterOnMs = heaterOnTotalMs;
    cycleBaselineUptimeSec  = diagUptimeSec;
    cycleBaselineOverCount  = overCount;
    cycleBaselineOverSum    = overSum;
}

float getCycleDutyCycle()
{
    uint32_t deltaOnMs = heaterOnTotalMs - cycleBaselineHeaterOnMs;
    unsigned long deltaUptimeSec = diagUptimeSec - cycleBaselineUptimeSec;
    if (deltaUptimeSec == 0)
        return 0.0f;
    return ((float)deltaOnMs / 1000.0f) / (float)deltaUptimeSec * 100.0f;
}

float getCycleAvgOvershoot()
{
    uint32_t deltaCount = overCount - cycleBaselineOverCount;
    float deltaSum = overSum - cycleBaselineOverSum;
    return (deltaCount > 0) ? (deltaSum / deltaCount) : 0.0f;
}

// ============================================================
// 4. diag_sample – call once per sensor cycle (every 2s)
// ============================================================
void diag_sample()
{
    // Only sample if we have a valid temperature (rawTemperature is global)
    extern float rawTemperature;
    extern bool hasValidTemperature;
    extern bool heaterState;

    if (!hasValidTemperature)
        return;

    float temp = rawTemperature;
    // ============================================================
    // Phase 3.5: تتبع وقت تشغيل السخان (Duty Cycle & Avg ON Time)
    // ============================================================
    static bool lastHeaterStateForTiming = false;
    bool currentHeaterState = heaterState;

    // اكتشاف حافة ON (انتقال من OFF إلى ON)
    if (!lastHeaterStateForTiming && currentHeaterState)
    {
        currentOnStartMs = millis();
    }
    // اكتشاف حافة OFF (انتقال من ON إلى OFF)
    if (lastHeaterStateForTiming && !currentHeaterState)
    {
        if (currentOnStartMs != 0)
        {
            unsigned long onTime = millis() - currentOnStartMs;
            if (onTime > 100)
            { // تجاهل التشغيلات القصيرة جداً (أقل من 0.1 ثانية)
                onTimeSumMs += onTime;
                onTimeCount++;
            }
            currentOnStartMs = 0;
        }
    }
    // إذا كان السخان يعمل حالياً، أضف الوقت المنقضي منذ آخر تحديث إلى الإجمالي
    if (currentHeaterState)
    {
        // لا نضيف هنا مباشرة، بل نضيف في نهاية الدورة أو نستخدم طريقة أخرى.
        // الطريقة الأفضل: إضافة الوقت المنقضي بين القراءات (2 ثانية) إذا كان السخان يعمل.
        // Accumulate heater on-time using measured inter-sample deltaMs (not a fixed 2 s).
        // Sample interval baseline ~SENSOR_INTERVAL; actual on-time uses measured deltaMs below.
    }
    lastHeaterStateForTiming = currentHeaterState;

    // بدلاً من الطريقة أعلاه، سنستخدم طريقة مباشرة: إضافة وقت العينة (2 ثانية) إلى الإجمالي إذا كان السخان يعمل.
    // هذا أكثر دقة ويتوافق مع تردد العينات.
    // Actual inter-sample delta (always advance, for duty + bandTime)
    static unsigned long lastSampleMs = 0;
    unsigned long nowMs = millis();
    unsigned long deltaMs = (lastSampleMs == 0) ? 2000UL : (nowMs - lastSampleMs);
    if (deltaMs > 10000UL) deltaMs = 2000UL;  // clamp after long stall
    lastSampleMs = nowMs;
    if (currentHeaterState) {
        heaterOnTotalMs += deltaMs;
    }

    // ---- Update Welford ----
    thermal.n++;
    float delta = temp - thermal.mean;
    thermal.mean += delta / thermal.n;
    thermal.m2 += delta * (temp - thermal.mean);
    // ============================================================
    // Phase 3.5: تحديث رسم بياني الحرارة (P95)
    // ============================================================
    int bin = (int)((temp - 30.0f) / 0.1f);
    if (bin >= 0 && bin < 150)
    {
        tempHist[bin]++;
    }
    if (temp < thermal.minT)
        thermal.minT = temp;
    if (temp > thermal.maxT)
        thermal.maxT = temp;

    // ---- Update bandTime with actual delta ----
    float seconds = (float)deltaMs / 1000.0f;
    int band = getBandIndex(temp);
    if (band >= 0 && band < 6)
        bandTime[band] += seconds;

    // ---- Count total samples ----
    totalSamples++;

    // ---- Detect heater OFF transition (ON -> OFF) ----
    if (heaterWasOn && !heaterState)
    {
        heaterCycles++;
        blobDirty = true;
    }
    heaterWasOn = heaterState;

    // Mark dirty if any change occurred (we do it above already)
    // but we also set dirty if any new sample changes stats (always)
    static bool lastHeaterState = false;
    // bool currentHeaterState = heaterState;

    if (lastHeaterState && !currentHeaterState)
    {
        // Heater just turned OFF -> arm overshoot window
        overshoot.armed = true;
        overshoot.offMs = millis();
        overshoot.offTemp = rawTemperature;
        overshoot.peak = rawTemperature;
        overshoot.samples = 0;
    }
    lastHeaterState = currentHeaterState;

    // If armed and we have valid temp, track peak
    if (overshoot.armed && hasValidTemperature)
    {
        overshoot.samples++;
        if (rawTemperature > overshoot.peak)
        {
            overshoot.peak = rawTemperature;
        }
        // Check if window expired (120 seconds)
        if ((millis() - overshoot.offMs) >= 120000UL)
        {
            // Calculate overshoot
            if (overshoot.samples >= 3)
            {
                float overshootVal = overshoot.peak - overshoot.offTemp;
                if (overshootVal < 0)
                    overshootVal = 0;
                // Update running stats
                overCount++;
                overSum += overshootVal;
                if (overshootVal > overMax)
                    overMax = overshootVal;
                // Update histogram (bin width 0.05°C, max 1.0°C)
                int bin = (int)(overshootVal / 0.05f);
                if (bin > 19)
                    bin = 19;
                overHist[bin]++;
                blobDirty = true;
                // توليد خطأ إذا كان التجاوز مرتفعاً
                if (overshootVal > 0.4f)
                {
                    addFault(FAULT_OVERSHOOT_HIGH, FAULT_CRITICAL, overshootVal);
                }
            }
            overshoot.armed = false;
        }
    }

    // ============================================================
    // Phase 2: Sensor Delta (DS18B20 agreement)
    // ============================================================
    extern DSTemperatureSensor dsSensors[];
    if (dsSensors[0].valid && dsSensors[1].valid)
    {
        float d = fabsf(dsSensors[0].temperature - dsSensors[1].temperature);
        sensorDelta.n++;
        sensorDelta.sum += d;
        if (d > sensorDelta.max)
            sensorDelta.max = d;

        // ----- تحسين كشف فرق الحساسين (مع مهلة ورفع العتبة) -----
        static float deltaHistory[3] = {0.0f, 0.0f, 0.0f};
        static int deltaIdx = 0;

        deltaHistory[deltaIdx] = d;
        deltaIdx = (deltaIdx + 1) % 3;

        // التحقق من أن آخر 3 قراءات جميعها تجاوزت العتبة الجديدة 0.7 درجة
        float threshold = 0.7f;
        if (deltaHistory[0] > threshold && deltaHistory[1] > threshold && deltaHistory[2] > threshold)
        {
            addFault(FAULT_SENSOR_DELTA_HIGH, FAULT_CRITICAL, d);
        }
    }

    // Phase 3: Track sensor disconnections
    diag_trackSensorDisconnect();

    // ============================================================
    // Phase 3.5: عد القراءات غير الصالحة (Invalid reads)
    // ============================================================
    if (!dsSensors[0].valid || !dsSensors[1].valid)
    {
        invalidReads++;
    }

    // Mark dirty if any change occurred
    blobDirty = true;
}

    // Mark 

// ============================================================
// Phase 3: Fault Ring Functions
// ============================================================

void addFault(uint8_t code, uint8_t level, float value)
{
    faultRing[faultHead].code = code;
    faultRing[faultHead].level = level;
    faultRing[faultHead].ts = millis() / 1000; // uptime seconds
    faultRing[faultHead].value = value;
    faultHead = (faultHead + 1) % FAULT_RING_SIZE;
    if (faultCount < FAULT_RING_SIZE)
        faultCount++;
    blobDirty = true;
    // حفظ فوري في NVS عند حدوث CRITICAL أو EMERGENCY
    if (level >= FAULT_CRITICAL)
    {
        saveDiagnosticsBlob(); // حفظ فوري

        prefDiag.putULong("faultHead", faultHead);
        prefDiag.putULong("faultCount", faultCount);
        // saveFaultsToNVS();
    }
    DetailedLog log;
    log.id = nextLogId;
    log.type = LOG_TYPE_FAULT;
    snprintf(log.reason, sizeof(log.reason), "Fault 0x%02X", code);
    strcpy(log.timestamp, getTimeString().c_str());
    log.uptimeSec = millis() / 1000;
    log.value = value;
    log.code = code;
    saveLog(log);


}
// ============================================================
// حفظ واستعادة البيانات كنسخة مزدوجة مع CRC
// ============================================================
void saveDiagnosticsBlob() {
    // 1. ملء الهيكل بالبيانات الحالية
    blob.magic = 0xABADBABE;
    blob.version = 1;
    blob.timestamp = millis();

    blob.thermal = thermal;
    memcpy(blob.bandTime, bandTime, sizeof(bandTime));
    blob.totalSamples = totalSamples;
    blob.heaterCycles = heaterCycles;
    blob.diagUptimeSec = diagUptimeSec;

    blob.resetUnexpected = resetUnexpected;
    blob.resetWdt = resetWdt;
    blob.resetBrownout = resetBrownout;

    blob.sensorDisconnectCount = sensorDisconnectCount;

    blob.overCount = overCount;
    blob.overSum = overSum;
    blob.overMax = overMax;
    memcpy(blob.overHist, overHist, sizeof(overHist));

    blob.deltaN = sensorDelta.n;
    blob.deltaSum = sensorDelta.sum;
    blob.deltaMax = sensorDelta.max;

    blob.heaterOnTotalMs = heaterOnTotalMs;
    blob.onTimeSumMs = onTimeSumMs;
    blob.onTimeCount = onTimeCount;
    blob.invalidReads = invalidReads;
    memcpy(blob.tempHist, tempHist, sizeof(tempHist));

    memcpy(blob.dayDeltaAvg, dayDeltaAvg, sizeof(dayDeltaAvg));
    memcpy(blob.dayDuty, dayDuty, sizeof(dayDuty));
    memcpy(blob.dayOverAvg, dayOverAvg, sizeof(dayOverAvg));
    memcpy(blob.dayMeanDev, dayMeanDev, sizeof(dayMeanDev));
    memcpy(blob.dayHealth, dayHealth, sizeof(dayHealth));
    blob.dayIdx = dayIdx;
    blob.lastDayStart = lastDayStart;

    // نسخ آخر 8 أعطال من الحلقة الدائرية
    uint8_t start = (faultHead >= 8) ? (faultHead - 8) : 0;
    for (int i = 0; i < 8; i++) {
        uint8_t idx = (start + i) % FAULT_RING_SIZE;
        blob.savedFaults[i] = faultRing[idx];
    }

    // 2. حساب CRC على الهيكل (عدا حقل CRC نفسه)
    size_t crcOffset = offsetof(DiagBlob, crc);
    blob.crc = calculateCRC16((uint8_t*)&blob + crcOffset + sizeof(uint16_t), 
                              sizeof(DiagBlob) - crcOffset - sizeof(uint16_t));

    // 3. الكتابة في النسخة غير النشطة
    uint8_t writeIdx = (activeBlobIdx == 0) ? 1 : 0;
    prefDiag.putBytes(BLOB_KEYS[writeIdx], (uint8_t*)&blob, sizeof(DiagBlob));
    
    // 4. تأكيد الكتابة بتبديل المؤشر
    activeBlobIdx = writeIdx;
    prefDiag.putUChar("activeIdx", activeBlobIdx);
    
    blobDirty = false;
    // Serial.println("💾 Dual‑blob saved successfully");
}

bool loadDiagnosticsBlob() {
    // قراءة المؤشر النشط المحفوظ
    activeBlobIdx = prefDiag.getUChar("activeIdx", 0);
    
    // محاولة تحميل النسخة النشطة أولاً
    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t idx = (activeBlobIdx + attempt) % 2;
        size_t len = prefDiag.getBytesLength(BLOB_KEYS[idx]);
        if (len != sizeof(DiagBlob)) continue;

        DiagBlob temp;
        prefDiag.getBytes(BLOB_KEYS[idx], (uint8_t*)&temp, sizeof(DiagBlob));

        if (temp.magic != 0xABADBABE || temp.version != 1) continue;

        // التحقق من CRC
        size_t crcOffset = offsetof(DiagBlob, crc);
        uint16_t calcCrc = calculateCRC16((uint8_t*)&temp + crcOffset + sizeof(uint16_t),
                                          sizeof(DiagBlob) - crcOffset - sizeof(uint16_t));
        if (temp.crc != calcCrc) continue;

        // البيانات صحيحة → نسخها إلى المتغيرات العامة
        memcpy(&blob, &temp, sizeof(DiagBlob));
        activeBlobIdx = idx;
        return true;
    }
    return false; // لا توجد نسخة صالحة
}
// void saveFaultsToNVS()
// {
//     // نحفظ آخر 8 أحداث فقط لتوفير المساحة
//     uint8_t start = (faultHead >= 8) ? (faultHead - 8) : 0;
//     for (int i = 0; i < 8; i++)
//     {
//         uint8_t idx = (start + i) % FAULT_RING_SIZE;
//         char key[8];
//         snprintf(key, sizeof(key), "f%dc", i);
//         prefDiag.putUChar(key, faultRing[idx].code);
//         snprintf(key, sizeof(key), "f%dl", i);
//         prefDiag.putUChar(key, faultRing[idx].level);
//         snprintf(key, sizeof(key), "f%dt", i);
//         prefDiag.putULong(key, faultRing[idx].ts);
//         snprintf(key, sizeof(key), "f%dv", i);
//         prefDiag.putFloat(key, faultRing[idx].value);
//     }
// }

// void restoreFaultsFromNVS()
// {
//     for (int i = 0; i < 8; i++)
//     {
//         char key[8];
//         snprintf(key, sizeof(key), "f%dc", i);
//         if (prefDiag.isKey(key))
//         {
//             uint8_t idx = (faultHead + i) % FAULT_RING_SIZE;
//             faultRing[idx].code = prefDiag.getUChar(key, 0);
//             snprintf(key, sizeof(key), "f%dl", i);
//             faultRing[idx].level = prefDiag.getUChar(key, 0);
//             snprintf(key, sizeof(key), "f%dt", i);
//             faultRing[idx].ts = prefDiag.getULong(key, 0);
//             snprintf(key, sizeof(key), "f%dv", i);
//             faultRing[idx].value = prefDiag.getFloat(key, 0.0f);
//         }
//     }
// }

// ============================================================
// 5. diag_periodic – call every ~10 seconds from loop()
// ============================================================
// ============================================================
// 5. diag_periodic – call every ~10 seconds from loop()
// ============================================================
void diag_periodic()
{
    // حساب الوقت الكلي بشكل تراكمي (وليس من الصفر)
    static unsigned long lastDiagTick = 0;
    unsigned long nowMillis = millis();
    
    if (lastDiagTick == 0) lastDiagTick = nowMillis;
    diagUptimeSec += (nowMillis - lastDiagTick) / 1000;
    lastDiagTick = nowMillis;

    // ✨ حفظ الوقت التراكمي بشكل دوري في الذاكرة
    if (blobDirty && (millis() - lastSaveTime >= SAVE_INTERVAL_MS)) {
        prefDiag.putULong("diag_uptime", diagUptimeSec);
        saveDiagnosticsBlob();
        lastSaveTime = millis();
    }

    // --- منطق اليوم (24 ساعة) ---
    if (lastDayStart == 0) lastDayStart = millis();
    if ((millis() - lastDayStart) >= 86400000UL) { // 24 ساعة
        // احفظ متوسطات اليوم المنتهي
        dayDeltaAvg[dayIdx] = getAvgDelta();
        dayDuty[dayIdx] = getDutyCycle();
        dayOverAvg[dayIdx] = getAvgOvershoot();
        dayMeanDev[dayIdx] = fabsf(thermal.mean - 37.7f);
        dayHealth[dayIdx] = (uint8_t)getHealthScore();
        
        dayIdx = (dayIdx + 1) % 30;
        if (dayCountFilled < 30) dayCountFilled++;
        lastDayStart = millis();
        blobDirty = true;
    }
    
    // حفظ البيانات إذا كانت متسخة ومر الوقت الكافي (فقط مرة واحدة، بدون تكرار)
    if (blobDirty && (millis() - lastSaveTime >= SAVE_INTERVAL_MS)) {
        saveDiagnosticsBlob();
        lastSaveTime = millis();
    }
}
// ============================================================
// 6. Health Score Calculation (Phase 3)
// ============================================================

float getHealthScore()
{
    // المكونات الستة، كل منها يبدأ بـ 100 ثم تنقص العقوبات
    float thermalScore = 100.0f;
    float overshootScore = 100.0f;
    float sensorScore = 100.0f;
    float heaterScore = 100.0f;
    float softwareScore = 100.0f;
    float actuatorScore = 100.0f;  // <-- تعريف واحد فقط

    // ----- 1. Thermal (penalties from diagnostics.h) -----
    float meanDev = fabsf(thermal.mean - 37.7f);
    if (meanDev > 0.3f)
        thermalScore -= PENALTY_MEAN_DEV_HIGH;
    else if (meanDev > 0.15f)
        thermalScore -= PENALTY_MEAN_DEV_MED;

    float spread = thermal.maxT - thermal.minT;
    if (spread > 0.6f)
        thermalScore -= PENALTY_SPREAD_HIGH;
    else if (spread > 0.4f)
        thermalScore -= PENALTY_SPREAD_MED;

    float totalBand = 0;
    for (int i = 0; i < 6; i++)
        totalBand += bandTime[i];
    if (totalBand > 0)
    {
        float idealPct = (bandTime[3] / totalBand) * 100.0f;
        if (idealPct < 80.0f)
            thermalScore -= PENALTY_IDEAL_BAND_LOW;
        else if (idealPct < 90.0f)
            thermalScore -= PENALTY_IDEAL_BAND_MED;
    }

    // ----- 2. Overshoot -----
    float avgOver = getAvgOvershoot();
    float p95Over = getP95Overshoot();
    if (p95Over > 0.4f)
        overshootScore -= PENALTY_OVERSHOOT_P95_HIGH;
    else if (p95Over > 0.25f)
        overshootScore -= PENALTY_OVERSHOOT_P95_MED;
    else if (avgOver > 0.2f)
        overshootScore -= PENALTY_OVERSHOOT_AVG;

    // ----- 3. Sensors -----
    float avgDelta = getAvgDelta();
    float maxDelta = getMaxDelta();
    if (maxDelta > 0.5f)
        sensorScore -= PENALTY_DELTA_MAX_HIGH;
    else if (maxDelta > 0.3f)
        sensorScore -= PENALTY_DELTA_MAX_MED;
    else if (avgDelta > 0.2f)
        sensorScore -= PENALTY_DELTA_AVG;

    if (sensorDisconnectCount > 0)
    {
        sensorScore -= min(PENALTY_SENSOR_DISCONNECT_CAP,
                           (float)sensorDisconnectCount * PENALTY_SENSOR_DISCONNECT_STEP);
    }

    // ----- 4. Heater (M8: عقوبة حسب معدل دورات/ساعة وليس مجرد >0) -----
    if (diagUptimeSec >= 3600UL && heaterCycles > 0) {
        float cph = (float)heaterCycles / ((float)diagUptimeSec / 3600.0f);
        if (cph > 120.0f) heaterScore -= PENALTY_HEATER_CYCLES * 2.0f;
        else if (cph > 60.0f) heaterScore -= PENALTY_HEATER_CYCLES;
        // أقل من 60 دورة/ساعة: لا عقوبة دائمة
    } else if (diagUptimeSec < 3600UL && heaterCycles > 200) {
        heaterScore -= PENALTY_HEATER_CYCLES;  // حماية مبكرة فقط عند نشاط مفرط
    }

    // ----- 5. Software -----
    if (resetUnexpected > 0)
        softwareScore -= PENALTY_RESET_UNEXPECTED * resetUnexpected;
    if (resetWdt > 0)
        softwareScore -= PENALTY_RESET_WDT * resetWdt;
    if (resetBrownout > 0)
        softwareScore -= PENALTY_RESET_BROWNOUT * resetBrownout;

    // ----- 6. Actuators (0=Fan, 1=Turner, 2=Heater, 3=Evaporator) -----
    if (diagSwitchCount[2] > 500) actuatorScore -= PENALTY_HEATER_HIGH_SWITCH;
    else if (diagSwitchCount[2] > 300) actuatorScore -= PENALTY_HEATER_MED_SWITCH;
    if (diagSwitchCount[0] < 10 && diagUptimeSec > 3600) actuatorScore -= PENALTY_FAN_LOW_SWITCH;
    if (diagUptimeSec > 0) {
        float duty = (float)diagOnMs[2] / (diagUptimeSec * 1000.0f);
        if (duty > 0.5f) actuatorScore -= PENALTY_HEATER_HIGH_DUTY;
    }
    
    if (actuatorScore < 0) actuatorScore = 0;

    // ---- الأوزان ----
    float weights[6] = {0.30f, 0.20f, 0.15f, 0.10f, 0.15f, 0.10f};
    float scores[6] = {thermalScore, overshootScore, sensorScore, heaterScore, softwareScore, actuatorScore};

    float totalScore = 0;
    for (int i = 0; i < 6; i++)
    {
        if (scores[i] < 0)
            scores[i] = 0;
        if (scores[i] > 100)
            scores[i] = 100;
        totalScore += scores[i] * weights[i];
    }

    // ---- القاعدة الإجبارية ----
    uint32_t nowSec = millis() / 1000;
    bool hasCritical = false;
    for (int i = 0; i < FAULT_RING_SIZE; i++)
    {
        if (faultRing[i].level >= FAULT_CRITICAL)
        {
            if (nowSec - faultRing[i].ts < 86400UL)
            {
                hasCritical = true;
                break;
            }
        }
    }
    if (hasCritical)
    {
        if (totalScore > 40.0f)
            totalScore = 40.0f;
    }

    return totalScore;
}
const char *getHealthClass()
{
    float score = getHealthScore();
    if (score >= 85.0f)
        return "EXCELLENT";
    else if (score >= 70.0f)
        return "GOOD";
    else if (score >= 50.0f)
        return "WARNING";
    else
        return "CRITICAL";
}

// ============================================================
// مسح جميع بيانات التشخيص والبدء من الصفر
// ============================================================
void clearAllDiagnostics()
{
    // ----- 1. تصفير المتغيرات الإحصائية -----
    thermal.n = 0;
    thermal.mean = 0.0f;
    thermal.m2 = 0.0f;
    thermal.minT = 999.0f;
    thermal.maxT = -999.0f;

    for (int i = 0; i < 6; i++) bandTime[i] = 0.0f;
    dayCountFilled = 0;
    totalSamples = 0;
    heaterCycles = 0;
    diagUptimeSec = 0;

    resetUnexpected = 0;
    resetWdt = 0;
    resetBrownout = 0;

    sensorDisconnectCount = 0;
    lastSensorDisconnectState[0] = false;
    lastSensorDisconnectState[1] = false;

    overCount = 0;
    overSum = 0.0f;
    overMax = 0.0f;
    memset(overHist, 0, sizeof(overHist));
    overshoot.armed = false;
    overshoot.offMs = 0;
    overshoot.offTemp = 0.0f;
    overshoot.peak = 0.0f;
    overshoot.samples = 0;

    sensorDelta.n = 0;
    sensorDelta.sum = 0.0f;
    sensorDelta.max = 0.0f;

    heaterOnTotalMs = 0;
    onTimeSumMs = 0;
    onTimeCount = 0;
    currentOnStartMs = 0;

    invalidReads = 0;
    memset(tempHist, 0, sizeof(tempHist));

    faultHead = 0;
    faultCount = 0;
    memset(faultRing, 0, sizeof(faultRing));

    // ----- 2. مسح مساحة NVS بالكامل -----
    prefDiag.clear();

    // ----- 3. إزالة مفاتيح الـ blob يدوياً (للتأكد) -----
    prefDiag.remove(BLOB_KEYS[0]);
    prefDiag.remove(BLOB_KEYS[1]);
    prefDiag.remove("activeIdx");
    prefDiag.remove("faultHead");
    prefDiag.remove("faultCount");

        // تصفير عدادات المشغلات
    for (int i = 0; i < 4; i++) {
        diagSwitchCount[i] = 0;
        diagOnMs[i] = 0;
        lastOnStart[i] = 0;
    }

    // ----- 4. إعادة ضبط المؤشرات -----
    activeBlobIdx = 0;
    blobDirty = true;
    lastSaveTime = millis();

    // ----- 5. حفظ الحالة النظيفة (blob فارغ) -----
    saveDiagnosticsBlob();

    Serial.println("🗑️ All diagnostics data cleared! Starting fresh.");
    Serial.println("📊 Type 'REPORT' to see the new clean state.");
}// 7. Print functions
// ============================================================

void printFullReport()
{
    Serial.println("\n========== SMART INCUBATOR RELIABILITY REPORT ==========");

    // Uptime
    unsigned long uptimeSec = diagUptimeSec;
    unsigned long days = uptimeSec / 86400;
    unsigned long hours = (uptimeSec % 86400) / 3600;
    unsigned long mins = (uptimeSec % 3600) / 60;
    unsigned long secs = uptimeSec % 60;
    Serial.printf("Uptime: %lud %02luh %02lum %02lus\n", days, hours, mins, secs);
    Serial.printf("Samples: %lu\n", totalSamples);
    Serial.printf("Heater Cycles: %lu\n", heaterCycles);
    extern unsigned long cycleCount;
    Serial.printf("Cycle Count: %lu\n", cycleCount);

    // في قسم SENSOR HEALTH أضف:
    Serial.printf("  Avg Humidity: %.1f %%\n", avgHumidity);
    Serial.printf("  Humidity Fails: %lu\n", humidityFailCount);

    // Thermal stats
    float variance = (thermal.n > 1) ? (thermal.m2 / thermal.n) : 0.0f;
    float stddev = sqrtf(variance);
    Serial.printf("\nTHERMAL PERFORMANCE\n");
    Serial.printf("  Avg: %.2f °C\n", thermal.mean);
    Serial.printf("  Min: %.2f  Max: %.2f\n", thermal.minT, thermal.maxT);
    Serial.printf("  Std Dev: %.3f °C\n", stddev);
    Serial.printf("  Spread: %.2f °C\n", thermal.maxT - thermal.minT);
    Serial.printf("  P95: %.2f °C\n", getP95Temp());
    // Band distribution
    float totalBandTime = 0.0f;
    for (int i = 0; i < 6; i++)
        totalBandTime += bandTime[i];
    if (totalBandTime > 0.0f)
    {
        float idealPct = (bandTime[3] / totalBandTime) * 100.0f;
        Serial.printf("  Time in Ideal band (37.5-38.0): %.1f %%\n", idealPct);
    }
    else
    {
        Serial.printf("  Time in Ideal band: N/A\n");
    }

    // 📌 THERMAL OVERSHOOT (Phase 2)
    Serial.printf("  Overshoot Avg: %.2f °C\n", getAvgOvershoot());
    Serial.printf("  Overshoot Max: %.2f °C\n", getMaxOvershoot());
    Serial.printf("  Overshoot P95: %.2f °C\n", getP95Overshoot());

    // SENSOR HEALTH (Phase 2 – Sensor Delta)
    Serial.printf("\nSENSOR HEALTH\n");
    Serial.printf("  Avg Delta: %.2f °C\n", getAvgDelta());
    Serial.printf("  Max Delta: %.2f °C\n", getMaxDelta());
    Serial.printf("  Invalid reads: %lu\n", getInvalidReads());
    Serial.printf("  Sensor Disconnects: %lu\n", sensorDisconnectCount);

    // HEATER PERFORMANCE (Phase 2 – Duty Cycle سيضاف لاحقاً)
    Serial.printf("\nHEATER PERFORMANCE\n");
    Serial.printf("  Duty Cycle: %.1f %%\n", getDutyCycle());
    Serial.printf("  Avg ON Time: %.1f s\n", getAvgOnTime());
    // Software reliability
    Serial.printf("\nSOFTWARE RELIABILITY\n");
    Serial.printf("  Unexpected Resets: %lu\n", resetUnexpected);
    Serial.printf("  WDT Resets: %lu\n", resetWdt);
    Serial.printf("  Brownout Resets: %lu\n", resetBrownout);

    // Faults (Phase 3)
    Serial.printf("\nFAULT HISTORY\n");
    uint32_t critCount = 0, warnCount = 0, infoCount = 0;
    for (int i = 0; i < FAULT_RING_SIZE; i++)
    {
        if (faultRing[i].level == FAULT_CRITICAL)
            critCount++;
        else if (faultRing[i].level == 1)
            warnCount++;
        else if (faultRing[i].level == 0)
            infoCount++;
    }
        // ACTUATOR ACTIVITY (إضافة جديدة لتكامل المعيار)
    Serial.printf("\nACTUATOR ACTIVITY\n");
    Serial.printf("  Fan Switching: %lu\n", diagSwitchCount[0]);
    Serial.printf("  Turner Ops: %lu\n", diagSwitchCount[1]);
    Serial.printf("  Heater Switching: %lu  Avg ON Time: %.1f s\n",
                  diagSwitchCount[2],
                  (diagOnMs[2] > 0 && diagSwitchCount[2] > 0)
                      ? (diagOnMs[2] / (float)diagSwitchCount[2]) / 1000.0f : 0.0f);
    Serial.printf("  Evaporator Ops: %lu\n", diagSwitchCount[3]);
    Serial.printf("  Critical: %lu  Warnings: %lu  Info: %lu\n", critCount, warnCount, infoCount);

    // Risk assessment (Power and PCB unknown)
    Serial.printf("\nRISK ASSESSMENT\n");
    Serial.printf("  Thermal: LOW (est.)\n");
    Serial.printf("  Sensor: LOW (est.)\n");
    Serial.printf("  Software: LOW (est.)\n");
    Serial.printf("  Actuator: UNKNOWN\n");
    Serial.printf("  Power: UNKNOWN\n");
    Serial.printf("  PCB Thermal: UNKNOWN\n");

    // Overall Health (Phase 3)
    float score = getHealthScore();
    const char *cls = getHealthClass();
    Serial.printf("\nSYSTEM HEALTH: %.1f / 100\n", score);
    Serial.printf("HEALTH CLASS: %s\n", cls);

    if (diagUptimeSec >= 86400UL)
    {
        Serial.printf("CONFIDENCE: HIGH (sufficient data)\n");
    }
    else if (diagUptimeSec >= 3600UL)
    {
        Serial.printf("CONFIDENCE: MEDIUM\n");
    }
    else
    {
        Serial.printf("CONFIDENCE: LOW (insufficient data)\n");
    }
    Serial.printf("DEGRADATION: %s\n", getDegradationStatus());
    if (score >= 85.0f)
    {
        Serial.printf("OVERALL: PASS — CONTINUE OPERATION\n");
    }
    else if (score >= 50.0f)
    {
        Serial.printf("OVERALL: MONITOR — INVESTIGATE SOON\n");
    }
    else
    {
        Serial.printf("OVERALL: FAIL — INTERVENE IMMEDIATELY\n");
    }
    Serial.printf("RECOMMENDATION: %s\n", (score >= 85.0f) ? "CONTINUE OPERATION" : "INVESTIGATE ISSUES");
    Serial.println("========================================================\n");
}

void printHealthSummary()
{
    float score = getHealthScore();
    const char *cls = getHealthClass();
    Serial.printf("HEALTH: %.1f/100 | Class: %s | Uptime: %lu s\n", score, cls, diagUptimeSec);
}

void printDiagStatus()
{
    Serial.printf("DIAGNOSTICS STATUS (Phase 3)\n");
    Serial.printf("  totalSamples: %lu\n", totalSamples);
    Serial.printf("  heaterCycles: %lu\n", heaterCycles);
    Serial.printf("  thermal.n: %lu, mean: %.2f, min: %.2f, max: %.2f\n",
                  thermal.n, thermal.mean, thermal.minT, thermal.maxT);
    Serial.printf("  bandTime: ");
    for (int i = 0; i < 6; i++)
    {
        Serial.printf("%.1f ", bandTime[i]);
    }
    Serial.println();
    Serial.printf("  reset counters: unexpected=%lu, wdt=%lu, brownout=%lu\n",
                  resetUnexpected, resetWdt, resetBrownout);
    Serial.printf("  sensorDisconnects: %lu\n", sensorDisconnectCount);
    Serial.printf("  blobDirty=%d, lastSave=%lu\n", blobDirty, lastSaveTime);    Serial.printf("  faultCount: %lu, faultHead: %u\n", faultCount, faultHead);
}
void printFaultRing() {
    Serial.println("===== FAULT RING (last 16 events) =====");
    uint8_t start = (faultHead >= 16) ? (faultHead - 16) : 0;
    int printed = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t idx = (start + i) % FAULT_RING_SIZE;
        if (faultRing[idx].ts == 0 && faultRing[idx].code == 0) continue; // فارغ
        Serial.printf("#%d: code=0x%02X level=%d ts=%lu sec value=%.2f\n",
            i+1, faultRing[idx].code, faultRing[idx].level,
            faultRing[idx].ts, faultRing[idx].value);
        printed++;
    }
    if (printed == 0) {
        Serial.println("  (No faults recorded)");
    }
    Serial.printf("Total faults in ring: %lu, Head: %u\n", faultCount, faultHead);
}
// ============================================================
// كشف التدهور (Degradation) باستخدام الميل الخطي
// ============================================================
float getDegradationSlope() {
    // dayCountFilled: number of valid daily samples (0..30)
    // While filling: days are stored at indices 0 .. dayCountFilled-1 in time order.
    // When full: dayIdx is the next write slot = oldest sample; chronological
    // order is dayIdx, dayIdx+1, ... (mod 30).
    int n = (int)dayCountFilled;
    if (n < 3) return 0.0f;

    float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    for (int i = 0; i < n; i++) {
        float x = (float)i;
        uint8_t idx;
        if (n < 30) {
            idx = (uint8_t)i;                 // sequential fill from 0
        } else {
            idx = (uint8_t)((dayIdx + i) % 30); // oldest → newest
        }
        float y = (float)dayHealth[idx];
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }
    float denom = (float)n * sumX2 - sumX * sumX;
    if (denom > -1e-6f && denom < 1e-6f) return 0.0f;
    float slope = ((float)n * sumXY - sumX * sumY) / denom;
    return slope; // negative = degradation
}

const char* getDegradationStatus() {
    float slope = getDegradationSlope();
    if (slope < DEGRADATION_THRESHOLD) return "DEGRADATION DETECTED";
    else return "NOT DETECTED";
}