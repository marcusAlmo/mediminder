# Mediminder by SJNHS

**Smart Medicine Dispenser System** - An IoT-based automated medication dispensing solution with ESP32 integration.

## 🏥 Overview

Mediminder is an intelligent medicine dispensing system designed to help patients manage their medication schedules effectively. The system uses an ESP32 microcontroller to control a 7-rack sequential dispenser with real-time monitoring, automated alerts, and comprehensive logging.

## ✨ Features

### Hardware Features
- **7-Rack Sequential Dispensing**: Automated medication dispensing from 7 storage racks
- **Precise Stepper Motor Control**: 28BYJ-48 microstepper with ULN2003 driver
- **Servo-Controlled Pill Cover**: Automated opening/closing mechanism
- **Ultrasonic Drawer Detection**: HC-SR04 sensor confirms medication retrieval
- **LCD Display**: Real-time clock, patient info, and status updates
- **Audio-Visual Alerts**: Buzzer and LED light for medication reminders
- **RTC Synchronization**: Accurate timekeeping synchronized with server (PHT/UTC+8)

### Software Features
- **RESTful API Backend**: Flask-based server with comprehensive endpoints
- **Web Dashboard**: Modern, responsive UI for configuration and monitoring
- **Real-Time Monitoring**: Live dispensation and intake logging
- **Retry Logic**: Automatic retry (2 attempts, 5-second intervals) on API failures
- **Cached Fallback**: Continues operation using cached settings when offline
- **Input Validation**: Comprehensive security measures and data sanitization
- **Rate Limiting**: Protection against abuse (placeholder for production)

## 📁 Project Structure

```
mediminder/
├── iot/                          # ESP32 Firmware
│   └── mediminder_esp32.ino     # Main Arduino sketch
├── server/                       # Backend API Server
│   ├── app.py                   # Flask API with security enhancements
│   ├── requirements.txt         # Python dependencies
│   ├── templates/               # HTML templates
│   │   └── index.html          # Web dashboard UI
│   ├── data/                    # Text file storage (single user)
│   │   └── data.json           # Config and logs
│   ├── static/                  # Static assets
│   │   ├── manifest.json       # PWA manifest
│   │   ├── sw.js               # Service Worker
│   │   ├── offline.html        # Offline fallback
│   │   └── style.css           # Dashboard styling
│   └── templates/               # HTML templates
│       └── dashboard.html      # Web dashboard UI
└── README.md                    # This file
```

## 🔧 Hardware Configuration

### ESP32 Pinout
| Component | Pin | Notes |
|-----------|-----|-------|
| Stepper IN1 | GPIO 26 | ULN2003 driver |
| Stepper IN2 | GPIO 27 | ULN2003 driver |
| Stepper IN3 | GPIO 25 | ULN2003 driver |
| Stepper IN4 | GPIO 32 | ULN2003 driver |
| Sonar TRIG | GPIO 5 | HC-SR04 |
| Sonar ECHO | GPIO 18 | HC-SR04 (with voltage divider) |
| LCD SDA | GPIO 21 | I2C (0x27 address) |
| LCD SCL | GPIO 22 | I2C |
| Buzzer | GPIO 19 | Passive buzzer |
| LED Relay | GPIO 23 | External LED light |
| Servo | GPIO 13 | External power required |

## 🚀 Getting Started

### Prerequisites
- ESP32 development board
- Arduino IDE with ESP32 board support
- Python 3.8+
- Required hardware components (see Hardware Configuration)

### Server Setup

1. **Navigate to server directory**
   ```bash
   cd server
   ```

2. **Install dependencies**
   ```bash
   pip install -r requirements.txt
   ```

3. **Run the server**
   ```bash
   python app.py
   ```

4. **Access the dashboard**
   Open your browser to `http://localhost:5000`

### ESP32 Firmware Setup

1. **Open Arduino IDE**
   - Load `iot/mediminder_esp32.ino`

2. **Configure WiFi credentials**
   ```cpp
   const char* WIFI_SSID = "your_wifi_ssid";
   const char* WIFI_PASSWORD = "your_wifi_password";
   ```

3. **Set API server URL**
   ```cpp
   const char* API_BASE_URL = "http://your_server_ip:5000";
   ```

4. **Upload to ESP32**
   - Select board: ESP32 Dev Module
   - Upload speed: 115200
   - Flash and monitor via Serial (9600 baud)

## 📡 API Endpoints

### GET Endpoints
- `GET /api/dispense` - Fetch 7-rack schedule and RTC sync data
- `GET /api/dispense-logs` - View dispensation history
- `GET /api/intake-logs` - View patient intake history
- `GET /api/status` - Health check and system status

### POST Endpoints
- `POST /api/dispense-log` - Submit successful dispensation event
- `POST /api/intake` - Submit patient intake confirmation
- `POST /api/dispense-schedule` - Update 7-rack schedule configuration

## 🔒 Security Features

### Implemented
- ✅ Input validation and sanitization
- ✅ Datetime format validation
- ✅ Rack ID range validation (1-7)
- ✅ String length limits
- ✅ Control character filtering
- ✅ CORS configuration
- ✅ Rate limiting structure (placeholder)

### Production Recommendations
1. **Enable HTTPS**: Use SSL/TLS certificates
2. **Implement proper rate limiting**: Use Redis or similar
3. **Add authentication**: API keys or OAuth for ESP32
4. **Configure CORS**: Restrict to specific origins
5. **Use production WSGI server**: Gunicorn or uWSGI
6. **Set DEBUG=False**: Disable debug mode
7. **Environment variables**: Store secrets securely
8. **Database backend**: Replace in-memory storage with PostgreSQL/MySQL
9. **Logging**: Implement comprehensive logging system
10. **Monitoring**: Add health checks and alerting

## 🔄 Retry & Fallback Logic

The ESP32 firmware implements robust error handling:

1. **API Request Attempt**: Initial request to server
2. **Retry 1**: If failed, wait 5 seconds and retry
3. **Retry 2**: If failed again, wait 5 seconds and retry
4. **Fallback**: After all retries fail, use cached settings and data
5. **Continue Operation**: System continues with last known configuration

## 📊 Dispensing Workflow

1. **Polling**: ESP32 polls `/api/dispense` every 15 seconds
2. **Schedule Check**: Compares current time with rack schedules
3. **Dispense Trigger**: When time matches, initiates dispense cycle:
   - Rotate stepper motor (52° default)
   - Open servo cover (90°)
   - Hold for 5 seconds
   - Close servo cover (0°)
   - Activate buzzer and LED (10 seconds)
   - POST dispense log to server
4. **Patient Alert**: LCD shows "Medicine Ready" + patient name
5. **Repeat Alarm**: Every 60 seconds until medication retrieved
6. **Drawer Detection**: Sonar detects drawer opening
7. **Intake Confirmation**: POST intake log to server
8. **Reset**: Return to idle state

## 🎨 Web Dashboard Features

- **Real-time Clock**: Live PHT/UTC+8 time display
- **7-Rack Schedule Manager**: Configure date/time for each rack
- **Device Simulator**: Test ESP32 API interactions
- **Dispensation Logs**: View complete history
- **Status Preview**: Current configuration snapshot
- **Auto-Fill**: Quick 4-hour interval scheduling

## 🧪 Testing

### ESP32 Serial Commands
- `D` or `DISPENSE` - Manual immediate dispense
- `POLL` - Force API schedule poll
- `S` or `STATUS` - Show current status
- `HELP` - Display command menu

### Web Dashboard Simulator
Use the built-in simulator buttons to test:
- GET /api/dispense (fetch schedule)
- POST /api/dispense-log (simulate dispensation)
- POST /api/intake (simulate drawer opening)

## 📝 Configuration

### Default Settings
- **Rotation Degree**: 52° per rack
- **Rack Count**: 7 racks
- **Warning Threshold**: 3 racks remaining
- **Poll Interval**: 15 seconds
- **Retry Count**: 2 attempts
- **Retry Delay**: 5 seconds
- **Alarm Repeat**: 60 seconds

### Customization
All settings can be modified via:
- Web dashboard configuration panel
- POST /api/dispense-schedule endpoint
- ESP32 firmware constants (requires reflashing)

## 🐛 Troubleshooting

### ESP32 Won't Connect to WiFi
- Verify SSID and password
- Check WiFi signal strength
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)

### API Requests Failing
- Verify server is running on correct IP and port
- Check firewall settings
- Ensure ESP32 and server are on same network
- Monitor serial output for HTTP error codes

### Stepper Motor Not Rotating
- Check ULN2003 connections
- Verify power supply (5V for motor)
- Test with manual dispense command (`D`)

### LCD Not Displaying
- Verify I2C address (default 0x27)
- Check SDA/SCL connections
- Ensure 5V power to LCD

## 📄 License

This project is developed by SJNHS for educational and healthcare purposes.

## 👥 Contributors

Developed with ❤️ by SJNHS team for better healthcare management.

## 🔮 Future Enhancements

- [ ] Mobile app integration
- [ ] SMS/Email notifications
- [ ] Multi-patient support
- [ ] Cloud database integration
- [ ] Advanced analytics dashboard
- [ ] Voice alerts
- [ ] Biometric authentication
- [ ] Prescription integration
- [ ] Inventory management
- [ ] Caregiver portal

## 📞 Support

For issues, questions, or contributions, please contact the SJNHS development team.

---

**Mediminder by SJNHS** - Making medication management smarter and safer.
