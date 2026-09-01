# 🚀 Mediminder Quick Start Guide

Get your Mediminder system up and running in minutes!

## 📋 Prerequisites Checklist

### Hardware
- [ ] ESP32 development board
- [ ] 28BYJ-48 stepper motor with ULN2003 driver
- [ ] HC-SR04 ultrasonic sensor
- [ ] 16x2 I2C LCD display (0x27 address)
- [ ] SG90 or similar servo motor
- [ ] Passive buzzer
- [ ] 5V relay module (for LED)
- [ ] Jumper wires and breadboard
- [ ] 5V power supply (2A recommended)

### Software
- [ ] Arduino IDE (1.8.x or 2.x)
- [ ] Python 3.8 or higher
- [ ] pip (Python package manager)
- [ ] Git (optional, for version control)

## 🔧 Step 1: Hardware Assembly

### Wiring Diagram
```
ESP32 Pin Connections:
├── Stepper Motor (ULN2003)
│   ├── GPIO 26 → IN1
│   ├── GPIO 27 → IN2
│   ├── GPIO 25 → IN3
│   └── GPIO 32 → IN4
├── Ultrasonic Sensor (HC-SR04)
│   ├── GPIO 5  → TRIG
│   └── GPIO 18 → ECHO (use voltage divider: 3.3V safe)
├── LCD Display (I2C)
│   ├── GPIO 21 → SDA
│   └── GPIO 22 → SCL
├── Servo Motor
│   └── GPIO 13 → Signal (external 5V power!)
├── Buzzer
│   └── GPIO 19 → Signal
└── LED Relay
    └── GPIO 23 → IN/Signal

Power:
├── ESP32: 5V via USB or VIN
├── Stepper: 5V (from ULN2003 board)
├── LCD: 5V
├── Servo: 5V (separate power recommended)
└── All GND connected together
```

### Important Notes
⚠️ **Voltage Divider for HC-SR04 ECHO**: ESP32 is 3.3V, HC-SR04 outputs 5V
```
HC-SR04 ECHO → 1kΩ resistor → ESP32 GPIO 18
                            └→ 2kΩ resistor → GND
```

⚠️ **Servo External Power**: Servos draw high current, use separate 5V supply

## 💻 Step 2: Server Setup (5 minutes)

### Option A: Quick Setup
```bash
# Navigate to project directory
cd mediminder/server

# Install dependencies
pip install Flask flask-cors

# Run the server
python app.py
```

### Option B: Virtual Environment (Recommended)
```bash
# Create virtual environment
python -m venv venv

# Activate virtual environment
# On macOS/Linux:
source venv/bin/activate
# On Windows:
venv\Scripts\activate

# Install dependencies
pip install -r requirements.txt

# Run the server
python app.py
```

### Verify Server is Running
Open browser to: `http://localhost:5000`

You should see the Mediminder dashboard! 🎉

## 📱 Step 3: ESP32 Firmware Setup (10 minutes)

### Install Arduino IDE Libraries
1. Open Arduino IDE
2. Go to **Sketch → Include Library → Manage Libraries**
3. Install these libraries:
   - `LiquidCrystal I2C` by Frank de Brabander
   - `WiFi` (built-in for ESP32)
   - `HTTPClient` (built-in for ESP32)

### Configure ESP32 Board
1. Go to **File → Preferences**
2. Add to "Additional Board Manager URLs":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools → Board → Boards Manager**
4. Search for "esp32" and install "esp32 by Espressif Systems"
5. Select **Tools → Board → ESP32 Dev Module**

### Configure WiFi & API Settings
1. Open `iot/mediminder_esp32.ino`
2. Find these lines (around line 76-78):
   ```cpp
   const char* WIFI_SSID       = "test";
   const char* WIFI_PASSWORD   = "tester2025";
   const char* API_BASE_URL    = "http://10.81.124.83:5000";
   ```
3. Update with your values:
   ```cpp
   const char* WIFI_SSID       = "YourWiFiName";
   const char* WIFI_PASSWORD   = "YourWiFiPassword";
   const char* API_BASE_URL    = "http://YOUR_COMPUTER_IP:5000";
   ```

### Find Your Computer's IP Address
**macOS/Linux:**
```bash
ifconfig | grep "inet " | grep -v 127.0.0.1
```

**Windows:**
```cmd
ipconfig
```
Look for "IPv4 Address" under your active network adapter.

### Upload Firmware
1. Connect ESP32 via USB
2. Select **Tools → Port** → (your ESP32 port)
3. Click **Upload** button (→)
4. Wait for "Done uploading" message

### Monitor Serial Output
1. Click **Tools → Serial Monitor**
2. Set baud rate to **9600**
3. You should see:
   ```
   ╔═══════════════════════════════════════════════════════════╗
   ║         MEDIMINDER by SJNHS  –  Smart Dispenser          ║
   ║    Schematic Pinout | Flask REST IoT | 9600 Baud         ║
   ╚═══════════════════════════════════════════════════════════╝
   ```

## 🎯 Step 4: First Test (2 minutes)

### Test 1: LCD Display
- LCD should show: "Mediminder" / "by SJNHS"
- Then: "Initializing..." / "Please wait..."
- Finally: Current time and date

### Test 2: WiFi Connection
Check Serial Monitor for:
```
[OK]    [WIFI     ] Connected! IP: 192.168.x.x
```

### Test 3: API Communication
Serial Monitor should show:
```
[OK]    [API-GET  ] 7-Rack schedule & RTC timestamp received.
```

### Test 4: Web Dashboard
1. Open `http://localhost:5000` in browser
2. Click "🔄 Refresh" button in Device Cache Preview
3. You should see current configuration loaded

### Test 5: Manual Dispense
In Serial Monitor, type: `D` and press Enter

You should see:
- Stepper motor rotating
- Servo opening and closing
- Buzzer beeping
- LED flashing
- LCD showing "Medicine Ready!"

## 🎨 Step 5: Configure Your Schedule

### Via Web Dashboard
1. Open `http://localhost:5000`
2. Enter patient name
3. Set rotation degree (default 52° works for most setups)
4. Configure 7 rack schedules:
   - Click on datetime fields
   - Set date and time for each rack
   - Or click "⚡ Auto-Fill 4h Spacing" for quick setup
5. Click "💾 Save & Sync Configuration"

### Via Serial Commands
Type in Serial Monitor:
- `STATUS` or `S` - View current configuration
- `POLL` - Force refresh from server
- `HELP` - Show all commands

## 🧪 Step 6: Test Dispensing Cycle

### Method 1: Manual Test
1. In Serial Monitor, type: `D`
2. Watch the complete cycle:
   - ✓ Stepper rotates
   - ✓ Servo opens
   - ✓ Servo closes
   - ✓ Buzzer sounds
   - ✓ LED lights up
   - ✓ LCD shows "Medicine Ready!"

### Method 2: Scheduled Test
1. In web dashboard, set Rack #1 to current time + 1 minute
2. Click "Save & Sync"
3. Wait for the scheduled time
4. System should automatically dispense

### Method 3: Simulate from Dashboard
1. Click "ESP32: POST /api/dispense-log"
2. Check "Dispensation History" panel
3. Verify log entry appears

## 📊 Monitoring & Logs

### Real-Time Monitoring
- **LCD Display**: Shows current time, patient name, next dose
- **Serial Monitor**: Detailed logs of all operations
- **Web Dashboard**: Live status and history

### View Logs
- **Dispensation Logs**: Click "Dispensation History" in dashboard
- **Intake Logs**: Available via API `/api/intake-logs`
- **System Status**: Click "ESP32: GET /api/dispense" to see full status

## 🔧 Troubleshooting

### ESP32 Won't Connect to WiFi
```
Problem: WiFi connection timeout
Solution:
1. Verify SSID and password are correct
2. Ensure 2.4GHz network (ESP32 doesn't support 5GHz)
3. Check WiFi signal strength
4. Try moving ESP32 closer to router
```

### API Requests Failing
```
Problem: HTTP error codes in Serial Monitor
Solution:
1. Verify server is running (check http://localhost:5000)
2. Check firewall isn't blocking port 5000
3. Ensure ESP32 and computer are on same network
4. Verify API_BASE_URL has correct IP address
5. Try pinging server from ESP32 network
```

### Stepper Motor Not Moving
```
Problem: No rotation during dispense
Solution:
1. Check all 4 wires connected (IN1-IN4)
2. Verify 5V power to ULN2003 board
3. Check stepper motor is properly seated
4. Test with manual command: D
5. Verify GPIO pins match code (26,27,25,32)
```

### LCD Not Displaying
```
Problem: Blank or garbled LCD
Solution:
1. Check I2C address (default 0x27, some use 0x3F)
2. Verify SDA/SCL connections (GPIO 21/22)
3. Ensure 5V power to LCD
4. Adjust contrast potentiometer on LCD backpack
5. Test I2C scanner sketch to find address
```

### Servo Not Moving
```
Problem: Servo doesn't open/close
Solution:
1. Verify GPIO 13 connection
2. Check servo has separate 5V power supply
3. Ensure servo ground connected to ESP32 ground
4. Test with simple servo sweep sketch first
5. Check servo isn't mechanically stuck
```

## 📱 Next Steps

### Customize Your System
- [ ] Adjust rotation degrees for your rack design
- [ ] Set up 7-day medication schedule
- [ ] Configure alarm repeat intervals
- [ ] Customize LCD messages
- [ ] Add more patients (requires code modification)

### Enhance Security (See SECURITY_ENHANCEMENTS.md)
- [ ] Enable HTTPS
- [ ] Add API authentication
- [ ] Implement rate limiting
- [ ] Set up monitoring

### Advanced Features
- [ ] Add mobile notifications
- [ ] Integrate with pharmacy systems
- [ ] Set up cloud database
- [ ] Add voice alerts
- [ ] Implement caregiver portal

## 📚 Additional Resources

- **Full Documentation**: See `README.md`
- **Security Guide**: See `SECURITY_ENHANCEMENTS.md`
- **API Reference**: Check web dashboard footer for endpoints
- **Hardware Schematics**: See `docs/` folder (if available)

## 🆘 Getting Help

### Serial Monitor Commands
```
D or DISPENSE - Manual immediate dispense
POLL          - Force API schedule poll
S or STATUS   - Show current status
HELP          - Display command menu
```

### Check System Status
```bash
# Server status
curl http://localhost:5000/api/status

# Get current schedule
curl http://localhost:5000/api/dispense

# View logs
curl http://localhost:5000/api/dispense-logs
```

## ✅ Success Checklist

After completing this guide, you should have:
- [ ] Server running on port 5000
- [ ] ESP32 connected to WiFi
- [ ] LCD displaying time and patient info
- [ ] Successful API communication
- [ ] Manual dispense test working
- [ ] Web dashboard accessible
- [ ] Schedule configured
- [ ] All sensors responding

## 🎉 Congratulations!

Your Mediminder system is now operational! 

The system will:
- ✓ Automatically dispense medication at scheduled times
- ✓ Alert patient with buzzer and LED
- ✓ Detect when medication is retrieved
- ✓ Log all events to server
- ✓ Retry API calls if connection fails
- ✓ Continue operation with cached data if offline

---

**Mediminder by SJNHS** - Making medication management smarter and safer.

For questions or issues, refer to the main README.md or SECURITY_ENHANCEMENTS.md files.
