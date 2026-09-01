# Mediminder - Logging Improvements Guide

Quick reference for implementing comprehensive logging across all components.

---

## 1. FRONTEND LOGGING IMPROVEMENTS

### Add Logger Class (index.html)

```javascript
// Add this before the main script section (around line 234)

class Logger {
  constructor(component) {
    this.component = component;
    this.enableDebug = localStorage.getItem('DEBUG_MODE') === 'true';
  }

  _formatTime() {
    return new Date().toISOString();
  }

  _formatMessage(level, tag, msg) {
    return `[${this._formatTime()}] [${level}] [${this.component}:${tag}] ${msg}`;
  }

  debug(tag, msg) {
    if (this.enableDebug) {
      console.debug(this._formatMessage('DEBUG', tag, msg));
    }
  }

  info(tag, msg) {
    console.log(this._formatMessage('INFO', tag, msg));
  }

  warn(tag, msg) {
    console.warn(this._formatMessage('WARN', tag, msg));
  }

  error(tag, msg) {
    console.error(this._formatMessage('ERROR', tag, msg));
  }

  enableDebugMode(enable = true) {
    localStorage.setItem('DEBUG_MODE', enable ? 'true' : 'false');
    this.enableDebug = enable;
    console.log(`Debug mode ${enable ? 'ENABLED' : 'DISABLED'}`);
  }
}

const logger = new Logger('FRONTEND');

// Usage examples:
// logger.info('CONFIG', 'Fetching current configuration...');
// logger.error('API', 'Failed to save configuration: ' + err.message);
// logger.warn('VALIDATION', 'Invalid rotation degree value');
// logger.debug('STATE', 'Current racks: ' + JSON.stringify(currentRacks));
```

### Update API Calls with Logging

```javascript
// Replace existing fetchCurrentConfig function (around line 420)

async function fetchCurrentConfig() {
  logger.info('API', 'Fetching current configuration...');
  const startTime = performance.now();
  
  try {
    const res = await fetch('/api/dispense');
    const duration = performance.now() - startTime;
    
    if (!res.ok) {
      logger.error('API', `HTTP ${res.status} error fetching config`);
      return;
    }

    const data = await res.json();
    logger.info('API', `Config fetched successfully (${duration.toFixed(2)}ms)`);
    logger.debug('API', 'Config data: ' + JSON.stringify(data));

    currentRacks = data.racks || [];
    activeCurrentRack = data.current_rack || 1;
    
    document.getElementById('preview-patient').innerText = data.patient_name || '--';
    document.getElementById('preview-degree').innerText = data.rotation_degree + '°';
    document.getElementById('preview-racks').innerText = data.rack_count + ' racks';
    document.getElementById('preview-next-due').innerText = data.next_rack_datetime || '--';
    
    renderRacksList();
    updatePreview();
    logger.info('UI', 'Configuration preview updated');
  } catch (err) {
    logger.error('API', 'Failed to load config: ' + err.message);
  }
}
```

### Update Form Submission with Logging

```javascript
// Replace existing form submission (around line 430)

document.getElementById('config-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  logger.info('FORM', 'Configuration form submitted');

  const patientName = document.getElementById('patient-name').value;
  const rotationDegree = parseInt(document.getElementById('rotation-degree').value);

  // Validation with logging
  if (!patientName.trim()) {
    logger.warn('VALIDATION', 'Patient name is empty');
    showAlert('❌ Patient name is required', true);
    return;
  }

  if (rotationDegree < 1 || rotationDegree > 360) {
    logger.warn('VALIDATION', `Invalid rotation degree: ${rotationDegree}`);
    showAlert('❌ Rotation degree must be between 1 and 360', true);
    return;
  }

  logger.debug('FORM', `Patient: ${patientName}, Rotation: ${rotationDegree}°`);

  const payload = {
    patient_name: patientName,
    rotation_degree: rotationDegree,
    current_rack: activeCurrentRack,
    racks: currentRacks
  };

  logger.info('API', 'Sending configuration to server...');
  const startTime = performance.now();

  try {
    const res = await fetch('/api/dispense-schedule', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });

    const duration = performance.now() - startTime;
    const result = await res.json();

    if (res.ok) {
      logger.info('API', `Configuration saved successfully (${duration.toFixed(2)}ms)`);
      showAlert('✅ Configuration saved and synced to ESP32!');
      fetchCurrentConfig();
    } else {
      logger.error('API', `Failed to save config: HTTP ${res.status}`);
      logger.debug('API', 'Error response: ' + JSON.stringify(result));
      showAlert('❌ Failed to save configuration: ' + result.message, true);
    }
  } catch (err) {
    logger.error('API', 'Network error: ' + err.message);
    showAlert('❌ Network error saving configuration', true);
  }
});
```

### Add Debug Mode Toggle

```javascript
// Add this function to enable/disable debug mode from console
window.setDebugMode = function(enable) {
  logger.enableDebugMode(enable);
  console.log(`Debug mode is now ${enable ? 'ON' : 'OFF'}`);
};

// Usage in browser console:
// setDebugMode(true)   - Enable debug logging
// setDebugMode(false)  - Disable debug logging
```

---

## 2. BACKEND LOGGING IMPROVEMENTS

### Add Python Logging Configuration (app.py)

```python
# Add at the top of app.py (after imports, around line 15)

import logging
import logging.handlers
import uuid
from functools import wraps

# Configure logging
LOG_DIR = os.path.join(os.path.dirname(__file__), "logs")
os.makedirs(LOG_DIR, exist_ok=True)

# Create logger
logger = logging.getLogger('mediminder')
logger.setLevel(logging.DEBUG)

# File handler with rotation
file_handler = logging.handlers.RotatingFileHandler(
    os.path.join(LOG_DIR, 'mediminder.log'),
    maxBytes=10485760,  # 10MB
    backupCount=5
)
file_handler.setLevel(logging.DEBUG)

# Console handler
console_handler = logging.StreamHandler()
console_handler.setLevel(logging.INFO)

# Formatter
formatter = logging.Formatter(
    '%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
file_handler.setFormatter(formatter)
console_handler.setFormatter(formatter)

logger.addHandler(file_handler)
logger.addHandler(console_handler)

# Request ID middleware
def get_request_id():
    """Generate or retrieve request ID for correlation"""
    if not hasattr(get_request_id, 'request_id'):
        get_request_id.request_id = str(uuid.uuid4())[:8]
    return get_request_id.request_id

def log_with_request_id(level, message):
    """Log message with request ID"""
    request_id = get_request_id()
    getattr(logger, level)(f"[{request_id}] {message}")
```

### Replace Existing Logging Hooks

```python
# Replace the existing @app.before_request and @app.after_request

@app.before_request
def log_incoming_request():
    """Log all incoming HTTP requests with request ID"""
    if request.path.startswith("/static"):
        return
    
    request_id = str(uuid.uuid4())[:8]
    get_request_id.request_id = request_id
    
    now_str = datetime.now(PHT).strftime("%Y-%m-%d %H:%M:%S PHT")
    logger.info(f"[{request_id}] >>> {request.method} {request.path} from {request.remote_addr} at {now_str}")
    
    if request.is_json and request.get_json(silent=True):
        logger.debug(f"[{request_id}] Payload: {request.get_json(silent=True)}")

@app.after_request
def log_outgoing_response(response):
    """Log response status code with timing"""
    if not request.path.startswith("/static"):
        request_id = get_request_id()
        status_emoji = "✅" if response.status_code < 400 else "❌"
        logger.info(f"[{request_id}] <<< {status_emoji} HTTP {response.status_code} {request.method} {request.path}")
    return response
```

### Update GET Dispense Endpoint

```python
# Replace the print statements in get_dispense_schedule (around line 278)

@app.route("/api/dispense", methods=["GET"])
@app.route("/api/dispense-schedule", methods=["GET"])
@app.route("/api/schedule", methods=["GET"])
def get_dispense_schedule():
    """GET endpoint to fetch schedule and RTC time"""
    request_id = get_request_id()
    logger.info(f"[{request_id}] Processing GET /api/dispense")
    
    rtc_time = get_rtc_current_time()
    
    # ... existing code ...
    
    # Replace print statements with:
    logger.info(f"[{request_id}] [DATA SERVED TO CLIENT / ESP32]")
    logger.debug(f"[{request_id}] Patient: {response_data['patient_name']}")
    logger.debug(f"[{request_id}] Active Rack: #{response_data['current_rack']}")
    logger.debug(f"[{request_id}] Rotation: {response_data['rotation_degree']}°")
    logger.debug(f"[{request_id}] Racks Available: {response_data['rack_count']}")
    
    if response_data['rack_warning']:
        logger.warning(f"[{request_id}] LOW RACK WARNING: Only {response_data['rack_count']} racks remaining")
    
    return jsonify(response_data), 200
```

### Update POST Dispense Log Endpoint

```python
# Replace print statements in submit_dispensation (around line 384)

@app.route("/api/dispense", methods=["POST"])
@app.route("/api/dispense-log", methods=["POST"])
def submit_dispensation():
    """POST endpoint for dispensation submission"""
    request_id = get_request_id()
    start_time = time.time()
    logger.info(f"[{request_id}] Processing POST /api/dispense-log")
    
    # ... existing validation code ...
    
    # Replace print statements with:
    logger.info(f"[{request_id}] [ACTION: DISPENSATION RECORDED]")
    logger.debug(f"[{request_id}] Event ID: #{log_entry['id']}")
    logger.debug(f"[{request_id}] Rack: #{slot}")
    logger.debug(f"[{request_id}] Patient: {patient_name}")
    logger.debug(f"[{request_id}] Next Rack: #{dispenser_config.get('current_rack')}")
    logger.debug(f"[{request_id}] Rotation: {rotation_degree}°")
    logger.debug(f"[{request_id}] Racks Remaining: {rack_count}")
    
    if rack_warning:
        logger.warning(f"[{request_id}] LOW RACK WARNING: {rack_count} racks remaining (threshold: {threshold})")
    
    duration = time.time() - start_time
    logger.info(f"[{request_id}] Dispensation recorded in {duration:.3f}s")
    
    return jsonify({...}), 201
```

### Add Error Logging

```python
# Add error handler (around line 690)

@app.errorhandler(Exception)
def handle_exception(error):
    """Log all unhandled exceptions"""
    request_id = get_request_id()
    logger.error(f"[{request_id}] Unhandled exception: {str(error)}", exc_info=True)
    return jsonify({
        "status": "error",
        "message": "Internal server error",
        "request_id": request_id
    }), 500
```

### Add Performance Monitoring

```python
# Add this decorator (around line 40)

def log_performance(f):
    """Decorator to log function execution time"""
    @wraps(f)
    def decorated_function(*args, **kwargs):
        request_id = get_request_id()
        start_time = time.time()
        result = f(*args, **kwargs)
        duration = time.time() - start_time
        logger.debug(f"[{request_id}] {f.__name__} took {duration:.3f}s")
        return result
    return decorated_function

# Usage:
@app.route("/api/dispense", methods=["GET"])
@log_performance
def get_dispense_schedule():
    # ... existing code ...
```

---

## 3. IOT FIRMWARE LOGGING IMPROVEMENTS

### Add Performance Logging

```cpp
// Add after logError function (around line 204)

void logPerformance(const char* tag, unsigned long startMs) {
  unsigned long duration = millis() - startMs;
  printTimestamp();
  serialPrintf("  " ANSI_CYAN "[PERF]" ANSI_RESET " [%-9s] %lu ms\n", tag, duration);
}

void logDebug(const char* tag, const char* msg) {
  if (DEBUG_MODE) {
    printTimestamp();
    serialPrintf(ANSI_BOLD ANSI_WHITE "[DEBUG] " ANSI_RESET "[%-9s] %s\n", tag, msg);
  }
}
```

### Add Debug Mode Flag

```cpp
// Add near the top with other defines (around line 78)

#define DEBUG_MODE false  // Set to true for verbose logging
```

### Add Hardware Monitoring

```cpp
// Add new function (around line 400)

void logHardwareStatus() {
  if (!DEBUG_MODE) return;
  
  printTimestamp();
  Serial.println(F("\n" ANSI_BOLD ANSI_CYAN "=== HARDWARE STATUS ===" ANSI_RESET));
  
#if defined(ESP32)
  serialPrintf("  Free Heap    : %d bytes\n", ESP.getFreeHeap());
  serialPrintf("  WiFi Signal  : %d dBm\n", WiFi.RSSI());
  serialPrintf("  Uptime       : %lu seconds\n", millis() / 1000);
#endif
  
  Serial.println();
}
```

### Add Sonar Continuous Logging

```cpp
// Replace sonarFiltered function (around line 371)

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
  
  if (DEBUG_MODE) {
    logDebug("SONAR", ("Distance: " + String(result) + " cm").c_str());
  }
  
  return result;
}
```

### Add Operation Timing

```cpp
// Replace rotateDegrees function (around line 322)

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
  
  logPerformance("STEPPER", startMs);
}
```

---

## 4. IMPLEMENTATION CHECKLIST

### Frontend (2-3 hours)
- [ ] Add Logger class
- [ ] Update fetchCurrentConfig with logging
- [ ] Update form submission with logging
- [ ] Add debug mode toggle
- [ ] Test console logging in browser
- [ ] Verify timestamps are correct
- [ ] Test error logging

### Backend (2-3 hours)
- [ ] Add Python logging configuration
- [ ] Create logs directory
- [ ] Update request/response hooks
- [ ] Add request ID generation
- [ ] Update GET /api/dispense endpoint
- [ ] Update POST /api/dispense-log endpoint
- [ ] Update POST /api/intake endpoint
- [ ] Add error handler
- [ ] Test log file creation
- [ ] Verify log rotation works

### IoT Firmware (1-2 hours)
- [ ] Add performance logging function
- [ ] Add debug mode flag
- [ ] Add hardware status logging
- [ ] Add sonar continuous logging
- [ ] Add operation timing
- [ ] Test serial output
- [ ] Verify performance metrics

### Testing (1-2 hours)
- [ ] Test frontend logging with browser console
- [ ] Test backend logging with log file
- [ ] Test IoT logging with serial monitor
- [ ] Verify request IDs correlate across components
- [ ] Test debug mode enable/disable
- [ ] Verify log rotation works
- [ ] Check for performance impact

---

## 5. TESTING COMMANDS

### Frontend Testing
```javascript
// In browser console:
setDebugMode(true);
logger.info('TEST', 'This is an info message');
logger.warn('TEST', 'This is a warning message');
logger.error('TEST', 'This is an error message');
logger.debug('TEST', 'This is a debug message');
```

### Backend Testing
```bash
# Check logs
tail -f server/logs/mediminder.log

# Search for errors
grep ERROR server/logs/mediminder.log

# Search by request ID
grep "a1b2c3d4" server/logs/mediminder.log

# Count log entries
wc -l server/logs/mediminder.log
```

### IoT Testing
```
Serial Monitor (9600 baud):
- Watch for [PERF] entries
- Look for [DEBUG] entries when DEBUG_MODE=true
- Monitor [WARN] entries for low rack warnings
```

---

## 6. PRODUCTION DEPLOYMENT CHECKLIST

Before deploying to production:

- [ ] Enable persistent logging on backend
- [ ] Set DEBUG_MODE=false on IoT firmware
- [ ] Configure log rotation on backend
- [ ] Set up log file backups
- [ ] Configure centralized logging (optional)
- [ ] Test error handling and logging
- [ ] Verify no sensitive data in logs
- [ ] Set up log monitoring/alerts
- [ ] Document log file locations
- [ ] Create log analysis scripts

---

## 7. MONITORING & MAINTENANCE

### Weekly Tasks
- [ ] Review error logs for patterns
- [ ] Check log file sizes
- [ ] Verify log rotation is working
- [ ] Check for performance issues

### Monthly Tasks
- [ ] Archive old logs
- [ ] Analyze trends in logs
- [ ] Update logging configuration if needed
- [ ] Review and optimize logging

### Quarterly Tasks
- [ ] Implement new logging features
- [ ] Upgrade logging infrastructure
- [ ] Review and update logging standards
- [ ] Train team on logging practices

---

**Estimated Total Implementation Time: 20-25 hours**

For detailed analysis, see: LOGGING_ANALYSIS.md
For summary, see: LOGGING_SUMMARY.txt
