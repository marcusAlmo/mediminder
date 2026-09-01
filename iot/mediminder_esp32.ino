/*
 * Mediminder - Smart Medicine Dispenser by SJNHS
 * ============================================================================
 * Hardware Pinout (From Schematic):
 *   1. Microstepper (ULN2003) : IN1=GPIO 26, IN2=GPIO 27, IN3=GPIO 25, IN4=GPIO 32
 *   2. Sonar (HC-SR04)        : TRIG=GPIO 5, ECHO=GPIO 18 (with Voltage Divider)
 *   3. Character LCD (I2C)    : SDA=GPIO 21, SCL=GPIO 22 (Address: 0x27 / 5V VCC)
 *   4. RTC Clock (DS3231)     : SDA=GPIO 21, SCL=GPIO 22 (3V3 VCC)
 *   5. Passive Buzzer         : Signal=GPIO 19
 *   6. LED Bulb (Relay)       : IN/Signal=GPIO 23
 *   7. Servo Motor            : Signal=GPIO 13 (External Power)
 *
 * Dispenser Logic Flow:
 *   1.  Poll GET /api/dispense every 15 seconds for config + RTC sync (with retry & fallback)
 *   2.  Display RTC time (refreshes every second); every 10s show patient + next dispense
 *   3.  On schedule match: rotate microstepper 52 degrees (CW)
 *   4.  Raise servo to 90° (open pill cover)
 *   5.  Hold 5 seconds, then return servo to 0° (close cover)
 *   6.  Play high-frequency buzzer for 10 seconds; POST dispense log (decrements rack)
 *   7.  LCD: "Medicine Ready, <patient name>"
 *   8.  After 1 minute, repeat alarm that medicine is ready
 *   9.  When sonar detects drawer opening (distance increases), stop alarm loop
 *  10.  POST intake log (drawer opened = confirmed medication intake)
 *
 * Retry Logic: API calls retry twice with 5-second intervals on failure
 * Fallback: Uses cached settings and data if all retries fail
 *
 * Serial: 9600 Baud (8-N-1) | HELP command available
 * ============================================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <stdarg.h>

#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
#endif

// ============================================================================
// 1. PIN CONFIGURATION (Schematic Mapping)
// ============================================================================

// Microstepper – 28BYJ-48 via ULN2003
#define STEPPER_IN1_PIN  26
#define STEPPER_IN2_PIN  27
#define STEPPER_IN3_PIN  25
#define STEPPER_IN4_PIN  32
#define STEPS_PER_360    4096   // 4096 half-steps = 360°

// HC-SR04 Ultrasonic Sonar
#define SONAR_TRIG_PIN   5
#define SONAR_ECHO_PIN   18
#define SONAR_TIMEOUT_US 26000UL  // ~4.4 m max range

// I2C Bus (shared LCD + DS3231)
#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22
#define LCD_ADDR         0x27
#define LCD_COLS         16
#define LCD_ROWS         2

// Passive Buzzer
#define BUZZER_PIN       19

// LED Bulb Relay
#define LED_RELAY_PIN    23

// ============================================================================
// DEBUG & PERFORMANCE MONITORING
// ============================================================================
#define DEBUG_MODE       false  // Set to true for verbose debug logs
#define PERF_MONITORING  true   // Set to true to log operation timings
#define HARDWARE_STATUS_INTERVAL_MS 60000UL  // Log hardware status every 60s

// Servo Motor (Pill Cover Gate)
#define SERVO_PIN        13

#define SERIAL_BAUD_RATE 9600

// ============================================================================
// 2. NETWORK & API CONFIGURATION
// ============================================================================
#define ENABLE_WIFI true
const char* WIFI_SSID       = "test";
const char* WIFI_PASSWORD   = "tester2025";
const char* API_BASE_URL    = "http://10.81.124.83:5000";
const char* API_GET_PATH    = "/api/dispense";
const char* API_POST_DISPENSE_PATH = "/api/dispense-log";
const char* API_POST_INTAKE_PATH   = "/api/intake";

// ============================================================================
// 3. TUNING CONSTANTS
// ============================================================================
#define POLL_INTERVAL_MS      15000UL  // GET poll every 15 s (temporary)
#define LCD_ALT_DISPLAY_MS    10000UL  // Alternate between clock & patient info every 10 s
#define ROTATION_DEGREES      52.0f    // Stepper advance per dispense slot
#define SERVO_OPEN_ANGLE      90       // Pill cover open angle
#define SERVO_HOLD_MS         5000UL   // Hold cover open for 5 s
#define BUZZER_ALERT_MS       10000UL  // Buzzer on for 10 s after dispense
#define BUZZER_HIGH_FREQ      1000     // 1 kHz high-frequency alert
#define REPEAT_ALARM_INTERVAL 60000UL  // Re-alarm every 60 s if med not taken
#define SONAR_IDLE_THRESHOLD  20.0f    // cm – drawer considered "open" when dist > threshold
#define RACK_WARNING_DEFAULT  3        // Default low-rack warning threshold

// ============================================================================
// 4. ANSI TERMINAL COLOURS
// ============================================================================
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_WHITE   "\033[37m"

// ============================================================================
// 5. DISPENSER STATE MACHINE
// ============================================================================
enum DispenserState {
  STATE_IDLE,          // Showing clock; waiting for schedule match
  STATE_DISPENSING,    // Rotating stepper + opening/closing servo
  STATE_MED_READY,     // Waiting for patient to open drawer
  STATE_INTAKE_DONE    // Sonar triggered; intake confirmed; wait for next slot
};

// ============================================================================
// 6. GLOBAL VARIABLES & DATA STRUCTURES
// ============================================================================
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

struct RackSlot {
  int    rackId;
  String dt;         // "YYYY-MM-DD HH:MM"
  String status;     // "pending" / "dispensed"
};

// Configuration (populated / refreshed by GET poll)
String   cfg_patientName    = "Test Patient";
float    cfg_rotationDeg    = ROTATION_DEGREES;
int      cfg_currentRack    = 1;          // Active sequential rack to dispense (1 to 7)
int      cfg_rackCount      = 7;          // Total available loaded racks
int      cfg_rackThreshold  = RACK_WARNING_DEFAULT;
RackSlot cfg_racks[8];                    // 7 Storage racks (index 1..7)
int      cfg_scheduleCount  = 7;

// RTC (synced from API current_time; ticked by software every second)
int rtcYear = 2026, rtcMonth = 8, rtcDay = 31;
int rtcHour = 14,   rtcMin   = 30, rtcSec = 0;

// Timers
unsigned long lastPollMs        = 0;
unsigned long lastClockTickMs   = 0;
unsigned long lastLcdAltMs      = 0;
unsigned long dispenseReadyMs   = 0;   // When STATE_MED_READY was entered
unsigned long lastRepeatAlarmMs = 0;   // Last time re-alarm played in MED_READY
unsigned long lastHardwareStatusMs = 0; // Last time hardware status was logged

// State
DispenserState state      = STATE_IDLE;
int  lastDispensedRackId  = -1;    // Prevents re-triggering same rack within same minute
int  activeDispenseRack   = 1;     // Rack currently being dispensed
bool lcdShowingAlt        = false; // Clock vs patient-info alternation flag
bool isWifiConnected      = false;
bool rackWarningTriggered = false;

// Baseline sonar distance captured when MED_READY begins (for drawer-open detection)
float sonarBaselineDistCm = -1.0f;

// ULN2003 8-step half-step sequence
const uint8_t uln2003Steps[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

// ============================================================================
// 7. LOGGING HELPERS
// ============================================================================
void serialPrintf(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
}

void printTimestamp() {
  unsigned long ms = millis();
  unsigned long s  = ms / 1000;
  serialPrintf(ANSI_CYAN "[%02lu:%02lu.%03lu] " ANSI_RESET, s / 60, s % 60, ms % 1000);
}

void logInfo(const char* tag, const char* msg) {
  printTimestamp();
  serialPrintf(ANSI_BOLD ANSI_CYAN "[INFO]  " ANSI_RESET "[%-9s] %s\n", tag, msg);
}
void logOk(const char* tag, const char* msg) {
  printTimestamp();
  serialPrintf(ANSI_BOLD ANSI_GREEN "[OK]    " ANSI_RESET "[%-9s] %s\n", tag, msg);
}
void logWarn(const char* tag, const char* msg) {
  printTimestamp();
  serialPrintf(ANSI_BOLD ANSI_YELLOW "[WARN]  " ANSI_RESET "[%-9s] %s\n", tag, msg);
}
void logError(const char* tag, const char* msg) {
  printTimestamp();
  serialPrintf(ANSI_BOLD ANSI_RED "[ERROR] " ANSI_RESET "[%-9s] %s\n", tag, msg);
}

void logDebug(const char* tag, const char* msg) {
  if (DEBUG_MODE) {
    printTimestamp();
    serialPrintf(ANSI_BOLD ANSI_WHITE "[DEBUG] " ANSI_RESET "[%-9s] %s\n", tag, msg);
  }
}

void logPerf(const char* tag, unsigned long startMs) {
  if (PERF_MONITORING) {
    unsigned long duration = millis() - startMs;
    printTimestamp();
    serialPrintf(ANSI_BOLD ANSI_BLUE "[PERF]  " ANSI_RESET "[%-9s] %lu ms\n", tag, duration);
  }
}

void logHardwareStatus() {
  #if defined(ESP32)
  if (DEBUG_MODE) {
    printTimestamp();
    Serial.println(F(ANSI_BOLD ANSI_BLUE "=== HARDWARE STATUS ===" ANSI_RESET));
    serialPrintf("  Free Heap    : %d bytes\n", ESP.getFreeHeap());
    if (isWifiConnected) {
      serialPrintf("  WiFi Signal  : %d dBm\n", WiFi.RSSI());
    } else {
      serialPrintf("  WiFi Signal  : N/A (offline)\n");
    }
    serialPrintf("  Uptime       : %lu seconds\n", millis() / 1000);
    Serial.println();
  }
  #endif
}

// ============================================================================
// 8. TIMEKEEPING & DATE SPECIFIC SCHEDULING
// ============================================================================
void tickRtcClock() {
  rtcSec++;
  if (rtcSec >= 60) {
    rtcSec = 0;
    rtcMin++;
    if (rtcMin >= 60) {
      rtcMin = 0;
      rtcHour++;
      if (rtcHour >= 24) {
        rtcHour = 0;
        rtcDay++;
        // Approximate month roll for autonomous operation
        if (rtcDay > 30) { rtcDay = 1; rtcMonth++; if (rtcMonth > 12) { rtcMonth = 1; rtcYear++; } }
      }
    }
  }
}

// "YYYY-MM-DD HH:MM" for exact date & time schedule comparison
String getYMDHM() {
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", rtcYear, rtcMonth, rtcDay, rtcHour, rtcMin);
  return String(buf);
}

// "HH:MM:SS" for display
String getHMS() {
  char buf[10];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", rtcHour, rtcMin, rtcSec);
  return String(buf);
}

// "YYYY-MM-DD" for display
String getYMD() {
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", rtcYear, rtcMonth, rtcDay);
  return String(buf);
}

// ISO timestamp for API payloads
String getIsoTs() {
  char buf[30];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
           rtcYear, rtcMonth, rtcDay, rtcHour, rtcMin, rtcSec);
  return String(buf);
}

// Return next active scheduled rack due string
String nextScheduleDescription() {
  for (int i = 1; i <= 7; i++) {
    if (cfg_racks[i].rackId == cfg_currentRack && cfg_racks[i].status == "pending") {
      // Return e.g. "#3 08-31 18:00"
      String dt = cfg_racks[i].dt;
      if (dt.length() >= 16) {
        return "#" + String(cfg_currentRack) + " " + dt.substring(5);
      }
      return "#" + String(cfg_currentRack) + " " + dt;
    }
  }
  return "All Dispensed";
}

// Initialize default 7 storage racks configuration
void initDefault7Racks() {
  cfg_rackCount = 7;
  cfg_scheduleCount = 7;
  cfg_currentRack = 1;
  const char* defaultDates[7] = {
    "2026-08-31 08:00", "2026-08-31 12:30", "2026-08-31 18:00", "2026-08-31 21:00",
    "2026-09-01 08:00", "2026-09-01 12:30", "2026-09-01 18:00"
  };
  for (int i = 1; i <= 7; i++) {
    cfg_racks[i].rackId = i;
    cfg_racks[i].dt = defaultDates[i - 1];
    cfg_racks[i].status = "pending";
  }
}

// Returns rackId (1..7) if current date and time matches the sequential scheduled rack
int checkAlarm() {
  String currentYMDHM = getYMDHM();

  // Check the active sequential rack first
  if (cfg_currentRack >= 1 && cfg_currentRack <= 7) {
    RackSlot& slot = cfg_racks[cfg_currentRack];
    if (slot.status == "pending" && slot.dt == currentYMDHM && lastDispensedRackId != slot.rackId) {
      return slot.rackId;
    }
  }

  // Fallback: check any pending rack matching exact current minute
  for (int i = 1; i <= 7; i++) {
    if (cfg_racks[i].status == "pending" && cfg_racks[i].dt == currentYMDHM && lastDispensedRackId != cfg_racks[i].rackId) {
      return cfg_racks[i].rackId;
    }
  }

  return -1;
}

// ============================================================================
// 9. HARDWARE DRIVERS
// ============================================================================

// --- ULN2003 Microstepper ---
void setCoils(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  digitalWrite(STEPPER_IN1_PIN, a);
  digitalWrite(STEPPER_IN2_PIN, b);
  digitalWrite(STEPPER_IN3_PIN, c);
  digitalWrite(STEPPER_IN4_PIN, d);
}
void releaseCoils() { setCoils(0,0,0,0); }

void rotateDegrees(float deg, bool cw = true) {
  unsigned long startMs = millis();
  int steps = max(1, (int)((deg / 360.0f) * STEPS_PER_360));
  printTimestamp();
  serialPrintf("  " ANSI_CYAN "[STEPPER]" ANSI_RESET " %.1f° %s → %d half-steps\n", deg, cw?"CW":"CCW", steps);
  for (int s = 0; s < steps; s++) {
    int idx = cw ? (s % 8) : (7 - (s % 8));
    setCoils(uln2003Steps[idx][0], uln2003Steps[idx][1],
             uln2003Steps[idx][2], uln2003Steps[idx][3]);
    delayMicroseconds(1200);
  }
  releaseCoils();
  logPerf("STEPPER", startMs);
}

// --- Bit-bang Servo (no library) ---
void servoWrite(int angle, int holdMs = 500) {
  unsigned long startMs = millis();
  angle = constrain(angle, 0, 180);
  int pw = map(angle, 0, 180, 500, 2400);
  int cycles = max(15, holdMs / 20);
  printTimestamp();
  serialPrintf("  " ANSI_CYAN "[SERVO]" ANSI_RESET " GPIO %d → %d° (%d µs, %d cycles)\n",
               SERVO_PIN, angle, pw, cycles);
  for (int i = 0; i < cycles; i++) {
    digitalWrite(SERVO_PIN, HIGH); delayMicroseconds(pw);
    digitalWrite(SERVO_PIN, LOW);  delayMicroseconds(20000 - pw);
  }
  logPerf("SERVO", startMs);
}

// --- Passive Buzzer (bit-bang tone) ---
void buzzTone(unsigned int freq, unsigned long durMs) {
  if (freq == 0) return;
  unsigned long half = 500000UL / freq;
  unsigned long n    = ((unsigned long)freq * durMs) / 1000UL;
  for (unsigned long i = 0; i < n; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(half);
    digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(half);
  }
}

// --- HC-SR04 Sonar ---
float sonarReadCm() {
  digitalWrite(SONAR_TRIG_PIN, LOW);  delayMicroseconds(4);
  digitalWrite(SONAR_TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(SONAR_TRIG_PIN, LOW);
  long dur = pulseIn(SONAR_ECHO_PIN, HIGH, SONAR_TIMEOUT_US);
  if (dur == 0) return -1.0f;
  return (float)dur / 58.2f;
}

// 3-sample median for stability
float sonarFiltered() {
  float r[3]; int n = 0;
  for (int i = 0; i < 3; i++) {
    float d = sonarReadCm();
    if (d > 0) r[n++] = d;
    delayMicroseconds(600);
  }
  if (n == 0) return -1.0f;
  if (n == 1) return r[0];
  if (n == 2) return (r[0] + r[1]) / 2.0f;
  // Sort 3
  if (r[0]>r[1]){float t=r[0];r[0]=r[1];r[1]=t;}
  if (r[1]>r[2]){float t=r[1];r[1]=r[2];r[2]=t;}
  if (r[0]>r[1]){float t=r[0];r[0]=r[1];r[1]=t;}
  float result = r[1];
  logDebug("SONAR", ("Filtered distance: " + String(result) + " cm").c_str());
  return result;
}

// ============================================================================
// 10. LCD HELPERS
// ============================================================================
void lcdPrint(const char* row0, const char* row1) {
  logDebug("LCD", ("row0: " + String(row0) + " | row1: " + String(row1)).c_str());
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(row0);
  lcd.setCursor(0, 1); lcd.print(row1);
}

// Pad/truncate string to exactly 16 chars for clean LCD display
String lcdFit(String s) {
  while (s.length() < 16) s += ' ';
  return s.substring(0, 16);
}

// ============================================================================
// 11. WI-FI CONNECTION
// ============================================================================
void connectWifi() {
#if defined(ESP32)
  if (!ENABLE_WIFI) { logWarn("WIFI", "Disabled – Standalone mode."); return; }
  logInfo("WIFI", "Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(300); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    isWifiConnected = true;
    logOk("WIFI", ("Connected! IP: " + WiFi.localIP().toString()).c_str());

    // Configure NTP for Philippine Standard Time (PHT / UTC+8)
    configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    logInfo("NTP", "NTP time server configured for Philippine Standard Time (PHT / UTC+8).");
  } else {
    isWifiConnected = false;
    logWarn("WIFI", "Timed out – running offline.");
  }
#endif
}

// ============================================================================
// 11. RETRY & FALLBACK CONFIGURATION
// ============================================================================
#define API_RETRY_COUNT      2       // Number of retries on API failure
#define API_RETRY_DELAY_MS   5000UL  // 5 seconds between retries

// Cached configuration for fallback when API is unavailable
bool hasCachedConfig = false;

// ============================================================================
// 12. JSON STRING EXTRACTION HELPERS (Robust Parsing)
// ============================================================================
String extractJsonString(const String& chunk, const String& key) {
  int kIdx = chunk.indexOf("\"" + key + "\"");
  if (kIdx < 0) return "";
  int colonIdx = chunk.indexOf(':', kIdx + key.length() + 2);
  if (colonIdx < 0) return "";
  int qStart = chunk.indexOf('"', colonIdx + 1);
  if (qStart < 0) return "";
  int qEnd = chunk.indexOf('"', qStart + 1);
  if (qEnd < 0) return "";
  return chunk.substring(qStart + 1, qEnd);
}

int extractJsonInt(const String& chunk, const String& key, int defaultVal = 0) {
  int kIdx = chunk.indexOf("\"" + key + "\"");
  if (kIdx < 0) return defaultVal;
  int colonIdx = chunk.indexOf(':', kIdx + key.length() + 2);
  if (colonIdx < 0) return defaultVal;
  int valStart = colonIdx + 1;
  while (valStart < (int)chunk.length() && (chunk[valStart] == ' ' || chunk[valStart] == '\t' || chunk[valStart] == '\r' || chunk[valStart] == '\n')) {
    valStart++;
  }
  return chunk.substring(valStart).toInt();
}

float extractJsonFloat(const String& chunk, const String& key, float defaultVal = 0.0f) {
  int kIdx = chunk.indexOf("\"" + key + "\"");
  if (kIdx < 0) return defaultVal;
  int colonIdx = chunk.indexOf(':', kIdx + key.length() + 2);
  if (colonIdx < 0) return defaultVal;
  int valStart = colonIdx + 1;
  while (valStart < (int)chunk.length() && (chunk[valStart] == ' ' || chunk[valStart] == '\t' || chunk[valStart] == '\r' || chunk[valStart] == '\n')) {
    valStart++;
  }
  return chunk.substring(valStart).toFloat();
}

// ============================================================================
// 13. STEP 1 – GET POLL: Sync 7-rack schedule, active rack & full PHT RTC timestamp
// ============================================================================
void pollScheduleFromApi() {
#if defined(ESP32)
  unsigned long startMs = millis();
  if (!isWifiConnected || WiFi.status() != WL_CONNECTED) {
    if (hasCachedConfig) {
      logWarn("API-GET", "WiFi disconnected. Using cached settings and data.");
    }
    return;
  }

  HTTPClient http;
  String url = String(API_BASE_URL) + API_GET_PATH + "?device=esp32";
  int code = -1;
  String body = "";
  bool success = false;

  // Retry logic: attempt up to (1 + API_RETRY_COUNT) times
  for (int attempt = 0; attempt <= API_RETRY_COUNT; attempt++) {
    if (attempt > 0) {
      printTimestamp();
      serialPrintf("  " ANSI_YELLOW "[RETRY %d/%d]" ANSI_RESET " Retrying API request in 5 seconds...\n", 
                   attempt, API_RETRY_COUNT);
      delay(API_RETRY_DELAY_MS);
    }

    http.begin(url);
    http.setTimeout(4000);
    code = http.GET();

    if (code == 200) {
      body = http.getString();
      success = true;
      logOk("API-GET", "7-Rack schedule & RTC timestamp received.");
      break;
    } else {
      printTimestamp();
      serialPrintf("  " ANSI_RED "[API-GET FAILED]" ANSI_RESET " HTTP code: %d (Attempt %d/%d)\n", 
                   code, attempt + 1, API_RETRY_COUNT + 1);
    }
    http.end();
  }

  if (success) {
    // ---- patient_name ----
    String pName = extractJsonString(body, "patient_name");
    if (pName.length() > 0) cfg_patientName = pName;

    // ---- rotation_degree ----
    float deg = extractJsonFloat(body, "rotation_degree", cfg_rotationDeg);
    if (deg > 0 && deg <= 360) cfg_rotationDeg = deg;

    // ---- current_rack (sequential pointer 1..7) ----
    int cr = extractJsonInt(body, "current_rack", cfg_currentRack);
    if (cr >= 1 && cr <= 7) cfg_currentRack = cr;

    // ---- rack_count (default 7 racks) ----
    cfg_rackCount = extractJsonInt(body, "rack_count", cfg_rackCount);

    // ---- rack_warning_threshold ----
    cfg_rackThreshold = extractJsonInt(body, "rack_warning_threshold", cfg_rackThreshold);

    // ---- Parse 7 Racks array from "racks": [ { ... }, ... ] ----
    int racksKey = body.indexOf("\"racks\"");
    if (racksKey >= 0) {
      int arrStart = body.indexOf('[', racksKey);
      int arrEnd = body.lastIndexOf(']');
      if (arrStart >= 0 && arrEnd > arrStart) {
        int pos = arrStart + 1;
        while (pos < arrEnd) {
          int objStart = body.indexOf('{', pos);
          if (objStart < 0 || objStart > arrEnd) break;
          int objEnd = body.indexOf('}', objStart);
          if (objEnd < 0 || objEnd > arrEnd) break;

          String objChunk = body.substring(objStart, objEnd + 1);
          int rId = extractJsonInt(objChunk, "rack_id", 0);
          if (rId >= 1 && rId <= 7) {
            cfg_racks[rId].rackId = rId;
            String dt = extractJsonString(objChunk, "datetime");
            if (dt.length() > 0) cfg_racks[rId].dt = dt;
            String st = extractJsonString(objChunk, "status");
            if (st.length() > 0) cfg_racks[rId].status = st;
          }
          pos = objEnd + 1;
        }
      }
    }

    // ---- Full RTC Date & Time Sync (PHT / UTC+8) ----
    int hr = extractJsonInt(body, "hour", -1);
    if (hr >= 0 && hr <= 23) rtcHour = hr;
    int mn = extractJsonInt(body, "minute", -1);
    if (mn >= 0 && mn <= 59) rtcMin = mn;
    int sc = extractJsonInt(body, "second", -1);
    if (sc >= 0 && sc <= 59) rtcSec = sc;
    int yr = extractJsonInt(body, "year", -1);
    if (yr >= 2020) rtcYear = yr;
    int mo = extractJsonInt(body, "month", -1);
    if (mo >= 1 && mo <= 12) rtcMonth = mo;
    int dy = extractJsonInt(body, "day", -1);
    if (dy >= 1 && dy <= 31) rtcDay = dy;

    // Mark that we have valid cached configuration
    hasCachedConfig = true;

    printTimestamp();
    serialPrintf("  " ANSI_BOLD ANSI_GREEN "=== [DATA RECEIVED FROM GET REQUEST] ===" ANSI_RESET "\n");
    serialPrintf("    • Patient Name       : %s\n", cfg_patientName.c_str());
    serialPrintf("    • Active Next Rack   : Rack #%d\n", cfg_currentRack);
    serialPrintf("    • Step Angle         : %.1f deg per slot\n", cfg_rotationDeg);
    serialPrintf("    • Available Racks    : %d racks (Threshold: %d)\n", cfg_rackCount, cfg_rackThreshold);
    serialPrintf("    • RTC Synchronized   : %s PHT (UTC+8)\n", getYMDHM().c_str());
    serialPrintf("    • 7-Rack Schedule    :\n");
    for (int i = 1; i <= 7; i++) {
      bool isNext = (cfg_racks[i].rackId == cfg_currentRack && cfg_racks[i].status == "pending");
      serialPrintf("      [%c] Rack #%d: %-16s | Status: %-9s %s\n",
                   isNext ? '*' : ' ', cfg_racks[i].rackId,
                   cfg_racks[i].dt.c_str(), cfg_racks[i].status.c_str(),
                   isNext ? "<- [NEXT DUE]" : "");
    }
    serialPrintf("  " ANSI_BOLD ANSI_GREEN "========================================" ANSI_RESET "\n");

  } else {
    // All retries failed - use cached configuration
    printTimestamp();
    serialPrintf("  " ANSI_BOLD ANSI_RED "[API FAILED]" ANSI_RESET " All %d attempts failed. Using cached settings and data.\n", 
                 API_RETRY_COUNT + 1);
    if (!hasCachedConfig) {
      logWarn("CACHE", "No cached configuration available. Using default settings.");
    }
  }
  http.end();
  logPerf("API-GET", startMs);
#endif
}

// ============================================================================
// 13. STEP 6 – POST DISPENSE LOG (Rack Specific)
// ============================================================================
void postDispenseLog(int rackId) {
#if defined(ESP32)
  unsigned long startMs = millis();
  if (!isWifiConnected || WiFi.status() != WL_CONNECTED) {
    logWarn("API-POST", "Offline – dispense event not sent to server.");
    return;
  }
  HTTPClient http;
  http.begin(String(API_BASE_URL) + API_POST_DISPENSE_PATH);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  char body[256];
  snprintf(body, sizeof(body),
    "{\"timestamp\":\"%s\",\"patient_name\":\"%s\",\"rotation_degree\":%d,"
    "\"rack_id\":%d,\"slot\":%d,\"status\":\"success\","
    "\"notes\":\"Rack #%d dispensed via stepper %.0f deg\"}",
    getIsoTs().c_str(), cfg_patientName.c_str(), (int)cfg_rotationDeg,
    rackId, rackId, rackId, cfg_rotationDeg);

  logInfo("API-POST", "Sending dispense log for Rack...");
  printTimestamp(); serialPrintf("  Body: %s\n", body);

  int code = http.POST(body);
  if (code == 201 || code == 200) {
    String resp = http.getString();
    logOk("API-POST", ("Dispense logged. Response: " + resp.substring(0,80)).c_str());

    // Update rack state locally
    if (rackId >= 1 && rackId <= 7) {
      cfg_racks[rackId].status = "dispensed";
    }

    // Parse next_rack and rack_count from response
    int nxt = extractJsonInt(resp, "next_rack", 0);
    if (nxt >= 1 && nxt <= 7) cfg_currentRack = nxt;

    int ri = extractJsonInt(resp, "rack_count", -1);
    if (ri >= 0) cfg_rackCount = ri;

    bool warn = (resp.indexOf("\"rack_warning\":true") >= 0 || resp.indexOf("\"rack_warning\": true") >= 0);
    if (warn && !rackWarningTriggered) {
      rackWarningTriggered = true;
      logWarn("RACK", ("LOW RACK WARNING! Only " + String(cfg_rackCount) + " storage racks remaining (threshold: " + String(cfg_rackThreshold) + ")").c_str());
    }
  } else {
    logError("API-POST", ("Failed. HTTP code: " + String(code)).c_str());
  }
  http.end();
  logPerf("API-POST", startMs);
#endif
}

// ============================================================================
// STEP 10 – POST INTAKE LOG (drawer opened)
// ============================================================================
void postIntakeLog(int rackId, float drawerDistCm) {
#if defined(ESP32)
  unsigned long startMs = millis();
  if (!isWifiConnected || WiFi.status() != WL_CONNECTED) {
    logWarn("API-POST", "Offline – intake event not sent to server.");
    return;
  }
  HTTPClient http;
  http.begin(String(API_BASE_URL) + API_POST_INTAKE_PATH);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  char body[256];
  snprintf(body, sizeof(body),
    "{\"timestamp\":\"%s\",\"patient_name\":\"%s\",\"rack_id\":%d,\"slot\":%d,"
    "\"notes\":\"Patient opened drawer at %.1f cm (Rack #%d)\"}",
    getIsoTs().c_str(), cfg_patientName.c_str(), rackId, rackId, drawerDistCm, rackId);

  logInfo("API-POST", "Sending intake log...");
  printTimestamp(); serialPrintf("  Body: %s\n", body);

  int code = http.POST(body);
  if (code == 201 || code == 200) {
    logOk("API-POST", "Intake confirmed and logged to server.");
  } else {
    logError("API-POST", ("Intake POST failed. HTTP code: " + String(code)).c_str());
  }
  http.end();
  logPerf("API-POST", startMs);
#endif
}

// ============================================================================
// 14. DISPLAY – IDLE CLOCK / PATIENT INFO ALTERNATION (Step 2)
// ============================================================================
void updateIdleDisplay() {
  unsigned long now = millis();
  bool showAlt = (now - lastLcdAltMs >= LCD_ALT_DISPLAY_MS);

  if (showAlt) {
    // Show patient name + next sequential rack due every 10 s
    lcdShowingAlt = true;
    lastLcdAltMs = now;

    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "%-16s", cfg_patientName.substring(0, 16).c_str());
    snprintf(row1, sizeof(row1), "Next: %-10s", nextScheduleDescription().substring(0, 10).c_str());
    lcd.setCursor(0, 0); lcd.print(row0);
    lcd.setCursor(0, 1); lcd.print(row1);
  } else {
    // Show live clock (HH:MM:SS) & date / available racks
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "Time: %s ", getHMS().c_str());
    snprintf(row1, sizeof(row1), "%-16s", (String(getYMD().substring(5)) + " Racks:" + cfg_rackCount).c_str());
    lcd.setCursor(0, 0); lcd.print(row0);
    lcd.setCursor(0, 1); lcd.print(row1);
  }
}

// ============================================================================
// 15. DISPENSE CYCLE (Steps 3–7)
// ============================================================================
void runDispenseCycle(int rackId) {
  unsigned long startMs = millis();
  state = STATE_DISPENSING;
  activeDispenseRack = (rackId >= 1 && rackId <= 7) ? rackId : cfg_currentRack;

  Serial.println(F("\n" ANSI_BOLD ANSI_MAGENTA
    "+===========================================================================+\n"
    "|                    STARTING DISPENSE CYCLE                                |\n"
    "+===========================================================================+" ANSI_RESET));
  printTimestamp();
  serialPrintf("  Rack #%d | Patient: %s | Rotation: %.0f° | Scheduled: %s\n",
               activeDispenseRack, cfg_patientName.c_str(), cfg_rotationDeg, getYMDHM().c_str());

  // --- STEP 3: Rotate microstepper by cfg_rotationDeg (default 52°) ---
  logInfo("STEP-3", "Rotating 28BYJ-48 microstepper via ULN2003...");
  char row0[17];
  snprintf(row0, sizeof(row0), "Dispensing #%d...", activeDispenseRack);
  lcdPrint(row0, "Rotating motor  ");
  rotateDegrees(cfg_rotationDeg, true);
  logOk("STEP-3", "Stepper rotation complete.");

  // --- STEP 4: Raise servo to 90° (open pill cover) ---
  logInfo("STEP-4", "Opening pill cover – Servo to 90°...");
  lcdPrint("Opening Cover...", "Servo: 90 deg   ");
  servoWrite(SERVO_OPEN_ANGLE, 600);
  logOk("STEP-4", "Pill cover open.");

  // --- STEP 5: Hold 5 seconds then close servo back to 0° ---
  logInfo("STEP-5", "Holding cover open for 5 seconds...");
  lcdPrint("Cover OPEN      ", "Hold 5s...      ");
  delay((int)SERVO_HOLD_MS);
  logInfo("STEP-5", "Closing pill cover – Servo to 0°...");
  lcdPrint("Closing Cover...", "Servo: 0 deg    ");
  servoWrite(0, 500);
  logOk("STEP-5", "Pill cover closed and locked.");

  // --- STEP 6: High-frequency buzzer & External LED Light for 10 seconds ---
  logInfo("STEP-6", "Activating external LED light (GPIO 23) and 1 kHz buzzer for 10s...");
  lcdPrint("! MEDICINE RDY !", "LED & Buzzer... ");

  // Pulse external LED light and buzzer in sync (500 ms ON / 200 ms OFF) for 10 s total
  unsigned long buzzStart = millis();
  while (millis() - buzzStart < BUZZER_ALERT_MS) {
    digitalWrite(LED_RELAY_PIN, HIGH);
    buzzTone(BUZZER_HIGH_FREQ, 500);
    digitalWrite(LED_RELAY_PIN, LOW);
    delay(200);
  }
  // Keep external LED light solid ON to illuminate tray for patient retrieval
  digitalWrite(LED_RELAY_PIN, HIGH);
  logOk("STEP-6", "Buzzer alert complete. External LED light ON to illuminate medication tray.");

  // POST dispense log (with rack_count decrement and sequence progression on server)
  postDispenseLog(activeDispenseRack);

  // --- STEP 7: LCD "Medicine Ready, <name>" ---
  {
    char row1[17];
    snprintf(row1, sizeof(row1), "Rack #%d: %s", activeDispenseRack, cfg_patientName.substring(0, 8).c_str());
    lcdPrint("Medicine Ready! ", row1);
  }

  // Capture baseline sonar distance (closed-drawer reference)
  sonarBaselineDistCm = sonarFiltered();
  printTimestamp();
  serialPrintf("  Sonar baseline: %.1f cm (drawer-open detection threshold: %.1f cm)\n",
               sonarBaselineDistCm, sonarBaselineDistCm + SONAR_IDLE_THRESHOLD);

  // Record rack as dispensed
  lastDispensedRackId = activeDispenseRack;
  dispenseReadyMs     = millis();
  lastRepeatAlarmMs   = millis();

  state = STATE_MED_READY;
  logInfo("STATE", "Entering MED_READY – External LED ON. Waiting for patient to open drawer.");
  logPerf("DISPENSE", startMs);
}

// ============================================================================
// 16. MED_READY STATE HANDLER (Steps 8–10)
// ============================================================================
void handleMedReadyState() {
  unsigned long now = millis();

  // --- STEP 8: Repeat alarm every 60 s if med still not taken ---
  if (now - lastRepeatAlarmMs >= REPEAT_ALARM_INTERVAL) {
    lastRepeatAlarmMs = now;
    logWarn("ALARM", "Medicine not yet taken – repeating reminder with external LED strobe!");
    lcdPrint("! REMINDER !    ", "Take Your Meds! ");
    
    // Strobe external LED light with triple alert beeps
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_RELAY_PIN, HIGH);
      buzzTone(BUZZER_HIGH_FREQ, 200);
      digitalWrite(LED_RELAY_PIN, LOW);
      delay(150);
    }
    // Re-enable solid external LED light illumination
    digitalWrite(LED_RELAY_PIN, HIGH);

    // Restore med-ready message
    char row1[17];
    snprintf(row1, sizeof(row1), "%-16s", cfg_patientName.substring(0, 16).c_str());
    lcdPrint("Medicine Ready! ", row1);
  }

  // --- STEP 9 & 10: Detect drawer opening via sonar ---
  float dist = sonarFiltered();

  // Drawer is considered "opened" when distance increases significantly
  // from baseline (i.e., patient's hand removes the cup, distance grows)
  bool drawerOpened = false;
  if (dist > 0 && sonarBaselineDistCm > 0) {
    drawerOpened = (dist > sonarBaselineDistCm + SONAR_IDLE_THRESHOLD);
  } else if (dist < 0) {
    // Timeout / no echo also treated as drawer opened (cup removed, nothing to reflect)
    drawerOpened = true;
  }

  if (drawerOpened) {
    printTimestamp();
    serialPrintf("  " ANSI_BOLD ANSI_GREEN "[DRAWER-OPEN]" ANSI_RESET
                 " Distance: %.1f cm (baseline was %.1f cm). Intake confirmed!\n",
                 dist, sonarBaselineDistCm);

    // Turn OFF external LED light
    digitalWrite(LED_RELAY_PIN, LOW);
    logInfo("LED", "External LED light turned OFF (drawer opened / med retrieved).");

    // Brief confirmation chime
    buzzTone(BUZZER_HIGH_FREQ, 100); delay(60);
    buzzTone(BUZZER_HIGH_FREQ * 2, 150);

    // LCD thank you
    lcdPrint("Thank You!      ", "Stay Healthy :) ");
    delay(2000);

    // --- STEP 10: POST intake log ---
    postIntakeLog(activeDispenseRack, dist);

    // Reset for next dispense slot
    state = STATE_IDLE;
    logInfo("STATE", "Intake confirmed. Returning to IDLE monitoring.");
  }
}

// ============================================================================
// 17. SERIAL COMMAND HANDLER
// ============================================================================
void handleSerialCmd() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim(); cmd.toUpperCase();
  if (cmd.length() == 0) return;

  printTimestamp();
  serialPrintf("  " ANSI_BOLD ANSI_YELLOW "[CLI]" ANSI_RESET " Command: \"%s\"\n", cmd.c_str());

  if (cmd == "D" || cmd == "DISPENSE") {
    logInfo("CMD", "Manual dispense triggered.");
    runDispenseCycle(-1);
  } else if (cmd == "POLL") {
    logInfo("CMD", "Manual API poll triggered.");
    pollScheduleFromApi();
  } else if (cmd == "STATUS" || cmd == "S") {
    Serial.println(F("\n" ANSI_BOLD ANSI_WHITE "+=== STATUS ===+"));
    serialPrintf("| Patient     : %s\n", cfg_patientName.c_str());
    serialPrintf("| Time        : %s\n", getHMS().c_str());
    serialPrintf("| Next dose   : %s\n", nextScheduleDescription().c_str());
    serialPrintf("| Rack count  : %d (warn ≤ %d)\n", cfg_rackCount, cfg_rackThreshold);
    serialPrintf("| Wi-Fi       : %s\n", isWifiConnected ? "CONNECTED" : "OFFLINE");
    serialPrintf("| State       : %d\n", (int)state);
    Serial.println(F(ANSI_RESET));
  } else if (cmd == "HELP") {
    Serial.println(F("\n" ANSI_BOLD "=== COMMANDS ===\n"
      "  D / DISPENSE : Manual immediate dispense\n"
      "  POLL         : Force API schedule poll\n"
      "  S / STATUS   : Show current status\n"
      "  HELP         : This menu\n" ANSI_RESET));
  } else {
    logWarn("CMD", "Unknown. Type HELP.");
  }
}

// ============================================================================
// 18. SETUP
// ============================================================================
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(800);

  // Banner
  Serial.println(F("\n" ANSI_BOLD ANSI_CYAN
    "╔═══════════════════════════════════════════════════════════╗\n"
    "║         MEDIMINDER by SJNHS  –  Smart Dispenser          ║\n"
    "║    Schematic Pinout | Flask REST IoT | 9600 Baud         ║\n"
    "╚═══════════════════════════════════════════════════════════╝" ANSI_RESET "\n"));

  // GPIO configuration
  logInfo("INIT", "Configuring GPIO...");
  pinMode(STEPPER_IN1_PIN, OUTPUT); pinMode(STEPPER_IN2_PIN, OUTPUT);
  pinMode(STEPPER_IN3_PIN, OUTPUT); pinMode(STEPPER_IN4_PIN, OUTPUT);
  releaseCoils();

  pinMode(SONAR_TRIG_PIN, OUTPUT);  digitalWrite(SONAR_TRIG_PIN, LOW);
  pinMode(SONAR_ECHO_PIN, INPUT);

  pinMode(BUZZER_PIN,    OUTPUT);   digitalWrite(BUZZER_PIN,    LOW);
  pinMode(LED_RELAY_PIN, OUTPUT);   digitalWrite(LED_RELAY_PIN, LOW);
  pinMode(SERVO_PIN,     OUTPUT);   digitalWrite(SERVO_PIN,     LOW);

  // I2C + LCD
  logInfo("INIT", "Initialising I2C bus and LCD...");
#if defined(ESP32) || defined(ESP8266)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
#else
  Wire.begin();
#endif
  lcd.init();
  lcd.backlight();
  lcdPrint("Mediminder      ", "by SJNHS        ");
  delay(2000);
  lcdPrint("Initializing... ", "Please wait...  ");
  delay(1000);

  // Initialize 7 storage racks by default
  initDefault7Racks();

  // Wi-Fi + initial API poll
  connectWifi();
  pollScheduleFromApi();

  // Startup chime
  buzzTone(392, 120); delay(40); buzzTone(523, 200);
  logOk("INIT", "Ready. Monitoring schedule.");

  // Log initial hardware status
  logHardwareStatus();

  // Prime LCD alternation timer so first display is immediate
  lastLcdAltMs = millis() - LCD_ALT_DISPLAY_MS;
}

// ============================================================================
// 19. MAIN LOOP
// ============================================================================
void loop() {
  unsigned long now = millis();

  // --- Serial CLI ---
  handleSerialCmd();

  // --- STEP 2: RTC software tick every 1000 ms ---
  if (now - lastClockTickMs >= 1000) {
    lastClockTickMs = now;
    tickRtcClock();

    // Refresh idle display once per second (manages its own 10s alternation)
    if (state == STATE_IDLE) {
      updateIdleDisplay();
    }

    // --- Schedule alarm check (only while IDLE) ---
    if (state == STATE_IDLE) {
      int rackId = checkAlarm();
      if (rackId >= 1 && rackId <= 7) {
        printTimestamp();
        serialPrintf(ANSI_BOLD ANSI_RED
          ">>> SCHEDULE MATCH: Rack #%d (%s) – Patient: %s <<<\n" ANSI_RESET,
          rackId, cfg_racks[rackId].dt.c_str(), cfg_patientName.c_str());
        runDispenseCycle(rackId);  // Blocks until MED_READY entered
      }
    }
  }

  // --- STEP 1: Periodic GET poll every 15 s ---
  if (now - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = now;
    if (state == STATE_IDLE) {      // Don't poll during dispensing
      pollScheduleFromApi();
    }
  }

  // --- Steps 8-10: Med ready / drawer detection ---
  if (state == STATE_MED_READY) {
    handleMedReadyState();
  }

  // --- Periodic hardware status logging ---
  if (now - lastHardwareStatusMs >= HARDWARE_STATUS_INTERVAL_MS) {
    lastHardwareStatusMs = now;
    logHardwareStatus();
  }

  delay(20); // Small yield
}
