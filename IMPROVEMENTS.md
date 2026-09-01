# تحسينات v2.0.1 (آمنة – لا تمس منطق التحكم)

## ما تم إضافته

### Memory10_2.ino
- `FIRMWARE_VERSION "2.0.1"` + `FIRMWARE_NAME`
- طباعة الإصدار عند الإقلاع + حجم الـ Heap الحر
- أوامر Serial جديدة:
  - `HELP` / `?` — قائمة كاملة بالأوامر
  - `HEAP` — `ESP.getFreeHeap()` و `getMinFreeHeap()`
  - `VERSION` — الإصدار + نموذج الشريحة + تردد المعالج
- رسالة الأمر المجهول تشير إلى `HELP`

### telegram_alerts.cpp
- `tgSendOnce` + إعادة محاولة واحدة بعد ~100 ms عند فشل الإرسال الأول
- `esp_task_wdt_reset()` قبل/بعد HTTP

## ما لم يُغيَّر (عمداً)
- عتبات الحرارة / Hysteresis / Emergency / Safe
- ترتيب المشغلات (0=Fan … 3=Evaporator)
- منطق السخان والمقلب والمروحة والمرطب
- Dual-blob و Health Score و Predictive و Cycle Archive
- `EMAIL_ENABLED` يبقى 0

## محتويات هذا الأرشيف
- كل ملفات المصدر اللازمة للبناء
- `secrets.h.example` (انسخه إلى `secrets.h` واملأ البيانات)
- مجلد `diagrams/` إن وُجد
- **لا** يحتوي على `secrets.h` الحقيقي (أمان)
