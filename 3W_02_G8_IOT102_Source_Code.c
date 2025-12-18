/*
 * ═══════════════════════════════════════════════════════════════
 *  SMARTDIGITAL CLOCK - WITH BLYNK MODE CONTROL
 * ═══════════════════════════════════════════════════════════════
 *  Author: Group 8
 *  Date: December 2025
 *  Version: 4.4 FIXED (Button + Health Warning)
 *  
 *  FIXED: 
 *  - Button short press: Switch mode (works in all modes)
 *  - Button long press (≥1s): Toggle mute in Mode 2
 *  - Health warning: Only beep after 10 seconds in danger zone
 * ═══════════════════════════════════════════════════════════════
 */

// ========== BLYNK CONFIG (MUST BE FIRST!) ==========
#define BLYNK_TEMPLATE_ID "TMPL6M3zYgiyV"
#define BLYNK_TEMPLATE_NAME "Smart Digital Clock"
#define BLYNK_AUTH_TOKEN "hxZ9BuGKx3jo4L0ogs0SjRyB2EeN9RoO"

// ========== LIBRARIES (AFTER BLYNK DEFINES) ==========
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <Wire.h>
#include <DS1302.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <EEPROM.h>

// ========== WIFI CONFIG ==========
char ssid[] = "Phat";
char pass[] = "12345678";

// WiFi connection timeout
const unsigned long WIFI_TIMEOUT = 15000; // 15 seconds
bool wifiConnected = false;
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 30000; // Check every 30s

// ========== PIN DEFINITIONS ==========
#define DHT_PIN        D3
#define RTC_CLK_PIN    D4
#define RTC_DAT_PIN    D5
#define RTC_RST_PIN    D8
#define BUTTON_PIN     D6
#define BUZZER_PIN     D7

// ========== BLYNK VIRTUAL PINS ==========
#define V_TIME         V0
#define V_DATE         V1
#define V_TEMP         V2
#define V_HUMIDITY     V3
#define V_HEARTRATE    V4
#define V_ALARM_HOUR   V5
#define V_ALARM_MIN    V6
#define V_ALARM_EN     V7
#define V_STOP_ALARM   V8
#define V_STATUS       V9
#define V_TERMINAL     V10
#define V_AUTO_MODE    V11
#define V_SELECT_MODE  V12
#define V_NEXT_MODE    V13

// ========== OBJECTS ==========
DHT dht(DHT_PIN, DHT11);
DS1302 rtc(RTC_RST_PIN, RTC_DAT_PIN, RTC_CLK_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
MAX30105 particleSensor;
BlynkTimer timer;

// ========== ALARM STRUCTURE ==========
struct AlarmData {
  int hour;
  int minute;
  bool enabled;
};

AlarmData alarm = {7, 0, false};

// ========== GLOBAL VARIABLES ==========
float temperature = 0.0;
float humidity = 0.0;
int heartRate = 0;
uint8_t beatsPerMinute = 0;
uint32_t irValue = 0;
bool fingerDetected = false;

int displayMode = 0;
bool autoModeSwitch = true;
unsigned long lastModeSwitch = 0;
const unsigned long MODE_INTERVAL = 5000;
bool forceUpdate = false;

bool alarmRinging = false;
unsigned long alarmStartTime = 0;
const unsigned long ALARM_DURATION = 60000;

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

bool lastButtonState = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;

const int HR_HIGH = 100;
const int HR_LOW = 60;
const float TEMP_HIGH = 35.0;

const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;

const char* MODE_NAMES[] = {"Time+Temp", "Heart Rate", "Full Info"};

bool alarmMuted = false;
unsigned long lastFingerRemoved = 0;

// Sensor reading intervals for offline mode
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 2000;

// Health warning tracking
unsigned long hrDangerStartTime = 0;
bool hrInDangerZone = false;
bool hrWarningActive = false;
const unsigned long HR_DANGER_DURATION = 10000; // 10 seconds
unsigned long lastHrWarningBeep = 0;

// ========== HELPER FUNCTIONS ==========
String getTimeString() {
  Time t = rtc.getTime();
  char buffer[10];
  sprintf(buffer, "%02d:%02d:%02d", t.hour, t.min, t.sec);
  return String(buffer);
}

String getDateString() {
  Time t = rtc.getTime();
  char buffer[12];
  sprintf(buffer, "%02d/%02d/%04d", t.date, t.mon, t.year);
  return String(buffer);
}

// ========== HEART RATE READING ==========
void readHeartRate() {
  irValue = particleSensor.getIR();

  if (irValue > 50000 && irValue < 200000) {
    fingerDetected = true;
  } else {
    if (fingerDetected) {
      lastFingerRemoved = millis();
    }
    fingerDetected = false;
  }

  if (fingerDetected && checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    float bpm = 60.0 / (delta / 1000.0);

    if (bpm > 20 && bpm < 200) {
      rates[rateSpot++] = (byte)bpm;
      rateSpot %= RATE_SIZE;

      heartRate = 0;
      for (byte i = 0; i < RATE_SIZE; i++) {
        heartRate += rates[i];
      }
      heartRate /= RATE_SIZE;
    }
  }

  if (!fingerDetected && millis() - lastFingerRemoved > 2000) {
    heartRate = 0;
    for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
  }
}

// ========== SENSOR READING ==========
void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  if (!isnan(h) && !isnan(t)) {
    humidity = h;
    temperature = t;
  }
}

// ========== BLYNK WRITE HANDLERS ==========
BLYNK_WRITE(V_ALARM_HOUR) {
  alarm.hour = param.asInt();
  saveAlarm();
  Blynk.virtualWrite(V_TERMINAL, 
    String("[") + getTimeString() + "] Alarm hour: " + String(alarm.hour) + "\n");
  updateStatusDisplay();
  Serial.printf("[BLYNK] Alarm hour: %02d\n", alarm.hour);
}

BLYNK_WRITE(V_ALARM_MIN) {
  alarm.minute = param.asInt();
  saveAlarm();
  Blynk.virtualWrite(V_TERMINAL, 
    String("[") + getTimeString() + "] Alarm minute: " + String(alarm.minute) + "\n");
  updateStatusDisplay();
  Serial.printf("[BLYNK] Alarm minute: %02d\n", alarm.minute);
}

BLYNK_WRITE(V_ALARM_EN) {
  alarm.enabled = param.asInt();
  saveAlarm();
  String status = alarm.enabled ? "ENABLED" : "DISABLED";
  Blynk.virtualWrite(V_TERMINAL, 
    String("[") + getTimeString() + "] Alarm " + status + "\n");
  updateStatusDisplay();
  Serial.printf("[BLYNK] Alarm %s\n", status.c_str());
}

BLYNK_WRITE(V_STOP_ALARM) {
  int buttonState = param.asInt();
  if (buttonState == 1 && alarmRinging) {
    stopAlarmSound("Blynk App");
  }
}

BLYNK_WRITE(V_AUTO_MODE) {
  autoModeSwitch = param.asInt();
  String status = autoModeSwitch ? "ENABLED" : "DISABLED";
  Blynk.virtualWrite(V_TERMINAL, 
    String("[") + getTimeString() + "] Auto mode: " + status + "\n");
  Serial.println("[MODE] Auto switch: " + status);
  if (autoModeSwitch) {
    lastModeSwitch = millis();
  }
  forceUpdate = true;
}

BLYNK_WRITE(V_SELECT_MODE) {
  int receivedValue = param.asInt();
  
  Serial.printf("[BLYNK] V_SELECT_MODE received: %d\n", receivedValue);
  
  int newMode = receivedValue;
  
  if (newMode >= 0 && newMode <= 2) {
    displayMode = newMode;
    if (autoModeSwitch) {
      autoModeSwitch = false;
      Blynk.virtualWrite(V_AUTO_MODE, 0);
    }
    Blynk.virtualWrite(V_TERMINAL, 
      String("[") + getTimeString() + "] Mode set to: " + 
      MODE_NAMES[newMode] + "\n");
    Serial.printf("[MODE] Manual select: Mode %d - %s\n", displayMode + 1, MODE_NAMES[newMode]);
    showModeChange();
    forceUpdate = true;
  } else {
    Serial.printf("[ERROR] Invalid mode value: %d (must be 0-2)\n", receivedValue);
    Blynk.virtualWrite(V_TERMINAL, 
      String("[") + getTimeString() + "] ERROR: Invalid mode value " + 
      String(receivedValue) + "\n");
  }
}

BLYNK_WRITE(V_NEXT_MODE) {
  int buttonPressed = param.asInt();
  if (buttonPressed == 1) {
    displayMode = (displayMode + 1) % 3;
    if (autoModeSwitch) {
      autoModeSwitch = false;
      Blynk.virtualWrite(V_AUTO_MODE, 0);
    }
    Blynk.virtualWrite(V_SELECT_MODE, displayMode);
    Blynk.virtualWrite(V_TERMINAL, 
      String("[") + getTimeString() + "] Mode switched to: " + 
      MODE_NAMES[displayMode] + "\n");
    Serial.printf("[MODE] Next mode: %s\n", MODE_NAMES[displayMode]);
    showModeChange();
    digitalWrite(BUZZER_PIN, HIGH);
    delay(50);
    digitalWrite(BUZZER_PIN, LOW);
    forceUpdate = true;
  }
}

void updateStatusDisplay() {
  if (!wifiConnected) return;
  
  String status = wifiConnected ? "🟢 Online" : "🔴 Offline";
  
  if (alarmRinging) {
    status = "🔴 ALARM RINGING!";
  } else if (alarm.enabled) {
    char buffer[30];
    sprintf(buffer, "🔔 Alarm: %02d:%02d", alarm.hour, alarm.minute);
    status = String(buffer);
  }
  
  if (!alarmRinging) {
    status += String(" | ") + MODE_NAMES[displayMode];
    if (autoModeSwitch) {
      status += " (Auto)";
    }
  }
  
  Blynk.virtualWrite(V_STATUS, status);
}

void showModeChange() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" MODE CHANGED ");
  lcd.setCursor(0, 1);
  String modeName = MODE_NAMES[displayMode];
  int spaces = (16 - modeName.length()) / 2;
  for (int i = 0; i < spaces; i++) lcd.print(" ");
  lcd.print(modeName);
  delay(1000);
  lcd.clear();
}

// ========== SEND DATA TO BLYNK ==========
void sendDataToBlynk() {
  if (!wifiConnected) return;
  
  Blynk.virtualWrite(V_TIME, getTimeString());
  Blynk.virtualWrite(V_DATE, getDateString());
  Blynk.virtualWrite(V_TEMP, temperature);
  Blynk.virtualWrite(V_HUMIDITY, humidity);
  Blynk.virtualWrite(V_HEARTRATE, fingerDetected ? heartRate : 0);
  updateStatusDisplay();
}

// ========== UPDATE LCD DISPLAY ==========
void updateDisplay() {
  if (autoModeSwitch && (millis() - lastModeSwitch >= MODE_INTERVAL)) {
    lastModeSwitch = millis();
    displayMode = (displayMode + 1) % 3;
    forceUpdate = true;
    
    if (wifiConnected) {
      Blynk.virtualWrite(V_SELECT_MODE, displayMode);
    }
    
    Serial.printf("[AUTO] Mode changed to: %d - %s\n", displayMode + 1, MODE_NAMES[displayMode]);
  }
  
  static int lastDisplayedMode = -1;
  static unsigned long lastLCDUpdate = 0;
  
  if (forceUpdate || lastDisplayedMode != displayMode || (millis() - lastLCDUpdate > 500)) {
    lastDisplayedMode = displayMode;
    lastLCDUpdate = millis();
    forceUpdate = false;
    
    lcd.clear();
    Time t = rtc.getTime();
    
    lcd.setCursor(14, 0);
    if (!wifiConnected) {
      lcd.print("O");
    }
    lcd.setCursor(15, 0);
    lcd.print(displayMode + 1);
    
    switch(displayMode) {
      case 0:
        lcd.setCursor(0, 0);
        if (t.hour < 10) lcd.print("0");
        lcd.print(t.hour);
        lcd.print(":");
        if (t.min < 10) lcd.print("0");
        lcd.print(t.min);
        lcd.print(":");
        if (t.sec < 10) lcd.print("0");
        lcd.print(t.sec);
        
        if (alarm.enabled) {
          lcd.setCursor(9, 0);
          lcd.print("A");
          lcd.setCursor(10, 0);
          lcd.printf("%02d:%02d", alarm.hour, alarm.minute);
        }
        
        lcd.setCursor(0, 1);
        lcd.printf("T:%.1fC H:%d%%", temperature, (int)humidity);
        break;
        
      case 1:
        lcd.setCursor(0, 0);
        lcd.print("IR:");
        lcd.print(irValue / 1000);
        lcd.print("k");
        
        if (alarmMuted) {
          lcd.setCursor(10, 0);
          lcd.print("[M]");
        }
        
        lcd.setCursor(0, 1);
        lcd.print("BPM:");
        
        if (irValue > 50000 && irValue < 200000) {
          if (heartRate > 0) {
            lcd.print(heartRate);
            lcd.print(" ");
            
            if (heartRate >= HR_HIGH || heartRate <= HR_LOW) {
              lcd.print("HIGH!");
            } else {
              lcd.print("OK");
            }
            
            if ((millis() / 500) % 2 == 0) {
              lcd.setCursor(13, 1);
              lcd.print("*");
            }
          } else {
            lcd.print("Wait...");
          }
        } else if (irValue >= 200000) {
          lcd.print("OVERLOAD!");
        } else {
          lcd.print("--");
        }
        break;
        
      case 2:
        lcd.setCursor(0, 0);
        if (t.date < 10) lcd.print("0");
        lcd.print(t.date);
        lcd.print("/");
        if (t.mon < 10) lcd.print("0");
        lcd.print(t.mon);
        lcd.print("/");
        lcd.print(t.year);
        
        lcd.setCursor(0, 1);
        lcd.printf("%.1fC %d%% %dBPM", temperature, (int)humidity, heartRate);
        break;
    }
  }
}

// ========== PHYSICAL BUTTON HANDLER ==========
void handlePhysicalButton() {
  static unsigned long buttonPressTime = 0;
  static bool buttonWasPressed = false;
  
  bool reading = digitalRead(BUTTON_PIN);
  
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;
      
      // Button PRESSED
      if (buttonState == LOW) {
        buttonPressTime = millis();
        buttonWasPressed = true;
      }
      // Button RELEASED
      else if (buttonWasPressed) {
        buttonWasPressed = false;
        unsigned long pressDuration = millis() - buttonPressTime;
        
        if (alarmRinging) {
          stopAlarmSound("Physical Button");
        } else {
          // SHORT PRESS: Switch Mode
          if (pressDuration < 1000) {
            displayMode = (displayMode + 1) % 3;
            
            if (autoModeSwitch) {
              autoModeSwitch = false;
              if (wifiConnected) {
                Blynk.virtualWrite(V_AUTO_MODE, 0);
                Blynk.virtualWrite(V_SELECT_MODE, displayMode);
              }
            }
            
            Serial.printf("[BUTTON] Short press - Mode switched to: %d - %s\n", displayMode + 1, MODE_NAMES[displayMode]);
            showModeChange();
            
            digitalWrite(BUZZER_PIN, HIGH);
            delay(50);
            digitalWrite(BUZZER_PIN, LOW);
            forceUpdate = true;
          }
          // LONG PRESS (≥1s): Toggle Mute (only in Mode 2)
          else {
            if (displayMode == 1) {
              alarmMuted = !alarmMuted;
              
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("Alarm Warning:");
              lcd.setCursor(0, 1);
              lcd.print(alarmMuted ? "MUTED" : "UNMUTED");
              
              Serial.print("[BUTTON] Long press - Alarm ");
              Serial.println(alarmMuted ? "MUTED" : "UNMUTED");
              
              // Beep pattern: 2 short beeps for mute, 1 long for unmute
              if (alarmMuted) {
                digitalWrite(BUZZER_PIN, HIGH);
                delay(100);
                digitalWrite(BUZZER_PIN, LOW);
                delay(100);
                digitalWrite(BUZZER_PIN, HIGH);
                delay(100);
                digitalWrite(BUZZER_PIN, LOW);
              } else {
                digitalWrite(BUZZER_PIN, HIGH);
                delay(300);
                digitalWrite(BUZZER_PIN, LOW);
              }
              
              delay(1500);
              forceUpdate = true;
            } else {
              // Long press in other modes - show info
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("Long press:");
              lcd.setCursor(0, 1);
              lcd.print("Mode 2 only");
              delay(1000);
              forceUpdate = true;
            }
          }
        }
      }
    }
  }
  
  lastButtonState = reading;
}

// ========== ALARM CONTROL ==========
void checkAlarm() {
  if (!alarm.enabled) {
    if (alarmRinging) stopAlarmSound("Auto");
    return;
  }
  
  Time t = rtc.getTime();
  
  if (t.hour == alarm.hour && 
      t.min == alarm.minute && 
      t.sec == 0 && 
      !alarmRinging) {
    
    alarmRinging = true;
    alarmStartTime = millis();
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("*** ALARM! ***");
    lcd.setCursor(0, 1);
    lcd.printf("Press button!");
    
    if (wifiConnected) {
      Blynk.virtualWrite(V_TERMINAL, 
        String("[") + getTimeString() + "] ⏰ ALARM RINGING!\n");
      
      Blynk.logEvent("alarm_event", String("Alarm at ") + 
        String(alarm.hour) + ":" + String(alarm.minute));
      
      updateStatusDisplay();
    }
    
    Serial.println("[ALARM] ⏰ TRIGGERED!");
  }
  
  if (alarmRinging) {
    playAlarmSound();
    
    if (millis() - alarmStartTime > ALARM_DURATION) {
      stopAlarmSound("Timeout");
    }
  }
}

void playAlarmSound() {
  if (millis() - lastBuzzerToggle > (buzzerState ? 1000 : 500)) {
    lastBuzzerToggle = millis();
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
  }
}

void stopAlarmSound(String source) {
  alarmRinging = false;
  digitalWrite(BUZZER_PIN, LOW);
  buzzerState = false;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Alarm Stopped");
  lcd.setCursor(0, 1);
  lcd.print("by " + source);
  delay(2000);
  
  if (wifiConnected) {
    Blynk.virtualWrite(V_TERMINAL, 
      String("[") + getTimeString() + "] Alarm stopped by " + source + "\n");
    updateStatusDisplay();
  }
  
  forceUpdate = true;
  Serial.println("[ALARM] Stopped by " + source);
}

// ========== HEALTH WARNINGS ==========
void checkHealthWarnings() {
  // Check if heart rate is in danger zone
  bool currentlyInDanger = false;
  
  if (fingerDetected && heartRate > 0) {
    if (heartRate >= HR_HIGH || heartRate <= HR_LOW) {
      currentlyInDanger = true;
      
      // Start tracking if just entered danger zone
      if (!hrInDangerZone) {
        hrInDangerZone = true;
        hrDangerStartTime = millis();
        Serial.printf("[HR] Entered danger zone: %d BPM\n", heartRate);
      }
      
      // Check if been in danger zone long enough
      unsigned long timeInDanger = millis() - hrDangerStartTime;
      
      if (timeInDanger >= HR_DANGER_DURATION && !hrWarningActive) {
        // Activate warning after 10 seconds
        hrWarningActive = true;
        
        String msg = String("⚠️ DANGER HR: ") + String(heartRate) + " BPM for " + 
                     String(timeInDanger/1000) + "s";
        
        if (wifiConnected) {
          Blynk.virtualWrite(V_TERMINAL, 
            String("[") + getTimeString() + "] " + msg + "\n");
          Blynk.logEvent("health_warning", msg);
        }
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("! DANGER HR !");
        lcd.setCursor(0, 1);
        lcd.printf("%d BPM - %ds", heartRate, (int)(timeInDanger/1000));
        
        Serial.printf("[WARNING] HR danger for %d seconds: %d BPM\n", 
                      (int)(timeInDanger/1000), heartRate);
        
        delay(2000);
        forceUpdate = true;
      }
      
      // Continuous beeping while in danger and not muted
      if (hrWarningActive && !alarmMuted) {
        if (millis() - lastHrWarningBeep >= 2000) {
          lastHrWarningBeep = millis();
          
          // Beep pattern: 3 quick beeps
          for (int i = 0; i < 3; i++) {
            digitalWrite(BUZZER_PIN, HIGH);
            delay(100);
            digitalWrite(BUZZER_PIN, LOW);
            delay(100);
          }
        }
      }
      
    } else {
      currentlyInDanger = false;
    }
  } else {
    currentlyInDanger = false;
  }
  
  // Reset if left danger zone
  if (!currentlyInDanger && hrInDangerZone) {
    hrInDangerZone = false;
    
    if (hrWarningActive) {
      hrWarningActive = false;
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("HR: Normal");
      lcd.setCursor(0, 1);
      lcd.printf("Was: %d BPM", heartRate);
      
      Serial.println("[HR] Returned to normal");
      
      if (wifiConnected) {
        Blynk.virtualWrite(V_TERMINAL, 
          String("[") + getTimeString() + "] HR returned to normal\n");
      }
      
      delay(1500);
      forceUpdate = true;
    }
  }
  
  // Temperature warning (keep original 30s cooldown)
  static unsigned long lastTempWarning = 0;
  
  if (temperature > TEMP_HIGH && millis() - lastTempWarning > 30000) {
    lastTempWarning = millis();
    
    String msg = String("⚠️ HIGH TEMP: ") + String(temperature, 1) + "°C";
    
    if (wifiConnected) {
      Blynk.virtualWrite(V_TERMINAL, 
        String("[") + getTimeString() + "] " + msg + "\n");
      Blynk.logEvent("health_warning", msg);
    }
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("! HIGH TEMP !");
    lcd.setCursor(0, 1);
    lcd.printf("%.1fC", temperature);
    
    for (int i = 0; i < 2; i++) {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(150);
      digitalWrite(BUZZER_PIN, LOW);
      delay(150);
    }
    
    delay(2000);
    forceUpdate = true;
    Serial.printf("[WARNING] High temp: %.1f°C\n", temperature);
  }
}

// ========== EEPROM FUNCTIONS ==========
void saveAlarm() {
  EEPROM.write(0, alarm.hour);
  EEPROM.write(1, alarm.minute);
  EEPROM.write(2, alarm.enabled ? 1 : 0);
  EEPROM.commit();
  Serial.println("[EEPROM] Alarm saved");
}

void loadAlarm() {
  alarm.hour = EEPROM.read(0);
  alarm.minute = EEPROM.read(1);
  alarm.enabled = EEPROM.read(2) == 1;
  
  if (alarm.hour > 23) alarm.hour = 7;
  if (alarm.minute > 59) alarm.minute = 0;
}

// ========== WIFI MANAGEMENT ==========
bool connectWiFi() {
  Serial.println("\n[WIFI] Attempting connection...");
  Serial.print("[WIFI] SSID: ");
  Serial.println(ssid);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi connecting");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  
  unsigned long startAttempt = millis();
  int dotCount = 0;
  
  while (WiFi.status() != WL_CONNECTED && 
         millis() - startAttempt < WIFI_TIMEOUT) {
    delay(500);
    Serial.print(".");
    
    lcd.setCursor(0, 1);
    for (int i = 0; i < dotCount; i++) lcd.print(".");
    dotCount = (dotCount + 1) % 16;
    
    if (millis() - startAttempt > WIFI_TIMEOUT / 2) {
      WiFi.disconnect();
      WiFi.begin(ssid, pass);
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] ✅ Connected!");
    Serial.print("[IP] ");
    Serial.println(WiFi.localIP());
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi: Connected");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(2000);
    
    return true;
  } else {
    Serial.println("\n[WIFI] ❌ Connection failed!");
    Serial.println("[WIFI] Continuing in OFFLINE mode");
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi: OFFLINE");
    lcd.setCursor(0, 1);
    lcd.print("Mode: Standalone");
    delay(3000);
    
    return false;
  }
}

void checkWiFiStatus() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = millis();
  
  bool currentStatus = (WiFi.status() == WL_CONNECTED);
  
  if (currentStatus != wifiConnected) {
    wifiConnected = currentStatus;
    
    if (wifiConnected) {
      Serial.println("[WIFI] ✅ Reconnected!");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("WiFi: Online");
      lcd.setCursor(0, 1);
      lcd.print(WiFi.localIP());
      delay(2000);
      forceUpdate = true;
    } else {
      Serial.println("[WIFI] ❌ Disconnected!");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("WiFi: Offline");
      lcd.setCursor(0, 1);
      lcd.print("Mode: Standalone");
      delay(2000);
      forceUpdate = true;
    }
  }
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║   SMART CLOCK - VERSION 4.4 FIXED    ║");
  Serial.println("║   Button & Health Warning Fixed      ║");
  Serial.println("╚═══════════════════════════════════════╝\n");
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);
  
  Wire.begin(D2, D1);
  Serial.println("[I2C] Initialized: SDA=D2, SCL=D1");
  
  Serial.print("[DS1302] Init... ");
  rtc.halt(false);
  rtc.writeProtect(false);
  Time t = rtc.getTime();
  Serial.println("OK");
  Serial.printf("[DS1302] %02d/%02d/%04d %02d:%02d:%02d\n",
    t.date, t.mon, t.year, t.hour, t.min, t.sec);
  
  Serial.print("[LCD] Init... ");
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" SMART CLOCK ");
  lcd.setCursor(0, 1);
  lcd.print("  v4.4 FIXED  ");
  Serial.println("OK");
  delay(2000);
  
  Serial.print("[DHT11] Init... ");
  dht.begin();
  Serial.println("OK");
  
  Serial.print("[MAX30102] Init... ");
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("FAILED!");
    lcd.clear();
    lcd.print("MAX30102 ERROR!");
    delay(2000);
  } else {
    Serial.println("OK");
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x0A);
    particleSensor.setPulseAmplitudeGreen(0);
    Serial.println("[MAX30102] LED: Red=0x0A, Green=OFF");
  }
  
  EEPROM.begin(512);
  loadAlarm();
  Serial.printf("[ALARM] Loaded: %02d:%02d (%s)\n", 
    alarm.hour, alarm.minute, alarm.enabled ? "ON" : "OFF");
  
  Serial.print("[BUTTON] Testing... ");
  Serial.println(digitalRead(BUTTON_PIN) == HIGH ? "OK" : "PRESSED");
  
  wifiConnected = connectWiFi();
  
  if (wifiConnected) {
    Serial.println("[BLYNK] Connecting...");
    Blynk.config(BLYNK_AUTH_TOKEN);
    
    if (Blynk.connect(3000)) {
      Serial.println("[BLYNK] ✅ Connected!");
      Blynk.virtualWrite(V_TERMINAL, 
        String("[") + getTimeString() + "] System started (Online Mode)\n");
      
      timer.setInterval(2000L, readSensors);
      timer.setInterval(3000L, sendDataToBlynk);
    } else {
      Serial.println("[BLYNK] ❌ Connection failed!");
      wifiConnected = false;
    }
  }
  
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  delay(100);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  
  if (wifiConnected) {
    Serial.println("\n[SYSTEM] Ready! Mode: ONLINE ✅");
  } else {
    Serial.println("\n[SYSTEM] Ready! Mode: OFFLINE (Standalone) 🔴");
    Serial.println("[INFO] All sensors working independently");
    Serial.println("[INFO] Short press: Switch mode | Long press (Mode 2): Mute");
  }
  
  lcd.clear();
}

// ========== MAIN LOOP ==========
void loop() {
  if (wifiConnected) {
    Blynk.run();
    timer.run();
    checkWiFiStatus();
  } else {
    if (millis() - lastSensorRead > SENSOR_READ_INTERVAL) {
      lastSensorRead = millis();
      readSensors();
    }
    checkWiFiStatus();
  }
  
  readHeartRate();
  checkAlarm();
  checkHealthWarnings(); // Check health warnings continuously
  handlePhysicalButton();
  updateDisplay();
}
