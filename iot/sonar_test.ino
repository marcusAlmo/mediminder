/*
 * SONAR PROXIMITY TEST – HC-SR04 Debug
 * 
 * Dual feedback:
 * - LED: fast blink (100ms) = valid | slow blink (500ms) = timeout | steady = out of range
 * - LCD: displays distance in cm + status
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define SONAR_TRIG_PIN   5
#define SONAR_ECHO_PIN   18
#define SONAR_TIMEOUT_US 26000UL
#define LED_PIN          2

#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22
#define LCD_ADDR         0x27
#define LCD_COLS         16
#define LCD_ROWS         2

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

void setup() {
  pinMode(SONAR_TRIG_PIN, OUTPUT);
  pinMode(SONAR_ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  
  // Init I2C & LCD
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  
  // Startup blink pattern
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SONAR TEST");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(1500);
}

float sonarReadCm() {
  digitalWrite(SONAR_TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(SONAR_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(SONAR_TRIG_PIN, LOW);
  
  long dur = pulseIn(SONAR_ECHO_PIN, HIGH, SONAR_TIMEOUT_US);
  if (dur == 0) return -1.0f;
  return (float)dur / 58.2f;
}

void loop() {
  float dist = sonarReadCm();
  
  // Update LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  
  if (dist < 0) {
    // Timeout - slow blink (500ms on, 500ms off)
    lcd.print("TIMEOUT");
    lcd.setCursor(0, 1);
    lcd.print("No response");
    
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
  } else if (dist > 400) {
    // Out of range - steady on
    lcd.print("OUT OF RANGE");
    lcd.setCursor(0, 1);
    lcd.print("> 400 cm");
    
    digitalWrite(LED_PIN, HIGH);
    delay(1000);
  } else {
    // Valid reading - fast blink (100ms on, 100ms off)
    lcd.print("Distance:");
    lcd.setCursor(0, 1);
    lcd.print(dist, 1);
    lcd.print(" cm");
    
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
}
