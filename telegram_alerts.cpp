// telegram_alerts.cpp
// Phase 4B – Non-blocking Telegram alerts with multi-code debounce
// Credentials: secrets.h (never commit real values)

#include "telegram_alerts.h"
#include "diagnostics.h"
#include "secrets.h"
#include <Preferences.h>
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <stdio.h>
#include <string.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "diag_io.h"   
#include <time.h>   
#define MAX_LOGS 50



struct ResetStats {
    uint32_t totalCount;
    uint32_t lastCount;
    uint32_t lastReasonCode;
    unsigned long lastBootTime;
    unsigned long lastDay;
    uint32_t dailyCount;
    uint32_t wdtCount, panicCount, brownoutCount, poweronCount, otherCount;
};

WiFiClientSecure secured_client;
UniversalTelegramBot bot(SECRET_TG_BOT_TOKEN, secured_client);
Preferences prefReset;
Preferences prefLog;    
ResetStats resetStats;

DetailedLog logBuffer[MAX_LOGS];
uint32_t logCount = 0;
uint32_t nextLogId = 1;

const char* ntpServer = "time.google.com";
const long  gmtOffset_sec = 7200;   // +2 ساعة (توقيت مصر الصيفي قد يكون 3، نضبط حسب الموسم)
const int   daylightOffset_sec = 0; // 3600 لو الصيفي

time_t nowTime;
struct tm timeinfo;

// ---- المتغيرات العامة المطلوبة للأوامر الجديدة ----
extern unsigned long elapsedSec;
// extern unsigned long TARGET_SEC;
extern unsigned long cycleCount;
extern float currentTemperature;
extern float avgHumidity;
extern bool heaterState;
extern bool turnerState;
extern bool safeMode;
extern bool emergencyMode;
extern bool hasValidTemperature;
extern uint32_t sensorDisconnectCount;
extern DSTemperatureSensor dsSensors[];
extern Actuator actuators[];
extern uint8_t bootCount;
// إعلان دوال من telegram_alerts.cpp
extern void saveLog(const DetailedLog& log);
extern String getTimeString();

// لتخزين آخر سبب لإعادة التشغيل ليتم إرساله عند الطلب
char last_reset_reason[80] = "Unknown";  // M6: هامش UTF-8 عربي

static const char* TG_BOT_TOKEN = SECRET_TG_BOT_TOKEN;
static const char* TG_CHAT_ID   = SECRET_TG_CHAT_ID;
static const unsigned long TG_TIMEOUT_MS = 2500UL;
static const unsigned long DEBOUNCE_MS   = 3600000UL;

#define DEBOUNCE_RING 4
static uint8_t  recentCodes[DEBOUNCE_RING] = {0xFF, 0xFF, 0xFF, 0xFF};
static unsigned long recentTimes[DEBOUNCE_RING] = {0};
static uint8_t  recentIdx = 0;

static bool isDebounced(uint8_t code) {
    unsigned long now = millis();
    for (uint8_t i = 0; i < DEBOUNCE_RING; i++) {
        if (recentCodes[i] == code && (now - recentTimes[i]) < DEBOUNCE_MS)
            return true;
    }
    return false;
}

static void recordSent(uint8_t code) {
    recentCodes[recentIdx] = code;
    recentTimes[recentIdx] = millis();
    recentIdx = (recentIdx + 1) % DEBOUNCE_RING;
}

static bool lastEmergState = false;
static unsigned long lastDailyReportDay = 0;
static char msgBuf[180];

extern bool emergencyMode;
extern unsigned long cycleCount;
extern float rawTemperature;

// ---- متغير لإرسال تقرير الإقلاع التلقائي (مرة واحدة فقط، غير حاجب) ----
static bool bootReportSent = false;

// ---- Strip characters that would break our simple JSON body ----
static void sanitizeTgText(char* s, size_t cap) {
    if (!s) return;
    for (size_t i = 0; s[i] && i < cap; i++) {
        if (s[i] == '"' || s[i] == '\\' || s[i] == '\n' || s[i] == '\r')
            s[i] = ' ';
    }
}

// ---- Single HTTP attempt; used by tgSend with one retry ----
static bool tgSendOnce(const char* text) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ Telegram FAIL: WiFi NOT connected!");
        return false;
    }
    
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage", TG_BOT_TOKEN);
             
    char body[700];  
    snprintf(body, sizeof(body),
             "{\"chat_id\":\"%s\",\"text\":\"%s\"}", TG_CHAT_ID, text);
             
    HTTPClient http;
    http.setTimeout(800);  // C6: مهلة أقصر عند الإقلاع/الشبكة الضعيفة
    esp_task_wdt_reset();   
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    esp_task_wdt_reset();   
    
    int code = http.POST(body);
    http.end();
    esp_task_wdt_reset();   
    
    Serial.printf("📡 Telegram HTTP Code: %d\n", code);
    return (code >= 200 && code < 300);
}

bool tgSend(const char* text) {
    if (tgSendOnce(text)) return true;
    {
        unsigned long t0 = millis();
        while ((millis() - t0) < 100UL) {
            esp_task_wdt_reset();
            delay(1);
        }
    }
    return tgSendOnce(text);
}

// ============================================================
// إحصائيات أسباب إعادة التشغيل (Reset Statistics)
// ============================================================
bool syncTime() {
    // setupWatchdog() already registered loopTask — reset is safe here.
    // Keep short retries: offline boards must not stall setup.
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    int retries = 0;
    while (retries < 3 && !getLocalTime(&timeinfo)) {
        delay(150);
        retries++;
        esp_task_wdt_reset();
    }
    return (retries < 6);
}

// دالة للحصول على String بالتنسيق "YYYY-MM-DD HH:MM:SS"
String getTimeString() {
    getLocalTime(&timeinfo);
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
}


void initLogStorage() {
    prefLog.begin("logs", false);
    logCount = prefLog.getULong("logCount", 0);
    nextLogId = prefLog.getULong("nextId", 1);
    // قراءة السجلات المحفوظة
    if (logCount > MAX_LOGS) logCount = MAX_LOGS;
    for (uint32_t i = 0; i < logCount; i++) {
        char key[20];
        snprintf(key, sizeof(key), "log%lu", i);
        prefLog.getBytes(key, (uint8_t*)&logBuffer[i], sizeof(DetailedLog));
    }
}

void saveLog(const DetailedLog& log) {
    uint32_t idx = logCount % MAX_LOGS;
    logBuffer[idx] = log;
    logCount++;
    if (logCount > MAX_LOGS) logCount = MAX_LOGS; // نحدها
    prefLog.putULong("logCount", logCount);
    prefLog.putULong("nextId", nextLogId);
    char key[20];
    snprintf(key, sizeof(key), "log%lu", idx);
    prefLog.putBytes(key, (uint8_t*)&log, sizeof(DetailedLog));
    nextLogId++;
    prefLog.putULong("nextId", nextLogId);
}


void initResetStats() {
    prefReset.begin("reset_stats", false);
    resetStats.totalCount = prefReset.getULong("total", 0);
    resetStats.lastCount = prefReset.getULong("last_count", 0);
    resetStats.lastReasonCode = prefReset.getULong("last_reason", 0);
    resetStats.lastBootTime = prefReset.getULong("last_boot", 0);
    resetStats.lastDay = prefReset.getULong("last_day", 0);
    resetStats.dailyCount = prefReset.getULong("daily", 0);
    resetStats.wdtCount = prefReset.getULong("wdt", 0);
    resetStats.panicCount = prefReset.getULong("panic", 0);
    resetStats.brownoutCount = prefReset.getULong("brown", 0);
    resetStats.poweronCount = prefReset.getULong("power", 0);
    resetStats.otherCount = prefReset.getULong("other", 0);
}

void saveResetStats() {
    prefReset.putULong("total", resetStats.totalCount);
    prefReset.putULong("last_count", resetStats.lastCount);
    prefReset.putULong("last_reason", resetStats.lastReasonCode);
    prefReset.putULong("last_boot", resetStats.lastBootTime);
    prefReset.putULong("last_day", resetStats.lastDay);
    prefReset.putULong("daily", resetStats.dailyCount);
    prefReset.putULong("wdt", resetStats.wdtCount);
    prefReset.putULong("panic", resetStats.panicCount);
    prefReset.putULong("brown", resetStats.brownoutCount);
    prefReset.putULong("power", resetStats.poweronCount);
    prefReset.putULong("other", resetStats.otherCount);
}

void updateResetStats() {
    initResetStats();
    
    esp_reset_reason_t reason = esp_reset_reason();
    uint32_t code = 0;
    
    switch (reason) {
        case ESP_RST_WDT: code = 1; resetStats.wdtCount++; break;
        case ESP_RST_INT_WDT: code = 2; resetStats.wdtCount++; break;
        case ESP_RST_PANIC: code = 3; resetStats.panicCount++; break;
        case ESP_RST_BROWNOUT: code = 4; resetStats.brownoutCount++; break;
        case ESP_RST_POWERON: code = 5; resetStats.poweronCount++; break;
        case ESP_RST_SW: code = 7; resetStats.otherCount++; break;
        case ESP_RST_TASK_WDT: code = 8; resetStats.wdtCount++; break;
        case ESP_RST_PWR_GLITCH: code = 9; resetStats.brownoutCount++; break;
        case ESP_RST_USB: code = 10; resetStats.otherCount++; break;
        case ESP_RST_DEEPSLEEP: code = 11; resetStats.otherCount++; break;
        case ESP_RST_EXT: code = 12; resetStats.otherCount++; break;
        case ESP_RST_CPU_LOCKUP: code = 13; resetStats.otherCount++; break;
        case ESP_RST_SDIO: code = 14; resetStats.otherCount++; break;
        case ESP_RST_JTAG: code = 15; resetStats.otherCount++; break;
        case ESP_RST_EFUSE: code = 16; resetStats.otherCount++; break;
        default: code = 6; resetStats.otherCount++; break;
    }       
    
    resetStats.totalCount++;
    resetStats.lastReasonCode = code;
    resetStats.lastCount++;
    
    unsigned long currentDay = millis() / 86400000UL;
    if (currentDay != resetStats.lastDay) {
        resetStats.lastDay = currentDay;
        resetStats.dailyCount = 1;
    } else {
        resetStats.dailyCount++;
    }
    
    resetStats.lastBootTime = millis();
    saveResetStats();
}

// ============================================================
// إرسال تقرير الإقلاع التلقائي (مرة واحدة، غير حاجب)
// ============================================================
static void sendBootResetReport() {
    // C6: غير حاجب — يُعاد لاحقاً عند فشل الشبكة
    if (bootReportSent) return;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("📡 Boot report pending: WiFi not connected yet.");
        return;
    }
    char report[768];
    fillDetailedResetReport(report, sizeof(report));
    if (tgSend(report)) {
        bootReportSent = true;
        Serial.println("📲 Boot reset report sent automatically.");
    } else {
        Serial.println("❌ Failed to send boot reset report.");
    }
}

// ============================================================
// دوال التيليجرام العامة
// ============================================================
void telegram_init() {
    initLogStorage();
    for (uint8_t i = 0; i < DEBOUNCE_RING; i++) {
        recentCodes[i] = 0xFF;
        recentTimes[i] = 0;
    }
    secured_client.setInsecure();
    secured_client.setTimeout(2);  // C3
    recentIdx = 0;
    lastEmergState = false;
    lastDailyReportDay = 0;
    
    updateResetStats();
    
    // محاولة إرسال تقرير الإقلاع التلقائي (مرة واحدة، غير حاجبة)
    sendBootResetReport();
    
    Serial.println("📲 Telegram alerts ready (multi-code debounce)");
}

void telegram_send_fault(uint8_t code, uint8_t level, float value) {
    if (isDebounced(code)) return;
    unsigned long now = millis();
    unsigned long uptimeSec = now / 1000UL;
    unsigned long days  = uptimeSec / 86400UL;
    unsigned long hours = (uptimeSec % 86400UL) / 3600UL;
    snprintf(msgBuf, sizeof(msgBuf),
             "INCUBATOR CRITICAL! Code: 0x%02X, Value: %.1f, Uptime: %lud %luh",
             code, value, days, hours);
    sanitizeTgText(msgBuf, sizeof(msgBuf));
    if (tgSend(msgBuf)) {
        recordSent(code);
        Serial.println("📲 Telegram alert sent");
    }
}

void telegram_send_daily_report() {
    float score = getHealthScore();
    const char* cls = getHealthClass();
    snprintf(msgBuf, sizeof(msgBuf),
             "Daily Report Health: %.1f (%s) Cycles: %lu",
             score, cls, cycleCount);
    sanitizeTgText(msgBuf, sizeof(msgBuf));
    if (tgSend(msgBuf))
        Serial.println("📲 Daily Telegram report sent");
}

// ============================================================
// وضع المراقبة المستمرة (كل دقيقة)
// ============================================================
bool monitorEnabled = false;  // C5: افتراضي OFF — التفعيل بـ MONITORON فقط
unsigned long lastMonitorMs = 0;

void telegram_set_monitoring(bool enabled) {
    monitorEnabled = enabled;
    Serial.printf("📊 Periodic Telegram Monitoring: %s\n", enabled ? "ON" : "OFF");
}

void telegram_send_periodic_report() {
    extern float currentTemperature;
    extern bool heaterState;
    extern bool turnerState;
    extern bool emergencyMode;
    extern bool safeMode;
    extern unsigned long cycleCount;
    extern float avgHumidity;
    extern DSTemperatureSensor dsSensors[];

    char buffer[512];
    snprintf(buffer, sizeof(buffer),
        "📊 *تقرير الحاضنة*\n"
        "=====================\n"
        "🌡️ الحرارة:\n"
        "   T1: %.2f°C | T2: %.2f°C\n"
        "   المتوسط: %.1f°C\n"
        "💧 الرطوبة: %.1f%%\n"
        "🔥 السخان: %s\n"
        "🔄 المقلب: %s\n"
        "⏱️ وقت التشغيل: %lu ثانية\n"
        "📈 عدد الدورات: %lu\n"
        "🩺 الصحة: %.1f/100\n"
        "⚠️ الحالة: %s",
        dsSensors[0].temperature,
        dsSensors[1].temperature,
        currentTemperature,
        avgHumidity,
        heaterState ? "ON" : "OFF",
        turnerState ? "ON" : "OFF",
        millis() / 1000,
        cycleCount,
        getHealthScore(),
        safeMode ? "SAFE MODE" : (emergencyMode ? "EMERGENCY" : "NORMAL")
    );

    if (tgSend(buffer)) {
        Serial.println("📊 Periodic Telegram report sent successfully!");
    } else {
        Serial.println("❌ Periodic Telegram report FAILED to send.");
    }
}

void telegram_monitor_loop() {
    if (monitorEnabled) {
        if (millis() - lastMonitorMs >= 300000UL) {  // C5: 5 دقائق بدل 60 ثانية
            lastMonitorMs = millis();
            telegram_send_periodic_report();
        }
    }
}

void telegram_loop() {
    if (emergencyMode != lastEmergState) {
        if (emergencyMode)
            telegram_send_fault(FAULT_EMERGENCY_TEMP, FAULT_EMERGENCY, rawTemperature);
        lastEmergState = emergencyMode;
    }
    unsigned long day = millis() / 86400000UL;
    if (day != lastDailyReportDay && day > 0) {
        lastDailyReportDay = day;
        telegram_send_daily_report();
    }
}
void resetTelegramBot() {
    // W6 + C3: إعادة تهيئة كاملة + مهلة عميل
    secured_client.setInsecure();
    secured_client.setTimeout(2);  // ثوانٍ — يقلل احتمال حجب TLS الطويل
    bootReportSent = false;
    lastEmergState = false;
    lastDailyReportDay = 0;
    memset(recentCodes, 0xFF, sizeof(recentCodes));
    memset(recentTimes, 0, sizeof(recentTimes));
    recentIdx = 0;
    Serial.println("🔄 Telegram bot reinitialized after WiFi recovery");
}
// ============================================================
// دوال أسباب إعادة التشغيل
// ============================================================
// ============================================================
// بناء تقرير أسباب إعادة التشغيل بتنسيق جميل (بديل fillDetailedResetReport)
// ============================================================
void fillDetailedResetReport(char* out, size_t n) {
    if (!out || n == 0) return;
    
    const char* reasonStr = "غير معروف";
    switch (resetStats.lastReasonCode) {
        case 1: reasonStr = "**Watchdog (توقف النظام)**"; break;
        case 2: reasonStr = "**Interrupt WDT (تعليق المقاطعة)**"; break;
        case 3: reasonStr = "**Panic (انهيار برمجي)**"; break;
        case 4: reasonStr = "**Brownout (انقطاع كهرباء مفاجئ)**"; break;
        case 5: reasonStr = "**إغلاق عادي (فصل كهرباء يدوي)**"; break;
        case 7: reasonStr = "**إعادة تشغيل برمجي (Software Reset)**"; break;
        case 8: reasonStr = "**Watchdog مهام (Task WDT)**"; break;
        case 9: reasonStr = "**تقلبات في مصدر الطاقة**"; break;
        case 10: reasonStr = "**إعادة تشغيل عبر USB**"; break;
        case 11: reasonStr = "**خروج من وضع النوم العميق**"; break;
        case 12: reasonStr = "**زر إعادة التشغيل الخارجي (EXT)**"; break;
        case 13: reasonStr = "**تجميد المعالج (CPU Lockup)**"; break;
        case 14: reasonStr = "**إعادة تشغيل عبر SDIO**"; break;
        case 15: reasonStr = "**إعادة تشغيل عبر JTAG**"; break;
        case 16: reasonStr = "**إعادة تشغيل بسبب EFUSE**"; break;
        default: break;
    }
    
    unsigned long uptimeSec = millis() / 1000UL;
    unsigned long mins = uptimeSec / 60;
    unsigned long secs = uptimeSec % 60;
    
    snprintf(out, n,
        "🔌 **تقرير سبب إعادة التشغيل المفصل**\n"
        "=============================\n"
        "● السبب الحالي: %s\n"
        "● وقت التشغيل الحالي: %lu دقيقة و %lu ثانية\n"
        "● حدث هذا السبب اليوم (آخر 24 ساعة): **%lu مرة**\n\n"
        "📊 **إحصائيات جميع الأسباب:**\n"
        "  • Watchdog / Interrupt WDT: %lu مرة\n"
        "  • Panic (انهيار برمجي): %lu مرة\n"
        "  • Brownout (انقطاع كهرباء): %lu مرة\n"
        "  • إغلاق عادي (فصل كهرباء يدوي): %lu مرة\n"
        "  • غير معروف: %lu مرة\n\n"
        "**إجمالي إعادة التشغيل الكلي:** %lu مرة\n"
        "**إجمالي المرات في آخر 24 ساعة:** %lu مرة",
        reasonStr,
        mins, secs,
        (unsigned long)resetStats.dailyCount,
        (unsigned long)resetStats.wdtCount,
        (unsigned long)resetStats.panicCount,
        (unsigned long)resetStats.brownoutCount,
        (unsigned long)resetStats.poweronCount,
        (unsigned long)resetStats.otherCount,
        (unsigned long)resetStats.totalCount,
        (unsigned long)resetStats.dailyCount);
}

// ============================================================
// بناء تقرير الأعطال بتنسيق جميل (بديل fillFaultsReport)
// ============================================================
void fillFaultsReport(char* out, size_t n) {
    if (!out || n == 0) return;
    
    size_t used = 0;
    int w = snprintf(out, n,
        "⚠️ **آخر 10 أعطال مسجلة:**\n"
        "==========================\n");
    if (w > 0) used = (size_t)w;
    
    uint8_t start = (faultHead >= 10) ? (faultHead - 10) : 0;
    int printed = 0;
    
    for (int i = 0; i < 10 && used + 64 < n; i++) {
        uint8_t idx = (start + i) % FAULT_RING_SIZE;
        if (faultRing[idx].ts == 0 && faultRing[idx].code == 0) continue;
        
        // تحويل الكود إلى وصف عربي
        const char* codeDesc = "عطل غير معروف";
        switch (faultRing[idx].code) {
            case 0x01: codeDesc = "انفصال حساس DS18B20"; break;
            case 0x02: codeDesc = "فرق كبير بين الحساسين"; break;
            case 0x05: codeDesc = "تجاوز الحرارة (>0.4°C)"; break;
            case 0x08: codeDesc = "طوارئ حرارة (≥39.8°C)"; break;
            case 0x10: codeDesc = "دخول Safe Mode"; break;
            case 0x20: codeDesc = "فشل النموذج التنبئي"; break;
            case 0x21: codeDesc = "تدهور الأداء"; break;
            default: break;
        }
        
        unsigned long mins = faultRing[idx].ts / 60;
        unsigned long secs = faultRing[idx].ts % 60;
        
        w = snprintf(out + used, n - used,
                     "• **%s** | القيمة: %.1f | منذ %luد %luث\n",
                     codeDesc, faultRing[idx].value, mins, secs);
        if (w > 0) used += (size_t)w;
        printed++;
    }
    
    if (printed == 0 && used + 30 < n) {
        snprintf(out + used, n - used, "✅ **لا توجد أعطال مسجلة.**");
    } else if (used + 30 < n) {
        snprintf(out + used, n - used, "\n_الأحدث في الأسفل._");
    }
}
// ============================================================
// دالة تحديث سبب إعادة التشغيل الأخير (تُستدعى في setup)
// ============================================================
void update_last_reset_reason() {
    esp_reset_reason_t reason = esp_reset_reason();
    const char* msg = "غير معروف";
    switch (reason) {
        case ESP_RST_POWERON:    msg = "إغلاق عادي (فصل كهرباء)"; break;
        case ESP_RST_WDT:        msg = "Watchdog (توقف النظام)"; break;
        case ESP_RST_BROWNOUT:   msg = "انقطاع كهرباء مفاجئ"; break;
        case ESP_RST_PANIC:      msg = "انهيار برمجي (Panic)"; break;
        case ESP_RST_INT_WDT:    msg = "Interrupt WDT"; break;
        case ESP_RST_SW:         msg = "إعادة تشغيل برمجي (Software Reset)"; break;
        case ESP_RST_TASK_WDT:   msg = "Watchdog مهام (Task WDT)"; break;
        case ESP_RST_PWR_GLITCH: msg = "تقلبات في مصدر الطاقة"; break;
        case ESP_RST_USB:        msg = "إعادة تشغيل عبر USB"; break;
        case ESP_RST_DEEPSLEEP:  msg = "خروج من وضع النوم العميق"; break;
        case ESP_RST_EXT:        msg = "زر إعادة التشغيل الخارجي"; break;
        case ESP_RST_CPU_LOCKUP: msg = "تجميد المعالج (CPU Lockup)"; break;
        case ESP_RST_SDIO:       msg = "إعادة تشغيل عبر SDIO"; break;
        case ESP_RST_JTAG:       msg = "إعادة تشغيل عبر JTAG"; break;
        case ESP_RST_EFUSE:      msg = "إعادة تشغيل بسبب EFUSE"; break;
        default:                 msg = "غير معروف"; break;
    }
    snprintf(last_reset_reason, sizeof(last_reset_reason), "%s", msg);
}
String getDetailedResetReport() {
    // غلاف توافق — يُفضّل fillDetailedResetReport في المسارات الجديدة
    static char buf[768];
    fillDetailedResetReport(buf, sizeof(buf));
    return String(buf);
}

// ============================================================

String getFaultsReport() {
    static char buf[512];
    fillFaultsReport(buf, sizeof(buf));
    return String(buf);
}

// ============================================================
// دالة استقبال الأوامر من تيليجرام (غير حاجبة)
// ============================================================
// ============================================================
// دالة استقبال الأوامر من تيليجرام (غير حاجبة)
// ============================================================
void checkTelegramCommands() {
    // C1/C2/C3: حارس WiFi + سقف رسائل + مهلة drain — لا تجميد loop
    if (WiFi.status() != WL_CONNECTED) return;

    esp_task_wdt_reset();
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    esp_task_wdt_reset();

    const int MAX_MESSAGES_PER_CALL = 2;
    const unsigned long MAX_DRAIN_MS = 800UL;
    unsigned long t0 = millis();
    int processed = 0;
    String reply;

    while (numNewMessages && processed < MAX_MESSAGES_PER_CALL
           && (millis() - t0) < MAX_DRAIN_MS) {
        esp_task_wdt_reset();
        for (int i = 0; i < numNewMessages && processed < MAX_MESSAGES_PER_CALL; i++) {
            if (bot.messages[i].chat_id != SECRET_TG_CHAT_ID) {
                processed++;
                continue;
            }
            String text = bot.messages[i].text;
            processed++;

            // ---- الأوامر القديمة ----
            if (text == "/resetreason" || text == "/why" || text == "/stats") {
                reply = getDetailedResetReport();
                bot.sendMessage(SECRET_TG_CHAT_ID, reply, "");
            }
            else if (text == "/faults") {
                reply = getFaultsReport();
                bot.sendMessage(SECRET_TG_CHAT_ID, reply, "");
            }
            else if (text == "/status") {
                bot.sendMessage(SECRET_TG_CHAT_ID, "🌡️ الحاضنة تعمل بشكل طبيعي.", "");
            }
            
            // ============================================================
            // ⭐ الأوامر الجديدة (بشكل محسّن)
            // ============================================================
            
            // ---- 1. عرض درجات الحرارة والرطوبة ----
            else if (text == "/temps") {
                String msg = "🌡️ **درجات الحرارة والرطوبة:**\n";
                msg += "========================\n";
                msg += "T1: " + String(dsSensors[0].temperature, 2) + " °C\n";
                msg += "T2: " + String(dsSensors[1].temperature, 2) + " °C\n";
                msg += "المتوسط: " + String(currentTemperature, 1) + " °C\n";
                msg += "الرطوبة: " + String(avgHumidity, 1) + " %\n";
                msg += "الحالة: " + String(hasValidTemperature ? "✅ صالحة" : "❌ غير صالحة");
                bot.sendMessage(SECRET_TG_CHAT_ID, msg, "");
            }
            
            // ---- 2. عرض حالة المشغلات ----
            else if (text == "/actuators") {
                String msg = "⚡ **حالة المشغلات:**\n";
                msg += "=====================\n";
                msg += "🔥 السخان: " + String(heaterState ? "ON" : "OFF") + "\n";
                msg += "🌀 المروحة: " + String(actuators[0].state ? "ON" : "OFF") + "\n";
                msg += "🔄 المقلب: " + String(turnerState ? "ON" : "OFF") + "\n";
                msg += "💧 المرطب: " + String(actuators[3].state ? "ON" : "OFF") + "\n";
                msg += "🛡️ الوضع الآمن: " + String(safeMode ? "⚠️ مفعل" : "✅ غير مفعل") + "\n";
                msg += "🚨 الطوارئ: " + String(emergencyMode ? "⚠️ مفعل" : "✅ غير مفعل");
                bot.sendMessage(SECRET_TG_CHAT_ID, msg, "");
            }
            
            // ---- 3. عرض معلومات الدورة ----
            else if (text == "/cycle") {
                unsigned long remaining = (TARGET_SEC > elapsedSec) ? (TARGET_SEC - elapsedSec) : 0;
                unsigned long hours = remaining / 3600;
                unsigned long mins = (remaining % 3600) / 60;
                unsigned long secs = remaining % 60;
                
                unsigned long totalSec = millis() / 1000;
                unsigned long totalHours = totalSec / 3600;
                unsigned long totalMins = (totalSec % 3600) / 60;
                
                String msg = "⏱️ **معلومات الدورة:**\n";
                msg += "=====================\n";
                msg += "رقم الدورة: " + String(cycleCount) + "\n";
                msg += "الوقت المتبقي: " + String(hours) + "س " + String(mins) + "د " + String(secs) + "ث\n";
                msg += "وقت التشغيل الكلي: " + String(totalHours) + "س " + String(totalMins) + "د\n";
                msg += "التقدم: " + String((elapsedSec * 100) / TARGET_SEC) + "%";
                bot.sendMessage(SECRET_TG_CHAT_ID, msg, "");
            }
            
            // ---- 4. عرض تقرير صحي مختصر ----
            else if (text == "/health") {
                float score = getHealthScore();
                const char* cls = getHealthClass();
                String msg = "🩺 **تقرير الصحة المختصر:**\n";
                msg += "========================\n";
                msg += "الصحة: " + String(score, 1) + "/100\n";
                msg += "التصنيف: " + String(cls) + "\n";
                msg += "تجاوز الحرارة (P95): " + String(getP95Overshoot(), 2) + " °C\n";
                msg += "دورة السخان: " + String(getDutyCycle(), 1) + "%\n";
                msg += "فرق الحساسات (متوسط): " + String(getAvgDelta(), 2) + " °C\n";
                msg += "قراءات غير صالحة: " + String(getInvalidReads()) + "\n";
                msg += "انفصال الحساسات: " + String(sensorDisconnectCount) + "\n";
                msg += "التدهور: " + String(getDegradationStatus());
                bot.sendMessage(SECRET_TG_CHAT_ID, msg, "");
            }
            
            // ---- 5. عرض معلومات النظام ----
            else if (text == "/sysinfo") {
                unsigned long uptimeSec = millis() / 1000;
                unsigned long days = uptimeSec / 86400;
                unsigned long hours = (uptimeSec % 86400) / 3600;
                unsigned long mins = (uptimeSec % 3600) / 60;
                
                String msg = "🖥️ **معلومات النظام:**\n";
                msg += "=====================\n";
                msg += "وقت التشغيل: " + String(days) + "يوم " + String(hours) + "س " + String(mins) + "د\n";
                msg += "الذاكرة الحرة: " + String((unsigned)ESP.getFreeHeap() / 1024) + " KB\n";
                msg += "أدنى ذاكرة: " + String((unsigned)ESP.getMinFreeHeap() / 1024) + " KB\n";
                msg += "رقم الإقلاع: " + String(bootCount) + "\n";
                msg += "آخر سبب إقلاع: " + String(last_reset_reason) + "\n";
                msg += "WiFi: " + String(wifiConnected ? "✅ متصل" : "❌ غير متصل") + "\n";
                msg += "الموديل: " + String(ESP.getChipModel()) + " | التردد: " + String(ESP.getCpuFreqMHz()) + " MHz";
                bot.sendMessage(SECRET_TG_CHAT_ID, msg, "");
            }
            
            // ---- 6. تقرير تشخيصي شامل ----
            else if (text == "/diagnose") {
                unsigned long uptimeSec = millis() / 1000UL;
                unsigned long days = uptimeSec / 86400;
                unsigned long hours = (uptimeSec % 86400) / 3600;
                unsigned long mins = (uptimeSec % 3600) / 60;
                
                String report = "🔍 **تقرير تشخيصي شامل**\n";
                report += "==========================\n\n";
                report += "🕒 وقت التشغيل: " + String(days) + "يوم " + String(hours) + "س " + String(mins) + "د\n";
                report += "🩺 الصحة: " + String(getHealthScore(), 1) + "/100\n";
                report += "🌡️ الحرارة: " + String(currentTemperature, 1) + "°C\n";
                report += "💧 الرطوبة: " + String(avgHumidity, 1) + "%\n";
                report += "🔥 السخان: " + String(heaterState ? "ON" : "OFF") + "\n";
                report += "🔄 المقلب: " + String(turnerState ? "ON" : "OFF") + "\n";
                report += "🛡️ Safe Mode: " + String(safeMode ? "⚠️ مفعل" : "✅ غير مفعل") + "\n";
                report += "🚨 Emergency: " + String(emergencyMode ? "⚠️ مفعل" : "✅ غير مفعل") + "\n\n";
                report += "🔌 **إحصائيات إعادة التشغيل:**\n";
                report += "• إجمالي: " + String(resetStats.totalCount) + "\n";
                report += "• اليوم (آخر 24 ساعة): " + String(resetStats.dailyCount) + "\n";
                report += "• WDT: " + String(resetStats.wdtCount) + "\n";
                report += "• Panic: " + String(resetStats.panicCount) + "\n";
                report += "• Brownout: " + String(resetStats.brownoutCount);
                
                bot.sendMessage(SECRET_TG_CHAT_ID, report, "");
            }
            else if (text == "/logtoday") {
                // إرسال سجلات اليوم فقط
                String response = "📋 **سجلات اليوم**\n";
                int count = 0;
                for (uint32_t i = 0; i < logCount; i++) {
                    uint32_t idx = (logCount - 1 - i) % MAX_LOGS;
                    DetailedLog& l = logBuffer[idx];
                    if (String(l.timestamp).substring(0, 10) == getTimeString().substring(0, 10)) {
                        response += String(l.timestamp) + " | " + l.reason;
                        if (l.type == LOG_TYPE_FAULT) response += " | val=" + String(l.value);
                        response += "\n";
                        count++;
                        if (count >= 20) break; // حد أقصى 20 رسالة
                    }
                }
                if (count == 0) response += "✅ لا توجد أحداث اليوم.";
                bot.sendMessage(SECRET_TG_CHAT_ID, response, "");
            }
            else if (text == "/logall") {
                // إرسال آخر 10 سجلات (أو كلها حسب الحجم)
                String response = "📋 **آخر 10 أحداث**\n";
                int count = 0;
                for (uint32_t i = 0; i < logCount && count < 10; i++) {
                    uint32_t idx = (logCount - 1 - i) % MAX_LOGS;
                    DetailedLog& l = logBuffer[idx];
                    response += String(l.timestamp) + " | " + l.reason;
                    if (l.type == LOG_TYPE_FAULT) response += " | val=" + String(l.value);
                    response += "\n";
                    count++;
                }
                if (count == 0) response += "✅ لا توجد سجلات.";
                bot.sendMessage(SECRET_TG_CHAT_ID, response, "");
            }
            
            // ---- 7. أمر تشغيل/إيقاف التقارير التلقائية (جديد) ----
            else if (text == "/monitor") {
                telegram_set_monitoring(!monitorEnabled);
                String msg = "📊 **التقارير التلقائية:** ";
                msg += monitorEnabled ? "✅ **مفعلة** (كل 5 دقائق)" : "⏹️ **معطلة**";
                bot.sendMessage(SECRET_TG_CHAT_ID, msg, "");
            }
            
            // ---- 8. قائمة المساعدة المحدثة ----
            else if (text == "/help") {
                String help = "📋 **الأوامر المتاحة:**\n";
                help += "========================\n";
                help += "/temps - درجات الحرارة والرطوبة\n";
                help += "/actuators - حالة المشغلات\n";
                help += "/cycle - معلومات الدورة\n";
                help += "/health - تقرير صحي مختصر\n";
                help += "/sysinfo - معلومات النظام\n";
                help += "/diagnose - تقرير تشخيصي شامل\n";
                help += "/resetreason - أسباب إعادة التشغيل\n";
                help += "/faults - آخر الأعطال المسجلة\n";
                help += "/monitor - تشغيل/إيقاف التقارير التلقائية\n";
                help += "/status - الحالة العامة\n";
                help += "/help - هذه القائمة";
                bot.sendMessage(SECRET_TG_CHAT_ID, help, "");
            }
        }
        esp_task_wdt_reset();
        if (processed >= MAX_MESSAGES_PER_CALL) break;
        if ((millis() - t0) >= MAX_DRAIN_MS) break;
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        esp_task_wdt_reset();
    }
}