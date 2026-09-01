"""
Mediminder by SJNHS - Flask API Backend
Smart Medicine Dispenser System
Controls sequential 7-rack date & time drug dispensing, rotation degrees, patient configuration,
ESP32 RTC date/time synchronization (Philippine Standard Time UTC+8), dispensation event logging,
and loaded storage rack count tracking with security enhancements.
"""

from datetime import datetime, timezone, timedelta
from typing import Any, Dict, List
from flask import Flask, jsonify, render_template, request
from flask_cors import CORS
import json
import re
import os
import logging
import logging.handlers
import time
import uuid

# ==========================================
# Persistent Logging Configuration
# ==========================================
LOGS_DIR = os.path.join(os.path.dirname(__file__), "logs")
os.makedirs(LOGS_DIR, exist_ok=True)

app_logger = logging.getLogger("mediminder")
app_logger.setLevel(logging.DEBUG)

# File handler with rotation (10 MB, keep 5 backups)
file_handler = logging.handlers.RotatingFileHandler(
    os.path.join(LOGS_DIR, "mediminder.log"),
    maxBytes=10485760,
    backupCount=5,
    encoding="utf-8"
)
file_handler.setLevel(logging.DEBUG)

# Console handler
console_handler = logging.StreamHandler()
console_handler.setLevel(logging.INFO)

formatter = logging.Formatter(
    "%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)
file_handler.setFormatter(formatter)
console_handler.setFormatter(formatter)

app_logger.addHandler(file_handler)
app_logger.addHandler(console_handler)

app = Flask(__name__)

# Security: Configure CORS with specific origins in production
# For development, allow all origins. In production, replace with specific ESP32 IP or domain
CORS(app, resources={
    r"/api/*": {
        "origins": "*",  # TODO: Replace with specific origins in production
        "methods": ["GET", "POST", "PUT", "DELETE", "OPTIONS"],
        "allow_headers": ["Content-Type"]
    }
})

# Security: Disable debug mode in production
app.config['DEBUG'] = False  # Set to False in production
app.config['JSON_SORT_KEYS'] = False

# Philippine Standard Time (PHT = UTC+8)
PHT = timezone(timedelta(hours=8))

# ==========================================
# Security & Validation Helpers
# ==========================================

def sanitize_string(value: str, max_length: int = 100) -> str:
    """Sanitize string input to prevent injection attacks."""
    if not isinstance(value, str):
        return ""
    # Remove any control characters and limit length
    sanitized = re.sub(r'[\x00-\x1f\x7f-\x9f]', '', value)
    return sanitized[:max_length].strip()

def validate_datetime_format(dt_str: str) -> bool:
    """Validate datetime string format (YYYY-MM-DD HH:MM)."""
    pattern = r'^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$'
    return bool(re.match(pattern, dt_str))

def validate_rack_id(rack_id: Any) -> bool:
    """Validate rack ID is within acceptable range (1-7)."""
    try:
        rid = int(rack_id)
        return 1 <= rid <= 7
    except (ValueError, TypeError):
        return False

def rate_limit_check(request_key: str) -> bool:
    """
    Simple rate limiting check (placeholder for production implementation).
    In production, implement proper rate limiting using Redis or similar.
    """
    # TODO: Implement proper rate limiting in production
    return True


def init_default_racks() -> List[Dict[str, Any]]:
    """Initialize default date and time specific schedules for all 7 storage racks."""
    now = datetime.now(PHT)
    times = ["08:00", "12:30", "18:00", "21:00"]
    racks = []
    for i in range(1, 8):
        day_offset = (i - 1) // 4
        time_idx = (i - 1) % 4
        target_date = (now + timedelta(days=day_offset)).strftime("%Y-%m-%d")
        t_str = times[time_idx]
        racks.append({
            "rack_id": i,
            "datetime": f"{target_date} {t_str}",
            "iso_datetime": f"{target_date}T{t_str}:00+08:00",
            "status": "pending",
            "notes": f"Rack {i} Medication Dose"
        })
    return racks


# ==========================================
# Simple text-file data persistence (one user)
# ==========================================
DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
DATA_FILE = os.path.join(DATA_DIR, "data.json")


def ensure_data_dir() -> None:
    """Ensure the data directory exists."""
    os.makedirs(DATA_DIR, exist_ok=True)


def load_data() -> Dict[str, Any]:
    """Load dispenser configuration and logs from a single JSON text file."""
    ensure_data_dir()
    if os.path.exists(DATA_FILE):
        try:
            with open(DATA_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            return data
        except (json.JSONDecodeError, IOError, PermissionError) as e:
            app_logger.error(f"Could not read {DATA_FILE}: {e}. Using defaults.")
    return {}


def save_data() -> None:
    """Persist dispenser configuration and logs to a single JSON text file."""
    ensure_data_dir()
    payload = {
        "config": dispenser_config,
        "dispense_logs": dispense_logs,
        "intake_logs": intake_logs,
    }
    temp_file = DATA_FILE + ".tmp"
    try:
        with open(temp_file, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
        os.replace(temp_file, DATA_FILE)
    except IOError as e:
        app_logger.error(f"Could not save {DATA_FILE}: {e}")


def migrate_default_data(data: Dict[str, Any]) -> None:
    """Ensure loaded data has all required keys with sensible defaults."""
    cfg = data.get("config", {})
    if not cfg.get("racks"):
        cfg["racks"] = init_default_racks()
    cfg.setdefault("patient_name", "Test Patient")
    cfg.setdefault("rotation_degree", 52)
    cfg.setdefault("current_rack", 1)
    cfg.setdefault("rack_count", 7)
    cfg.setdefault("rack_warning_threshold", 3)
    cfg.setdefault("dispense_time", ["08:00", "12:30", "18:00", "21:00"])
    cfg.setdefault("last_updated_at", "")
    data["config"] = cfg
    data.setdefault("dispense_logs", [])
    data.setdefault("intake_logs", [])


# Load data from text file or initialize defaults
_data = load_data()
migrate_default_data(_data)

dispenser_config: Dict[str, Any] = _data["config"]
dispense_logs: List[Dict[str, Any]] = _data["dispense_logs"]
intake_logs: List[Dict[str, Any]] = _data["intake_logs"]

# In-memory IoT sync state (last time the ESP32 fetched config)
iot_last_fetch_at: str | None = None

# Persist default data on first run
if not os.path.exists(DATA_FILE):
    save_data()


def get_rtc_current_time() -> Dict[str, Any]:
    """
    Generate detailed current time and date representation in Philippine Standard Time (PHT / UTC+8)
    for synchronizing the ESP32 RTC (Real Time Clock) and internal timekeeping.
    """
    now_pht = datetime.now(PHT)
    now_utc = datetime.now(timezone.utc)
    return {
        "iso": now_pht.isoformat(),
        "utc_iso": now_utc.isoformat(),
        "timezone": "PHT (UTC+8)",
        "unix_timestamp": int(now_pht.timestamp()),
        "formatted": now_pht.strftime("%Y-%m-%d %H:%M:%S PHT"),
        "datetime_str": now_pht.strftime("%Y-%m-%d %H:%M"),
        "date_str": now_pht.strftime("%Y-%m-%d"),
        "time_str": now_pht.strftime("%H:%M:%S"),
        "year": now_pht.year,
        "month": now_pht.month,
        "day": now_pht.day,
        "hour": now_pht.hour,
        "minute": now_pht.minute,
        "second": now_pht.second,
        "day_of_week": now_pht.weekday(),  # 0 = Monday, 6 = Sunday
    }


# ==========================================
# Security Headers & Middleware
# ==========================================

@app.after_request
def set_security_headers(response):
    """Add security headers to all responses."""
    response.headers['X-Content-Type-Options'] = 'nosniff'
    response.headers['X-Frame-Options'] = 'DENY'
    response.headers['X-XSS-Protection'] = '1; mode=block'
    response.headers['Referrer-Policy'] = 'strict-origin-when-cross-origin'
    response.headers['Permissions-Policy'] = 'geolocation=(), microphone=(), camera=()'
    return response

# ==========================================
# Comprehensive Activity & Debug Logging Hooks
# ==========================================

def get_request_id():
    """Return the current request ID (generated at request start)."""
    return getattr(request, "_request_id", "none")


@app.before_request
def log_incoming_request():
    """Log all incoming HTTP requests with request ID, client IP, method, and endpoint."""
    if request.path.startswith("/static"):
        return
    request._request_id = str(uuid.uuid4())[:8]
    request._start_time = time.time()
    payload = request.get_json(silent=True) if request.is_json else None
    app_logger.info(
        f"[{get_request_id()}] >>> {request.method} {request.path} from {request.remote_addr}"
    )
    if payload:
        app_logger.debug(f"[{get_request_id()}] Payload: {json.dumps(payload)}")


@app.after_request
def log_outgoing_response(response):
    """Log response status code, duration, and request ID."""
    if not request.path.startswith("/static"):
        request_id = get_request_id()
        duration = (time.time() - request._start_time) * 1000 if hasattr(request, "_start_time") else 0
        level = logging.INFO if response.status_code < 400 else logging.WARNING
        app_logger.log(
            level,
            f"[{request_id}] <<< HTTP {response.status_code} {request.method} {request.path} ({duration:.1f}ms)"
        )
    return response


# ==========================================
# 1. GET Endpoints: Fetch Dispense Schedule & RTC Time
# ==========================================

@app.route("/api/dispense", methods=["GET"])
@app.route("/api/dispense-schedule", methods=["GET"])
@app.route("/api/schedule", methods=["GET"])
def get_dispense_schedule():
    """
    GET endpoint to fetch the sequential 7-rack date & time schedule, active rack,
    rotation degrees, current Philippine Time (PHT / UTC+8) for RTC sync, patient name,
    and storage rack counts (default 7 racks).
    """
    rtc_time = get_rtc_current_time()

    # Track the last time the physical ESP32 fetched the schedule
    if request.args.get("device") == "esp32":
        global iot_last_fetch_at
        iot_last_fetch_at = rtc_time["iso"]
        app_logger.info(f"[{get_request_id()}] [IOT FETCH] ESP32 polled schedule at {iot_last_fetch_at}")

    # Guarantee 7 storage racks by default
    racks = dispenser_config.get("racks", [])
    if not racks or len(racks) < 7:
        dispenser_config["racks"] = init_default_racks()
        racks = dispenser_config["racks"]

    current_rack = dispenser_config.get("current_rack", 1)

    # Compute remaining pending storage racks (default max: 7)
    pending_racks = [r for r in racks if r.get("status") == "pending"]
    rack_count = len(pending_racks)
    dispenser_config["rack_count"] = rack_count
    threshold = dispenser_config.get("rack_warning_threshold", 3)

    # Determine next upcoming rack in sequence (1 to 7)
    next_rack_obj = next((r for r in racks if r["rack_id"] == current_rack and r.get("status") == "pending"), None)
    if not next_rack_obj and pending_racks:
        next_rack_obj = pending_racks[0]
        dispenser_config["current_rack"] = next_rack_obj["rack_id"]

    response_data = {
        "status": "success",
        "patient_name": dispenser_config.get("patient_name", "Test Patient"),
        "rotation_degree": dispenser_config.get("rotation_degree", 52),
        "total_racks": 7,  # Default 7 storage racks
        "current_rack": dispenser_config.get("current_rack", 1),
        "next_rack": next_rack_obj,
        "next_rack_id": next_rack_obj["rack_id"] if next_rack_obj else None,
        "next_rack_datetime": next_rack_obj["datetime"] if next_rack_obj else None,
        "next_rack_iso": next_rack_obj["iso_datetime"] if next_rack_obj else None,
        "rack_count": rack_count,
        "rack_warning_threshold": threshold,
        "rack_warning": rack_count <= threshold,
        "racks": racks,
        "dispense_time": [r["datetime"].split(" ")[-1] for r in racks if "datetime" in r],
        "current_time": rtc_time,
    }

    request_id = get_request_id()
    app_logger.info(f"[{request_id}] [DATA SERVED TO CLIENT / ESP32]")
    app_logger.debug(f"[{request_id}] Patient: {response_data['patient_name']}")
    app_logger.debug(f"[{request_id}] Active Next Rack: Rack #{response_data['current_rack']} due at {response_data['next_rack_datetime']}")
    app_logger.debug(f"[{request_id}] Rotation Degree: {response_data['rotation_degree']}°")
    app_logger.debug(f"[{request_id}] Storage Racks: {response_data['rack_count']} available (threshold: {response_data['rack_warning_threshold']})")
    if response_data['rack_warning']:
        app_logger.warning(f"[{request_id}] Rack Warning Active: YES (LOW CAPACITY)")
    app_logger.debug(f"[{request_id}] Current PHT Time: {rtc_time['formatted']} (ISO: {rtc_time['iso']})")

    return jsonify(response_data), 200


# ==========================================
# 2. POST Endpoints: Submit Successful Medicine Dispensation
# ==========================================

@app.route("/api/dispense", methods=["POST"])
@app.route("/api/dispense-log", methods=["POST"])
@app.route("/api/dispense/complete", methods=["POST"])
def submit_dispensation():
    """
    POST endpoint for the submission of successful medicine dispensation.
    Marks the active rack as dispensed, advances the sequence pointer, and decrements rack count.
    Security: Input validation and sanitization applied.
    """
    # Rate limiting check
    if not rate_limit_check(f"dispense_{request.remote_addr}"):
        return jsonify({"status": "error", "message": "Rate limit exceeded"}), 429
    
    data = request.get_json(silent=True) or {}

    # Validate and sanitize timestamp
    timestamp = data.get("timestamp")
    if timestamp:
        timestamp = sanitize_string(timestamp, 50)
    if not timestamp:
        timestamp = datetime.now(PHT).isoformat()

    # Sanitize inputs
    status = sanitize_string(data.get("status", "success"), 20)
    patient_name = sanitize_string(data.get("patient_name", dispenser_config["patient_name"]), 100)
    
    # Validate rotation degree
    try:
        rotation_degree = float(data.get("rotation_degree", dispenser_config["rotation_degree"]))
        if rotation_degree <= 0 or rotation_degree > 360:
            rotation_degree = dispenser_config["rotation_degree"]
    except (ValueError, TypeError):
        rotation_degree = dispenser_config["rotation_degree"]

    current_rack = dispenser_config.get("current_rack", 1)
    slot = data.get("slot") or data.get("rack_id") or current_rack
    
    # Validate rack ID
    if not validate_rack_id(slot):
        return jsonify({"status": "error", "message": "Invalid rack ID. Must be between 1 and 7."}), 400
    
    try:
        slot = int(slot)
    except (ValueError, TypeError):
        slot = current_rack

    notes = sanitize_string(data.get("notes", f"Rack #{slot} dispensed successfully via ESP32"), 200)

    # Mark the specific rack in the sequence as dispensed
    racks = dispenser_config.get("racks", [])
    for r in racks:
        if r["rack_id"] == slot:
            r["status"] = "dispensed"
            r["dispensed_at"] = timestamp
            break

    # Advance current_rack pointer to the next pending rack in sequence
    pending_after = [r for r in racks if r.get("status") == "pending" and r["rack_id"] > slot]
    if pending_after:
        dispenser_config["current_rack"] = pending_after[0]["rack_id"]
    else:
        # Wrap or find any remaining pending rack
        any_pending = [r for r in racks if r.get("status") == "pending"]
        dispenser_config["current_rack"] = any_pending[0]["rack_id"] if any_pending else None

    # Count remaining available storage racks
    rack_count = len([r for r in racks if r.get("status") == "pending"])
    dispenser_config["rack_count"] = rack_count
    threshold = dispenser_config["rack_warning_threshold"]
    rack_warning = rack_count <= threshold

    log_entry = {
        "id": len(dispense_logs) + 1,
        "timestamp": timestamp,
        "recorded_at": datetime.now(PHT).isoformat(),
        "status": status,
        "patient_name": patient_name,
        "rotation_degree": rotation_degree,
        "rack_id": slot,
        "slot": slot,
        "notes": notes,
        "next_rack": dispenser_config.get("current_rack"),
        "rack_count_after": rack_count,
        "rack_warning": rack_warning,
    }

    dispense_logs.append(log_entry)
    save_data()

    request_id = get_request_id()
    app_logger.info(f"[{request_id}] [ACTION: DISPENSATION RECORDED]")
    app_logger.debug(f"[{request_id}] Event ID: #{log_entry['id']}")
    app_logger.debug(f"[{request_id}] Timestamp (PHT): {log_entry['timestamp']}")
    app_logger.debug(f"[{request_id}] Patient: {patient_name}")
    app_logger.debug(f"[{request_id}] Rack Dispensed: Rack #{slot}")
    app_logger.debug(f"[{request_id}] Next Rack Due: Rack #{dispenser_config.get('current_rack')}")
    app_logger.debug(f"[{request_id}] Stepper Rotation: {rotation_degree}°")
    app_logger.debug(f"[{request_id}] Racks Remaining: {rack_count} available")
    if rack_warning:
        app_logger.warning(f"[{request_id}] Storage racks low (≤ {threshold})! Refill recommended.")

    return jsonify({
        "status": "success",
        "message": f"Rack #{slot} dispensation recorded successfully.",
        "log": log_entry,
        "dispensed_rack": slot,
        "next_rack": dispenser_config.get("current_rack"),
        "rack_count": rack_count,
        "rack_warning": rack_warning,
        "total_logs": len(dispense_logs)
    }), 201


# ==========================================
# 3. POST: Patient Intake Acknowledgement (Drawer Opened)
# ==========================================

@app.route("/api/intake", methods=["POST"])
@app.route("/api/intake-log", methods=["POST"])
def submit_intake():
    """
    POST endpoint submitted by ESP32 when sonar detects the patient opening
    the drawer / taking the medication cup. Signals confirmed medication intake.
    Security: Input validation and sanitization applied.
    """
    # Rate limiting check
    if not rate_limit_check(f"intake_{request.remote_addr}"):
        return jsonify({"status": "error", "message": "Rate limit exceeded"}), 429
    
    data = request.get_json(silent=True) or {}

    # Validate and sanitize timestamp
    timestamp = data.get("timestamp")
    if timestamp:
        timestamp = sanitize_string(timestamp, 50)
    if not timestamp:
        timestamp = datetime.now(PHT).isoformat()

    # Sanitize inputs
    patient_name = sanitize_string(data.get("patient_name", dispenser_config["patient_name"]), 100)
    slot = data.get("slot") or data.get("rack_id") or dispenser_config.get("current_rack", 1)
    
    # Validate rack ID
    if not validate_rack_id(slot):
        slot = dispenser_config.get("current_rack", 1)
    
    notes = sanitize_string(data.get("notes", "Patient retrieved medication from dispenser tray"), 200)

    intake_entry = {
        "id": len(intake_logs) + 1,
        "timestamp": timestamp,
        "recorded_at": datetime.now(PHT).isoformat(),
        "patient_name": patient_name,
        "rack_id": slot,
        "slot": slot,
        "notes": notes,
        "status": "intake_confirmed",
    }

    intake_logs.append(intake_entry)
    save_data()

    request_id = get_request_id()
    app_logger.info(f"[{request_id}] [ACTION: INTAKE CONFIRMED / DRAWER OPENED]")
    app_logger.debug(f"[{request_id}] Intake ID: #{intake_entry['id']}")
    app_logger.debug(f"[{request_id}] Timestamp (PHT): {intake_entry['timestamp']}")
    app_logger.debug(f"[{request_id}] Patient: {patient_name}")
    app_logger.debug(f"[{request_id}] Rack Confirmed: Rack #{slot}")
    app_logger.debug(f"[{request_id}] Notes: {notes}")

    return jsonify({
        "status": "success",
        "message": "Patient intake event recorded successfully.",
        "log": intake_entry,
        "total_intake_logs": len(intake_logs)
    }), 201


# ==========================================
# 4. Configuration & Management Endpoints (for Website UI)
# ==========================================

@app.route("/api/dispense-schedule", methods=["POST", "PUT"])
@app.route("/api/configure", methods=["POST"])
def update_dispense_schedule():
    """
    Update dispenser configuration (patient_name, 7-rack date & time schedule,
    active current_rack sequence pointer, rotation_degree, rack_warning_threshold).
    Security: Enhanced input validation and sanitization.
    """
    # Rate limiting check
    if not rate_limit_check(f"config_{request.remote_addr}"):
        return jsonify({"status": "error", "message": "Rate limit exceeded"}), 429
    
    data = request.get_json(silent=True)
    if not data:
        return jsonify({"status": "error", "message": "Invalid JSON payload"}), 400

    if "patient_name" in data:
        if not isinstance(data["patient_name"], str) or not data["patient_name"].strip():
            return jsonify({"status": "error", "message": "patient_name must be a non-empty string"}), 400
        # Sanitize patient name
        sanitized_name = sanitize_string(data["patient_name"], 100)
        if not sanitized_name:
            return jsonify({"status": "error", "message": "patient_name contains invalid characters"}), 400
        dispenser_config["patient_name"] = sanitized_name

    if "rotation_degree" in data:
        try:
            deg = float(data["rotation_degree"])
            if deg <= 0 or deg > 360:
                return jsonify({"status": "error", "message": "rotation_degree must be between 1 and 360"}), 400
            dispenser_config["rotation_degree"] = int(deg) if deg.is_integer() else deg
        except (ValueError, TypeError):
            return jsonify({"status": "error", "message": "rotation_degree must be a valid number"}), 400

    if "current_rack" in data:
        try:
            cr = int(data["current_rack"])
            if 1 <= cr <= 7:
                dispenser_config["current_rack"] = cr
        except (ValueError, TypeError):
            pass

    if "rack_warning_threshold" in data:
        try:
            thresh = int(data["rack_warning_threshold"])
            if thresh >= 0:
                dispenser_config["rack_warning_threshold"] = thresh
        except (ValueError, TypeError):
            pass

    # Update 7-rack date and time schedule with validation
    if "racks" in data and isinstance(data["racks"], list):
        # Limit to maximum 7 racks
        if len(data["racks"]) > 7:
            return jsonify({"status": "error", "message": "Maximum 7 racks allowed"}), 400
        
        updated_racks = []
        for item in data["racks"]:
            # Validate rack_id
            rack_id = item.get("rack_id", len(updated_racks) + 1)
            if not validate_rack_id(rack_id):
                return jsonify({"status": "error", "message": f"Invalid rack_id: {rack_id}. Must be between 1 and 7."}), 400
            
            rack_id = int(rack_id)
            dt_raw = str(item.get("datetime") or item.get("dispense_datetime") or "").strip()
            
            # Normalize to "YYYY-MM-DD HH:MM"
            dt_clean = dt_raw.replace("T", " ")
            if len(dt_clean) >= 16:
                dt_clean = dt_clean[:16]  # Truncate to YYYY-MM-DD HH:MM
            
            # Validate datetime format
            if dt_clean and not validate_datetime_format(dt_clean):
                return jsonify({"status": "error", "message": f"Invalid datetime format for rack {rack_id}. Expected: YYYY-MM-DD HH:MM"}), 400
            
            if len(dt_clean) == 16:
                iso_dt = dt_clean.replace(" ", "T") + ":00+08:00"
            else:
                iso_dt = dt_raw

            # Sanitize status and notes
            status = sanitize_string(item.get("status", "pending"), 20)
            if status not in ["pending", "dispensed"]:
                status = "pending"
            
            notes = sanitize_string(item.get("notes", f"Rack {rack_id}"), 200)
            
            updated_racks.append({
                "rack_id": rack_id,
                "datetime": dt_clean,
                "iso_datetime": iso_dt,
                "status": status,
                "notes": notes
            })
        
        dispenser_config["racks"] = updated_racks
        dispenser_config["rack_count"] = len([r for r in updated_racks if r["status"] == "pending"])
        dispenser_config["dispense_time"] = [r["datetime"].split(" ")[-1] for r in updated_racks if "datetime" in r]

    elif "dispense_time" in data and isinstance(data["dispense_time"], list):
        # Legacy time-only fallback: map to existing racks
        now = datetime.now(PHT)
        times = [str(t).strip() for t in data["dispense_time"] if str(t).strip()]
        for idx, r in enumerate(dispenser_config.get("racks", [])):
            if idx < len(times):
                t_str = times[idx]
                r_date = r["datetime"].split(" ")[0] if "datetime" in r else now.strftime("%Y-%m-%d")
                r["datetime"] = f"{r_date} {t_str}"
                r["iso_datetime"] = f"{r_date}T{t_str}:00+08:00"

    # Persist updated configuration to text file
    save_data()

    # Mark config as updated so the UI can tell when the ESP32 re-fetches
    now_pht = datetime.now(PHT).isoformat()
    dispenser_config["last_updated_at"] = now_pht
    save_data()

    request_id = get_request_id()
    app_logger.info(f"[{request_id}] [POST /api/dispense-schedule] 7-Rack Schedule updated via Save & Sync")
    app_logger.info(f"[{request_id}] [SYNC PENDING] Config updated at {now_pht}; waiting for ESP32 GET poll")
    app_logger.debug(f"[{request_id}] Patient Name: {dispenser_config['patient_name']}")
    app_logger.debug(f"[{request_id}] Active Next Rack: Rack #{dispenser_config.get('current_rack')}")
    app_logger.debug(f"[{request_id}] Rotation Degree: {dispenser_config['rotation_degree']}°")
    app_logger.debug(f"[{request_id}] Pending Racks: {dispenser_config['rack_count']} available")
    for r in dispenser_config.get("racks", []):
        app_logger.debug(f"[{request_id}]   - Rack #{r['rack_id']}: {r['datetime']} [{r['status'].upper()}]")

    return jsonify({
        "status": "success",
        "message": "7-Rack date and time schedule updated successfully.",
        "config": dispenser_config
    }), 200


@app.route("/api/dispense-logs", methods=["GET"])
def get_dispense_logs():
    """Fetch all recorded dispensation logs."""
    return jsonify({
        "status": "success",
        "count": len(dispense_logs),
        "logs": list(reversed(dispense_logs))  # Most recent first
    }), 200


@app.route("/api/intake-logs", methods=["GET"])
def get_intake_logs():
    """Fetch all recorded patient intake / drawer-open logs."""
    return jsonify({
        "status": "success",
        "count": len(intake_logs),
        "logs": list(reversed(intake_logs))  # Most recent first
    }), 200


@app.route("/api/dispense-logs", methods=["DELETE"])
def clear_dispense_logs():
    """Clear all dispensation logs (useful for testing)."""
    dispense_logs.clear()
    save_data()
    return jsonify({
        "status": "success",
        "message": "All dispensation logs cleared."
    }), 200


@app.route("/api/status", methods=["GET"])
def health_check():
    """Quick health & status check for IoT ESP32 connectivity."""
    rack_count = dispenser_config["rack_count"]
    threshold = dispenser_config["rack_warning_threshold"]
    return jsonify({
        "status": "online",
        "service": "IoT Medicine Dispenser API",
        "active_patient": dispenser_config["patient_name"],
        "current_rack": dispenser_config.get("current_rack", 1),
        "rack_count": rack_count,
        "rack_warning": rack_count <= threshold,
        "rack_warning_threshold": threshold,
        "current_time": get_rtc_current_time()["formatted"],
        "current_timestamp_iso": get_rtc_current_time()["iso"]
    }), 200


@app.route("/api/iot-sync-status", methods=["GET"])
def iot_sync_status():
    """Return whether the ESP32 has fetched the latest configuration and is still online."""
    last_updated_at = dispenser_config.get("last_updated_at", "")
    synced = (
        iot_last_fetch_at is not None and
        last_updated_at and
        iot_last_fetch_at >= last_updated_at
    )

    iot_online = False
    iot_last_seen_seconds_ago = None
    if iot_last_fetch_at:
        try:
            last_fetch = datetime.fromisoformat(iot_last_fetch_at)
            iot_last_seen_seconds_ago = int((datetime.now(PHT) - last_fetch).total_seconds())
            iot_online = iot_last_seen_seconds_ago <= 30  # 2x the 15s poll interval
        except ValueError:
            iot_last_seen_seconds_ago = None

    racks = dispenser_config.get("racks", [])
    current_rack = dispenser_config.get("current_rack", 1)
    next_rack_obj = next((r for r in racks if r["rack_id"] == current_rack and r.get("status") == "pending"), None)
    next_dose = next_rack_obj["datetime"] if next_rack_obj else None

    return jsonify({
        "status": "success",
        "last_updated_at": last_updated_at,
        "iot_last_fetch_at": iot_last_fetch_at,
        "iot_last_seen_seconds_ago": iot_last_seen_seconds_ago,
        "iot_online": iot_online,
        "synced": synced,
        "next_rack_id": next_rack_obj["rack_id"] if next_rack_obj else None,
        "next_dose": next_dose
    }), 200


# ==========================================
# 5. Web Dashboard UI
# ==========================================

@app.route("/")
def landing():
    """Render the landing page (hero, features carousel, rationale, CTA)."""
    return render_template("landing.html")


@app.route("/dashboard")
def dashboard():
    """Render the web configuration and monitoring dashboard."""
    return render_template("dashboard.html")


@app.errorhandler(Exception)
def handle_unhandled_exception(error):
    """Log all unhandled exceptions with request ID and stack trace."""
    request_id = get_request_id()
    app_logger.error(f"[{request_id}] Unhandled exception: {error}", exc_info=True)
    return jsonify({
        "status": "error",
        "message": "Internal server error",
        "request_id": request_id
    }), 500


if __name__ == "__main__":
    app_logger.info("="*70)
    app_logger.info("  MEDIMINDER by SJNHS - Smart Medicine Dispenser API Server")
    app_logger.info("="*70)
    app_logger.info("Starting Flask Server on port 5000...")
    app_logger.info("API Endpoints:")
    app_logger.info("  GET  /api/dispense          - Fetch 7-rack schedule, active rack & full PHT RTC timestamp")
    app_logger.info("  POST /api/dispense-log      - Submit successful dispense event (advances sequence pointer)")
    app_logger.info("  POST /api/intake            - Submit patient intake / drawer-open event")
    app_logger.info("  POST /api/dispense-schedule - Update 7-rack date & time schedule")
    app_logger.info("  GET  /api/dispense-logs     - View dispensation history")
    app_logger.info("  GET  /api/intake-logs       - View patient intake history")
    app_logger.info("  GET  /api/status            - Health check & system status")
    app_logger.info("  GET  /                      - Landing Page (hero, features, rationale)")
    app_logger.info("  GET  /dashboard             - Web Dashboard UI")
    app_logger.info("Security Features:")
    app_logger.info("  ✓ Input validation and sanitization")
    app_logger.info("  ✓ Rate limiting (placeholder - implement in production)")
    app_logger.info("  ✓ CORS configuration")
    app_logger.info("  ✓ Datetime format validation")
    
    # Security: In production, set debug=False and use a production WSGI server
    port = int(os.environ.get("PORT", 5000))
    app.run(host="0.0.0.0", port=port, debug=True)
