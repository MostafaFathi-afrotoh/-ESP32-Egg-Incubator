#pragma once

#include <stdint.h>
#include <WString.h>   // <-- ضروري لتعريف String

// ============================================================
// الثوابت العامة
// ============================================================
#define TARGET_SEC (24UL * 60UL * 60UL)   // 86400 ثانية
#define MAX_LOGS 50

// ============================================================
// تعريف LogType و DetailedLog (مرة واحدة فقط)
// ============================================================
typedef enum {
    LOG_TYPE_RESET = 0,
    LOG_TYPE_FAULT = 1
} LogType;

struct DetailedLog {
    uint32_t id;
    LogType type;
    char reason[32];
    char timestamp[30];
    uint32_t uptimeSec;
    float value;
    uint8_t code;
};

// ---- دوال سجل التشخيص المفصل ----
void initLogStorage();
void saveLog(const DetailedLog& log);

// ---- المتغيرات العامة (externa) ----
extern DetailedLog logBuffer[MAX_LOGS];
extern uint32_t logCount;
extern uint32_t nextLogId;

// ---- دوال الوقت (NTP) ----
bool syncTime();
String getTimeString();    // <-- الآن String معروفة

// ---- سبب إعادة التشغيل ----
extern char last_reset_reason[80];

// ============================================================
// تعريفات الهياكل المشتركة الأخرى (Actuator, HumiditySensor, DSTemperatureSensor)
// ============================================================
class DHT;   // forward declaration

struct Actuator {
    uint8_t pin;
    bool state;
    bool safeState;
    const char* name;
    bool isHeater;
};

struct HumiditySensor {
    float humidity;
    float temperature;
    bool valid;
    int failCount;
    bool bypassed;
    unsigned long bypassStartTime;
    DHT* dht;
};

struct DSTemperatureSensor {
    float temperature;
    bool valid;
    float lastValidTemp;
    int failCount;
};

// ============================================================
// تعريف سجل الخطأ (FaultRec)
// ============================================================
struct FaultRec {
    uint8_t code;
    uint8_t level;
    uint32_t ts;
    float value;
};

// جعل مصفوفة الأعطال ومؤشرها متاحة للاستخدام خارج diagnostics.cpp
extern FaultRec faultRing[];
extern uint8_t faultHead;
#define FAULT_RING_SIZE 32

// ============================================================
// تعريف مستويات الأخطاء
// ============================================================
#define FAULT_INFO 0
#define FAULT_WARNING 1
#define FAULT_CRITICAL 2
#define FAULT_EMERGENCY 3

// ============================================================
// أوزان Health-score (قابلة للتعديل من مكان واحد)
// ============================================================
#define PENALTY_MEAN_DEV_HIGH          15.0f
#define PENALTY_MEAN_DEV_MED            5.0f
#define PENALTY_SPREAD_HIGH            20.0f
#define PENALTY_SPREAD_MED             10.0f
#define PENALTY_IDEAL_BAND_LOW         25.0f
#define PENALTY_IDEAL_BAND_MED         10.0f
#define PENALTY_OVERSHOOT_P95_HIGH     30.0f
#define PENALTY_OVERSHOOT_P95_MED      15.0f
#define PENALTY_OVERSHOOT_AVG          10.0f
#define PENALTY_DELTA_MAX_HIGH         30.0f
#define PENALTY_DELTA_MAX_MED          15.0f
#define PENALTY_DELTA_AVG               5.0f
#define PENALTY_SENSOR_DISCONNECT_CAP  20.0f
#define PENALTY_SENSOR_DISCONNECT_STEP  2.0f
#define PENALTY_HEATER_CYCLES           5.0f
#define PENALTY_RESET_UNEXPECTED       15.0f
#define PENALTY_RESET_WDT              30.0f
#define PENALTY_RESET_BROWNOUT         20.0f
#define PENALTY_HEATER_HIGH_SWITCH     20.0f
#define PENALTY_HEATER_MED_SWITCH      10.0f
#define PENALTY_FAN_LOW_SWITCH         15.0f
#define PENALTY_HEATER_HIGH_DUTY       15.0f
#define PENALTY_TEMP_OUT_OF_BAND       PENALTY_IDEAL_BAND_LOW
#define PENALTY_SENSOR_FAIL            PENALTY_SENSOR_DISCONNECT_CAP

#define DEGRADATION_THRESHOLD         (-0.5f)

// ============================================================
// أكواد الأعطال
// ============================================================
#define FAULT_OVERSHOOT_HIGH 0x05
#define FAULT_EMERGENCY_TEMP 0x08
#define FAULT_SENSOR_DISCONNECT 0x01
#define FAULT_SAFE_MODE_ENTER 0x10
#define FAULT_SENSOR_DELTA_HIGH 0x02
#define FAULT_WDT_RESET 0x0B
#define FAULT_BROWNOUT_RESET 0x0C

extern uint32_t sensorDisconnectCount;

// ============================================================
// تواقيع الدوال العامة (API)
// ============================================================
void diag_init();
void diag_sample();
void diag_periodic();

void printFullReport();
void printHealthSummary();
void printDiagStatus();
void printFaultRing();

float getAvgOvershoot();
float getMaxOvershoot();
float getP95Overshoot();
float getAvgDelta();
float getMaxDelta();

void addFault(uint8_t code, uint8_t level, float value);

float getHealthScore();
const char* getHealthClass();

float getP95Temp();
uint32_t getInvalidReads();
float getDutyCycle();
float getAvgOnTime();

void clearAllDiagnostics();

void diag_actuator_event(int idx);
float getDegradationSlope();
const char* getDegradationStatus();

void  diag_cycleReset();
float getCycleDutyCycle();
float getCycleAvgOvershoot();

// ---- إعلان المتغيرات الخارجية (من Memory4.ino) ----
extern unsigned long cycleCount;
extern Actuator actuators[];
extern HumiditySensor humiditySensors[];

extern float avgHumidity;
extern uint32_t humidityFailCount;
extern uint32_t humiditySamples;