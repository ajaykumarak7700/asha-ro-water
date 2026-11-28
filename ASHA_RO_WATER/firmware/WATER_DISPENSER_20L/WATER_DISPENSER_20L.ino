/**
 * =================================================================
 *        🚰 ASHA RO WATER - ULTRA-FAST SMART DISPENSER (v2.5)
 * =================================================================
 * Features:
 *   1. ⚡ Lightning-Fast RFID: Max antenna gain (48dB), ultra-responsive.
 *   2. 🛡️ Smart Offline Fallback: If WiFi/Server lags, uses local flash memory.
 *      Never says 'Unregistered' on network glitches!
 *   3. 🏷️ 6 Quantity Tiers: 1L, 2L, 5L, 10L, 15L, 20L (Configurable in Admin).
 *   4. 🔘 9 Hardware Buttons:
 *      - 1L, 2L, 5L, 10L, 15L, 20L (Quantity)
 *      - Green START (Jar Protection)
 *      - Red CANCEL (Emergency Cutoff)
 *      - CARD BALANCE ENQUIRY (Check balance without deducting money)
 *   5. 🔄 Any-Order Dual Flow: Card first OR Quantity button first!
 *   6. 🛑 10s Safety Timeout: Rs.0 deducted & 0L wasted if unselected.
 *   7. 🔴 Remote Maintenance/Offline Mode from Admin Panel.
 *   8. 🌊 ZJ-S201 Flow Sensor (K-Factor) & Dry-Run Shield.
 *   9. 📶 Auto-Sync: Cards & offline deductions synced in background.
 * =================================================================
 */

#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

WiFiMulti wifiMulti;
Preferences prefs;

// =================================================================
// ⚙️ SERVER & SYSTEM CONFIGURATION (CLOUD + LOCAL AUTO-SWITCH)
// =================================================================
const char* CLOUD_SERVER_URL = "https://asha-ro-water.onrender.com"; // Render Live Global Cloud
const char* LOCAL_SERVER_URL = "http://192.168.1.8:3000";            // Local Computer LAN Fallback
String      activeServerBase = CLOUD_SERVER_URL;

WiFiClientSecure secClient;
WiFiClient       normalClient;

bool beginHttp(HTTPClient &http, String pathAndQuery) {
  String fullUrl = activeServerBase + pathAndQuery;
  if (fullUrl.startsWith("https://")) {
    secClient.setInsecure();
    return http.begin(secClient, fullUrl);
  } else {
    return http.begin(normalClient, fullUrl);
  }
}

float pricePerLitreRs       = 1.0;
int   flowStallSeconds      = 6;
float pulsesPerLitre        = 450.0;
const bool RELAY_ACTIVE_LOW = false;

// Quantity-Wise Tiered Prices (1L, 2L, 5L, 10L, 15L, 20L)
float price1L               = 1.0;
float price2L               = 2.0;
float price5L               = 5.0;
float price10L              = 10.0;
float price15L              = 15.0;
float price20L              = 15.0;

// ⚡ AUTO DISPENSE MODE (Bina Button Direct Paani)
bool  autoModeEnabled       = false; // Default OFF (toggled via web or long-press balance button)
float autoModeLitres        = 20.0;  // Default auto quantity (Litres)
float autoModeAmountRs      = 15.0;  // Default auto deduction amount (Rs)

bool isMachineInMaintenance = false;
float tankWaterRemaining    = 1000.0;

// =================================================================
// 📌 PIN DEFINITIONS (ALL 9 BUTTONS & SENSORS)
// =================================================================
#define RFID_SS_PIN    5
#define RFID_RST_PIN   22

#define OLED_SDA_PIN   21
#define OLED_SCL_PIN   15
#define OLED_ADDR      0x3C

#define BUZZER_PIN     13
#define FLOW_PIN       34
#define RELAY_PIN      26

// Push Buttons & LED Pins
#define BTN_START_PIN  4    // Green Push Button (Start)
#define BTN_CANCEL_PIN 16   // Red Push Button (Cancel / Emergency Stop)
#define LED_START_PIN  2    // Onboard Blue LED Indicator

#define BTN_1L_PIN     17   // 1 Litre Button
#define BTN_2L_PIN     14   // 2 Litres Button (NEW)
#define BTN_5L_PIN     27   // 5 Litres Button
#define BTN_10L_PIN    32   // 10 Litres Button
#define BTN_15L_PIN    12   // 15 Litres Button (NEW)
#define BTN_20L_PIN    33   // 20 Litres Button
#define BTN_BAL_PIN    25   // Check Card Balance Enquiry Button (Pin D25)

// Hardware Objects
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Flow Sensor Interrupt
volatile unsigned long flowPulseCount = 0;
void IRAM_ATTR flowPulseISR() {
  flowPulseCount++;
}

// Runtime State
bool isDispensing          = false;
String lastScannedUID      = "";
unsigned long lastScanTime = 0;
bool wifiConnected         = false;

unsigned long lastQRPollTime   = 0;
unsigned long lastCardSyncTime = 0;
unsigned long lastWiFiCheck    = 0;
unsigned long lastOledRefresh  = 0;

// Forward Declarations
bool checkPendingQROrders();
void handleCardScanned(String uid, float preselectedLitres = 0.0);
void handlePreSelection(float preselectedLitres);
void handleBalanceEnquiry();
void handleAutoModeConfigMenu();
bool readCardClean(String &outUid);
void syncCardsAndOfflineQueue();

// Helper: Calculate price based on selected litres
float getQuantityCost(float litres) {
  if (litres == 1.0)  return price1L;
  if (litres == 2.0)  return price2L;
  if (litres == 5.0)  return price5L;
  if (litres == 10.0) return price10L;
  if (litres == 15.0) return price15L;
  if (litres == 20.0) return price20L;
  return litres * pricePerLitreRs;
}

// =================================================================
// ⚡ 100% BULLETPROOF RFID ENGINE (REQA + WUPA + FIFO Auto-Flush)
// Har bar card pas aate hi instant read karega, kabhi lock nahi hoga!
// =================================================================
bool readCardClean(String &outUid) {
  // 1. Hardware health sanity check: if SPI hung or chip glitch, recover fast
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (ver == 0x00 || ver == 0xFF) {
    rfid.PCD_Init();
    rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
    return false;
  }

  // 2. Clear residual FIFO bytes & command interrupts from previous attempts
  rfid.PCD_WriteRegister(MFRC522::CommandReg, MFRC522::PCD_Idle);
  rfid.PCD_WriteRegister(MFRC522::ComIrqReg, 0x7F);
  rfid.PCD_WriteRegister(MFRC522::FIFOLevelReg, 0x80); // Flush FIFO

  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);

  // 3. Reset modulation registers
  rfid.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);

  // 4. Try standard REQA (PICC_CMD_REQA: detects fresh cards in IDLE state)
  MFRC522::StatusCode status = rfid.PICC_RequestA(bufferATQA, &bufferSize);

  // 5. If REQA fails, try WUPA (PICC_CMD_WUPA: wakes up cards in READY, HALT, or sleep state!)
  if (status != MFRC522::STATUS_OK && status != MFRC522::STATUS_COLLISION) {
    bufferSize = sizeof(bufferATQA);
    status = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
  }

  // 6. Card responded to either REQA or WUPA
  if (status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION) {
    if (rfid.PICC_ReadCardSerial()) {
      outUid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        if (i > 0) outUid += " ";
        if (rfid.uid.uidByte[i] < 0x10) outUid += "0";
        outUid += String(rfid.uid.uidByte[i], HEX);
      }
      outUid.toUpperCase();

      // Properly halt card & stop crypto
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      // Quick 1ms RF field pulse to discharge passive card capacitor
      // Prevents card staying stuck in HALT if held near reader!
      rfid.PCD_AntennaOff();
      delayMicroseconds(1000);
      rfid.PCD_AntennaOn();
      rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);

      return true;
    } else {
      // Collision or read glitch at edge of field: discharge card immediately
      rfid.PCD_AntennaOff();
      delay(15);
      rfid.PCD_AntennaOn();
      rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
      return false;
    }
  }

  return false;
}

// =================================================================
// 🔔 BUZZER & SOUND
// =================================================================
void beep(int count, int durationMs, int freq = 1400) {
  for (int i = 0; i < count; i++) {
    tone(BUZZER_PIN, freq, durationMs);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(60);
  }
}

// =================================================================
// ⚡ MOTOR / RELAY CONTROL
// =================================================================
void setMotor(bool on) {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
}

// =================================================================
// 🖥️ OLED SCREEN HELPER
// =================================================================
void showScreen(String title, String line1, String line2, String line3) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(4, 2);
  display.println(title);
  display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

  display.setCursor(0, 18);
  display.println(line1);

  display.setCursor(0, 32);
  display.println(line2);

  display.setCursor(0, 48);
  display.println(line3);

  display.display();
}

// =================================================================
// 🌐 NON-BLOCKING WIFI STATUS CHECK (Runs every 8 seconds)
// =================================================================
void checkWiFiConnection() {
  unsigned long now = millis();
  if (now - lastWiFiCheck < 8000) return;
  lastWiFiCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
  } else {
    if (wifiMulti.run() == WL_CONNECTED) {
      wifiConnected = true;
    } else {
      wifiConnected = false;
    }
  }
}

// =================================================================
// 🔘 WAIT FOR START BUTTON PRESS (Jar Placement Protection)
// =================================================================
bool waitForStartButton(String promptTitle, float litresToDispense) {
  Serial.println("[START_BTN] Waiting for start button...");
  unsigned long startWaitTime = millis();
  unsigned long lastBlinkTime = 0;
  bool ledState = false;
  unsigned long lastBeepTime = millis();
  const int WAIT_SECONDS = 30; // 30s timeout

  while (true) {
    unsigned long now = millis();
    long remainSec = WAIT_SECONDS - ((now - startWaitTime) / 1000);

    if (remainSec <= 0) {
      digitalWrite(LED_START_PIN, LOW);
      showScreen("TIME KHATAM!", "Jar Nahi Lagaya", "Order Cancelled", "Paisa Refund!");
      beep(3, 150, 700);
      delay(2000);
      return false;
    }

    // Blink Green LED
    if (now - lastBlinkTime >= 250) {
      lastBlinkTime = now;
      ledState = !ledState;
      digitalWrite(LED_START_PIN, ledState ? HIGH : LOW);
    }

    // Soft attention beep every 3.5s
    if (now - lastBeepTime >= 3500) {
      lastBeepTime = now;
      beep(1, 35, 1800);
    }

    // OLED Prompt
    static unsigned long lastPromptUpdate = 0;
    if (now - lastPromptUpdate >= 250) {
      lastPromptUpdate = now;
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(4, 2);
      display.print("JAR RAKHEIN: ");
      display.print(litresToDispense, (litresToDispense == (int)litresToDispense ? 0 : 1));
      display.println("L");
      display.drawFastHLine(0, 13, 128, SSD1306_WHITE);

      display.setCursor(2, 18);
      display.println("Hara Button Dabaye");

      display.setCursor(2, 32);
      display.println("Pani Girna Shuru");

      display.setCursor(2, 48);
      display.print("Lal=Radd (");
      display.print(remainSec);
      display.println("s)");
      display.display();
    }

    // Cancel Button
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_CANCEL_PIN) == LOW) {
        digitalWrite(LED_START_PIN, LOW);
        showScreen("RADD KIYA GAYA", "Lal Button Dabaya", "Order Cancelled", "Paisa Surakshit");
        beep(2, 150, 700);
        delay(1800);
        return false;
      }
    }

    // Start Button (Green)
    if (digitalRead(BTN_START_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_START_PIN) == LOW) {
        digitalWrite(LED_START_PIN, HIGH);
        beep(2, 60, 1600);
        showScreen("JAR DETECTED", "Pani Chalu Ho Raha..", String(litresToDispense, 1) + " Litres", "Starting Pump...");
        delay(600);
        digitalWrite(LED_START_PIN, LOW);
        return true;
      }
    }

    delay(10);
  }
}

// =================================================================
// 🔘 QUANTITY SELECTION BUTTONS (1L, 2L, 5L, 10L, 15L, 20L)
// =================================================================
float selectCardQuantity(String custName, float balanceRs) {
  Serial.println("[QTY] Select quantity: 1L, 2L, 5L, 10L, 15L, 20L");
  unsigned long startWait = millis();

  while (millis() - startWait < 10000) { // 10 seconds safety timeout
    unsigned long remainSec = 10 - ((millis() - startWait) / 1000);

    static unsigned long lastDisp = 0;
    if (millis() - lastDisp >= 250) {
      lastDisp = millis();
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(4, 2);
      display.println(custName.substring(0, 18));
      display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

      display.setCursor(2, 16);
      display.print("Bal: Rs.");
      display.print(balanceRs, 1);
      display.print(" (");
      display.print(remainSec);
      display.println("s)");

      display.setCursor(2, 30);
      display.println("1L 2L 5L 10L 15L 20L");

      display.setCursor(2, 46);
      display.println("Litre Chunein (Lal=X)");
      display.display();
    }

    // Cancel Button
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_CANCEL_PIN) == LOW) {
        showScreen("RADD KIYA GAYA", custName, "Process Cancelled", "Paisa Surakshit");
        beep(2, 100, 700);
        delay(1200);
        return 0.0;
      }
    }

    // 1 Litre
    if (digitalRead(BTN_1L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_1L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 1 LITER", custName, "Rupaye: Rs." + String(price1L, 1), "1L Selected");
        delay(500);
        return 1.0;
      }
    }

    // 2 Litres
    if (digitalRead(BTN_2L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_2L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 2 LITER", custName, "Rupaye: Rs." + String(price2L, 1), "2L Selected");
        delay(500);
        return 2.0;
      }
    }

    // 5 Litres
    if (digitalRead(BTN_5L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_5L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 5 LITER", custName, "Rupaye: Rs." + String(price5L, 1), "5L Selected");
        delay(500);
        return 5.0;
      }
    }

    // 10 Litres
    if (digitalRead(BTN_10L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_10L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 10 LITER", custName, "Rupaye: Rs." + String(price10L, 1), "10L Selected");
        delay(500);
        return 10.0;
      }
    }

    // 15 Litres
    if (digitalRead(BTN_15L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_15L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 15 LITER", custName, "Rupaye: Rs." + String(price15L, 1), "15L Selected");
        delay(500);
        return 15.0;
      }
    }

    // 20 Litres
    if (digitalRead(BTN_20L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_20L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 20 LITER", custName, "Offer: Rs." + String(price20L, 1), "20L Jar Selected!");
        delay(500);
        return 20.0;
      }
    }

    delay(10);
  }

  // 🛑 10s TIMEOUT: No button was pressed! Cancel safely (0 water, Rs.0 deducted)
  showScreen("TIME KHATAM!", "Litre Nahi Chuna", "Paisa Surakshit", "0L Pani - Rs.0 Kata");
  beep(2, 120, 800);
  delay(1800);
  return 0.0;
}

// =================================================================
// 💳 CHECK CARD BALANCE ENQUIRY (Zero Deduction, Safe Check)
// =================================================================
void handleBalanceEnquiry() {
  beep(1, 80, 1800);
  showScreen("BALANCE JAANCH", "Apna Card Tap Karein", "Balance Janne Ke Liye", "(Paisa Nahi Katega)");
  unsigned long startWait = millis();

  while (millis() - startWait < 12000) {
    // Cancel button
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      beep(1, 100, 700);
      showScreen("CANCELLED", "Balance Enquiry", "Band Ho Gayi", "");
      delay(800);
      return;
    }

    String uid = "";
    if (readCardClean(uid)) {
      String uidClean = uid;
      uidClean.replace(" ", "");

      // 1. Read from flash
      String nKey = "n_" + uidClean;
      bool isKnown = prefs.isKey(nKey.c_str());
      String custName = isKnown ? prefs.getString(nKey.c_str(), "Customer") : "Customer";
      float localBal = isKnown ? prefs.getFloat(("b_" + uidClean).c_str(), 0.0) : 0.0;

      // 2. Fetch latest online if connected
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        beginHttp(http, "/api/card/" + uidClean);
        http.setTimeout(1200);
        int code = http.GET();
        if (code == 200) {
          JsonDocument doc;
          deserializeJson(doc, http.getString());
          if (doc["success"] == true) {
            custName = doc["data"]["name"].as<String>();
            long bp = doc["data"]["balance_paise"] | 0;
            localBal = (float)bp / 100.0;
            prefs.putString(nKey.c_str(), custName);
            prefs.putFloat(("b_" + uidClean).c_str(), localBal);
          }
        }
        http.end();
      }

      beep(2, 70, 1700);
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(4, 2);
      display.println("CARD BALANCE ENQUIRY");
      display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

      display.setCursor(2, 16);
      display.println(custName.substring(0, 18));

      display.setCursor(2, 30);
      display.setTextSize(2);
      display.print("Rs.");
      display.println(localBal, 1);

      display.setTextSize(1);
      display.setCursor(2, 50);
      display.println("Rs.0 Kata | Surakshit!");
      display.display();

      delay(3800); // 3.8s peaceful display time

      // 🔄 Fresh RFID reset to avoid RF antenna lockup
      rfid.PCD_AntennaOff();
      delay(50);
      rfid.PCD_Init();
      rfid.PCD_AntennaOn();
      rfid.PCD_SetAntennaGain(rfid.RxGain_max);
      lastScannedUID = "";
      return;
    }
    delay(10);
  }

  showScreen("TIME OUT", "Card Tap Nahi Hua", "Standby Mode", "");
  delay(1000);
  rfid.PCD_AntennaOff();
  delay(30);
  rfid.PCD_Init();
  rfid.PCD_AntennaOn();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  lastScannedUID = "";
}

// =================================================================
// 🔘 PRE-SELECTION: LITRE BUTTON PRESSED FIRST ON STANDBY
// =================================================================
void handlePreSelection(float preselectedLitres) {
  beep(1, 80, 1800);
  float cost = getQuantityCost(preselectedLitres);
  String lStr = String(preselectedLitres, (preselectedLitres == (int)preselectedLitres ? 0 : 1)) + "L";

  unsigned long startWait = millis();
  while (millis() - startWait < 12000) { // 12 seconds
    unsigned long rem = 12 - ((millis() - startWait) / 1000);

    static unsigned long lastOled = 0;
    if (millis() - lastOled > 250) {
      lastOled = millis();
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(4, 2);
      display.print("CHUNA GAYA: ");
      display.println(lStr);
      display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

      display.setCursor(2, 18);
      display.print("Rupaye: Rs.");
      display.print(cost, 1);
      display.print(" (");
      display.print(rem);
      display.println("s)");

      display.setCursor(2, 34);
      display.println("Apna Card Tap Karein");

      display.setCursor(2, 48);
      display.println("Lal Button = Cancel");
      display.display();
    }

    // Cancel Button
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      beep(1, 100, 700);
      showScreen("CANCELLED", lStr + " Chuna Radd Hua", "Standby Mode", "");
      delay(800);
      return;
    }

    // Switching quantity if another button is pressed:
    if (digitalRead(BTN_1L_PIN) == LOW)  { preselectedLitres = 1.0;  cost = getQuantityCost(1.0);  lStr = "1L";  startWait = millis(); beep(1, 50, 1800); delay(200); }
    if (digitalRead(BTN_2L_PIN) == LOW)  { preselectedLitres = 2.0;  cost = getQuantityCost(2.0);  lStr = "2L";  startWait = millis(); beep(1, 50, 1800); delay(200); }
    if (digitalRead(BTN_5L_PIN) == LOW)  { preselectedLitres = 5.0;  cost = getQuantityCost(5.0);  lStr = "5L";  startWait = millis(); beep(1, 50, 1800); delay(200); }
    if (digitalRead(BTN_10L_PIN) == LOW) { preselectedLitres = 10.0; cost = getQuantityCost(10.0); lStr = "10L"; startWait = millis(); beep(1, 50, 1800); delay(200); }
    if (digitalRead(BTN_15L_PIN) == LOW) { preselectedLitres = 15.0; cost = getQuantityCost(15.0); lStr = "15L"; startWait = millis(); beep(1, 50, 1800); delay(200); }
    if (digitalRead(BTN_20L_PIN) == LOW) { preselectedLitres = 20.0; cost = getQuantityCost(20.0); lStr = "20L"; startWait = millis(); beep(1, 50, 1800); delay(200); }

    // Card Sensed!
    String uid = "";
    if (readCardClean(uid)) {
      handleCardScanned(uid, preselectedLitres);
      return;
    }

    delay(10);
  }

  showScreen("TIME OUT", "Litre Cancel Hua", "Paisa Surakshit", "Rs.0 Kata");
  delay(1200);
}

// =================================================================
// 🌊 WATER DISPENSER CONTROLLER (Accurate Flow & Dry-Run Shield)
// =================================================================
float dispenseWater(String customerName, float litresToDispense) {
  isDispensing = true;
  flowPulseCount = 0;
  unsigned long targetPulses = (unsigned long)(litresToDispense * pulsesPerLitre);

  Serial.println(">>> STARTING WATER DISPENSING <<<");
  beep(2, 80, 1500);

  setMotor(true);
  unsigned long startTime = millis();
  unsigned long lastDisplayUpdate = 0;
  unsigned long lastPulseTime = millis();
  unsigned long lastCheckedPulseCount = 0;

  const unsigned long STARTUP_GRACE_MS = (unsigned long)flowStallSeconds * 1000UL;
  const unsigned long MID_STALL_TIMEOUT_MS = (unsigned long)max(3, flowStallSeconds - 1) * 1000UL;
  bool dryRunDetected = false;

  while (flowPulseCount < targetPulses) {
    unsigned long now = millis();

    if (flowPulseCount > lastCheckedPulseCount) {
      lastCheckedPulseCount = flowPulseCount;
      lastPulseTime = now;
    }

    // Emergency Stop Button during flow
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_CANCEL_PIN) == LOW) {
        Serial.println("🛑 EMERGENCY CANCEL PRESSED!");
        break;
      }
    }

    // Dry Run check: Motor ON but no pulses
    if (now - startTime > STARTUP_GRACE_MS && flowPulseCount < 5) {
      dryRunDetected = true;
      Serial.println("🚨 DRY RUN: No water flow detected!");
      break;
    }

    // Midway Tank Empty
    if (flowPulseCount >= 5 && (now - lastPulseTime > MID_STALL_TIMEOUT_MS)) {
      dryRunDetected = true;
      Serial.println("🚨 TANK EMPTY MIDWAY!");
      break;
    }

    // 10 min safety cutoff
    if (now - startTime > 10UL * 60UL * 1000UL) {
      dryRunDetected = true;
      break;
    }

    // Display progress every 200ms
    if (now - lastDisplayUpdate >= 200) {
      lastDisplayUpdate = now;
      float currentLitres = (float)flowPulseCount / pulsesPerLitre;
      int progressPercent = (int)((currentLitres / litresToDispense) * 100.0);
      if (progressPercent > 100) progressPercent = 100;

      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);

      display.setTextSize(1);
      display.setCursor(4, 2);
      display.println("PANI BHAR RAHA HAI");
      display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

      display.setCursor(0, 16);
      display.println(customerName.substring(0, 18));

      display.setCursor(0, 28);
      display.setTextSize(2);
      display.print(currentLitres, 1);
      display.print(" / ");
      display.print(litresToDispense, 0);
      display.println("L");

      display.setTextSize(1);
      display.drawRect(0, 48, 128, 9, SSD1306_WHITE);
      int barW = (int)((124.0 * progressPercent) / 100.0);
      if (barW > 124) barW = 124;
      display.fillRect(2, 50, barW, 5, SSD1306_WHITE);

      display.display();
    }

    delay(10);
  }

  // Turn Motor OFF
  setMotor(false);
  delay(100);

  float actualLitres = (float)flowPulseCount / pulsesPerLitre;
  Serial.print(">>> DISPENSE FINISHED: ");
  Serial.print(actualLitres, 2);
  Serial.println(" Litres");

  if (dryRunDetected) {
    beep(4, 200, 600);
    showScreen("ALERT: NO WATER!", "Tanki Khali Hai!", "Paisa Wapas Hua", "Pls Check Plant");
    delay(3000);
  } else {
    beep(3, 80, 1800);
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(4, 2);
    display.println("PANI BHAR GAYA!");
    display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

    display.setCursor(0, 18);
    display.println(customerName.substring(0, 18));

    display.setCursor(0, 32);
    display.setTextSize(2);
    display.print(actualLitres, 1);
    display.println(" Litre");

    display.setTextSize(1);
    display.setCursor(0, 52);
    display.println("Shuddh RO Pani | Dhanyawad");
    display.display();
    delay(2500);
  }

  isDispensing = false;
  return actualLitres;
}

// =================================================================
// 📱 CHECK PENDING QR ORDERS & REMOTE STATUS (Every 1.5s)
// =================================================================
bool checkPendingQROrders() {
  if (!wifiConnected || isDispensing) return false;

  // 1. Probe Physical Hardware Silicon
  byte rfidVer = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  bool rfidOk = (rfidVer != 0x00 && rfidVer != 0xFF);
  String rfidVerStr = "0x" + String(rfidVer, HEX);

  Wire.beginTransmission(OLED_ADDR);
  bool oledOk = (Wire.endTransmission() == 0);

  int rssiDbm = WiFi.RSSI();
  String ssidName = WiFi.SSID();
  ssidName.replace(" ", "%20");
  String espIp = WiFi.localIP().toString();
  uint32_t freeRam = ESP.getFreeHeap();
  uint32_t uptimeSec = millis() / 1000;

  String telemetryUrl = "/api/machine/pending?rfid_ok=" + String(rfidOk ? "1" : "0") +
                        "&rfid_ver=" + rfidVerStr +
                        "&oled_ok=" + String(oledOk ? "1" : "0") +
                        "&ssid=" + ssidName +
                        "&rssi=" + String(rssiDbm) +
                        "&esp_ip=" + espIp +
                        "&free_heap=" + String(freeRam) +
                        "&uptime=" + String(uptimeSec);

  HTTPClient http;
  beginHttp(http, telemetryUrl);
  http.setTimeout(1400);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      String mStatus = doc["machine_status"] | "active";
      isMachineInMaintenance = (mStatus == "maintenance");

      if (doc["tank_current_litres"].is<float>()) {
        tankWaterRemaining = doc["tank_current_litres"].as<float>();
      }

      if (doc["price_tiers"].is<JsonObject>()) {
        JsonObject pt = doc["price_tiers"];
        if (pt["1"].is<float>())  price1L  = pt["1"].as<float>();
        if (pt["2"].is<float>())  price2L  = pt["2"].as<float>();
        if (pt["5"].is<float>())  price5L  = pt["5"].as<float>();
        if (pt["10"].is<float>()) price10L = pt["10"].as<float>();
        if (pt["15"].is<float>()) price15L = pt["15"].as<float>();
        if (pt["20"].is<float>()) price20L = pt["20"].as<float>();
      }

      // Sync Auto Mode from Cloud if configured
      if (doc["auto_mode"].is<bool>()) {
        autoModeEnabled = doc["auto_mode"].as<bool>();
        prefs.putBool("auto_mode", autoModeEnabled);
      }
      if (doc["auto_litres"].is<float>()) {
        autoModeLitres = doc["auto_litres"].as<float>();
        prefs.putFloat("auto_l", autoModeLitres);
      }
      if (doc["auto_amount_rs"].is<float>()) {
        autoModeAmountRs = doc["auto_amount_rs"].as<float>();
        prefs.putFloat("auto_rs", autoModeAmountRs);
      }

      if (doc["has_order"] == true && !isMachineInMaintenance) {
        String orderId    = doc["order_id"].as<String>();
        float orderLitres = doc["litres"].as<float>();
        String buyerName  = doc["customer_name"].as<String>();
        float amtRs       = doc["amount_rs"] | 0.0;

        Serial.println("[QR] Payment received!");
        showScreen("QR PAYMENT SUCCESS", "User: " + buyerName, "Amount: Rs." + String(amtRs, 1), "Litres: " + String(orderLitres, 1) + "L");
        beep(2, 150, 1600);
        delay(1000);

        if (tankWaterRemaining <= 2.0) {
          showScreen("TANKI KHALI HAI!", "Pani Uplabdh Nahi", "Paisa Auto-Refund", "24h me wapas");
          beep(3, 200, 600);
          HTTPClient compHttp;
          beginHttp(compHttp, "/api/machine/complete");
          compHttp.addHeader("Content-Type", "application/json");

          JsonDocument failDoc;
          failDoc["order_id"] = orderId;
          failDoc["actual_litres"] = 0.0;
          failDoc["status"] = "failed_no_water";
          failDoc["reason"] = "Tank Khali";
          String failPayload;
          serializeJson(failDoc, failPayload);
          compHttp.POST(failPayload);
          compHttp.end();
          delay(2000);
          return false;
        }

        if (!autoModeEnabled) {
          // Manual Mode: Wait for Green START button
          if (!waitForStartButton(buyerName, orderLitres)) {
            HTTPClient cancelHttp;
            beginHttp(cancelHttp, "/api/machine/complete");
            cancelHttp.addHeader("Content-Type", "application/json");

            JsonDocument cancelDoc;
            cancelDoc["order_id"] = orderId;
            cancelDoc["actual_litres"] = 0.0;
            cancelDoc["status"] = "cancelled_user";
            cancelDoc["reason"] = "Jar Nahi Lagaya / Cancelled";
            String cancelPayload;
            serializeJson(cancelDoc, cancelPayload);
            cancelHttp.POST(cancelPayload);
            cancelHttp.end();
            return false;
          }
        } else {
          // ⚡ AUTO MODE: Direct paani bina kisi button ke!
          showScreen("AUTO DISPENSE", buyerName, "Amount: Rs." + String(amtRs, 1), String(orderLitres, 1) + "L Paani Chalu..");
          beep(2, 60, 1800);
          delay(800);
        }

        float dispensed = dispenseWater(buyerName, orderLitres);

        HTTPClient compHttp;
        beginHttp(compHttp, "/api/machine/complete");
        compHttp.addHeader("Content-Type", "application/json");

        JsonDocument completeDoc;
        completeDoc["order_id"] = orderId;
        completeDoc["actual_litres"] = dispensed;
        completeDoc["status"] = (dispensed >= (orderLitres * 0.5)) ? "completed" : "failed_no_water";
        String compPayload;
        serializeJson(completeDoc, compPayload);
        compHttp.POST(compPayload);
        compHttp.end();

        // 🛡️ Full Post-QR RFID Reset & Antenna Power-Cycle
        rfid.PCD_AntennaOff();
        delay(25);
        rfid.PCD_Init();
        rfid.PCD_AntennaOn();
        rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
        lastScannedUID = "";
        lastScanTime = 0;
        isDispensing = false;
        lastOledRefresh = millis();
        String netStatus = (WiFi.status() == WL_CONNECTED) ? ("WiFi: " + WiFi.SSID().substring(0, 12)) : "Offline Ready";
        if (autoModeEnabled) {
          showScreen("ASHA RO WATER", netStatus, "⚡ AUTO: " + String((int)autoModeLitres) + "L (Rs." + String((int)autoModeAmountRs) + ")", "Scan QR / Tap Card");
        } else {
          showScreen("ASHA RO WATER", netStatus, "Scan QR / Tap Card", "1L|2L|5L|10L|15L|20L");
        }
        beep(1, 60, 1800);

        return true;
      }
    }
  } else {
    http.end();
  }
  return false;
}

// =================================================================
// 🔄 CARD SCAN HANDLER (Online + Seamless Flash Memory Offline)
// =================================================================
void handleCardScanned(String uid, float preselectedLitres) {
  if (isDispensing) return;

  if (isMachineInMaintenance) {
    beep(3, 150, 700);
    showScreen("MACHINE BAND HAI", "Maintenance Mode", "Card Tap Na Karein", "Thodi Der Me Chalu");
    delay(2000);
    return;
  }

  beep(1, 60, 2000);
  String uidClean = uid;
  uidClean.replace(" ", "");

  String nKey = "n_" + uidClean;
  bool isKnownLocally = prefs.isKey(nKey.c_str());
  String custName     = isKnownLocally ? prefs.getString(nKey.c_str(), "Customer") : "Customer";
  float localBal      = isKnownLocally ? prefs.getFloat(("b_" + uidClean).c_str(), 0.0) : 0.0;
  bool isActive       = isKnownLocally ? prefs.getBool(("k_" + uidClean).c_str(), true) : true;

  if (isKnownLocally) {
    showScreen("CARD PEHCHANA", custName, "Bal: Rs." + String(localBal, 1), "Checking...");
  } else {
    showScreen("CARD SCAN HUA", "UID: " + uidClean, "Server pe jaanch...", "Please wait...");
  }

  bool onlineSuccess = (WiFi.status() == WL_CONNECTED);
  float effectiveBal = localBal;

  // ⚡ SUPERFAST: If card is new/unknown locally, query server. If known, respond in 0ms!
  if (!isKnownLocally && WiFi.status() == WL_CONNECTED) {
    showScreen("CARD SCAN HUA", "UID: " + uidClean, "Server pe jaanch...", "Please wait...");
    HTTPClient http;
    beginHttp(http, "/api/card/" + uidClean);
    http.setTimeout(1800);

    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      http.end();

      JsonDocument doc;
      if (!deserializeJson(doc, payload) && doc["success"] == true) {
        custName = doc["data"]["name"].as<String>();
        String status = doc["data"]["status"] | "active";
        isActive = (status != "blocked");
        long balPaise = doc["data"]["balance_paise"] | 0;
        effectiveBal = (float)balPaise / 100.0;
        localBal = effectiveBal;

        prefs.putString(nKey.c_str(), custName);
        prefs.putFloat(("b_" + uidClean).c_str(), localBal);
        prefs.putBool(("k_" + uidClean).c_str(), isActive);
        isKnownLocally = true;
      }
    } else if (httpCode == 404) {
      http.end();
      showScreen("UNREGISTERED CARD", "UID: " + uidClean, "Admin me Card jodein", "");
      beep(3, 120, 700);
      delay(2000);
      return;
    } else {
      http.end();
    }
  }

  if (!isKnownLocally) {
    showScreen("OFFLINE: ANJAAN CARD", "Online WiFi me", "Ek bar tap karein", "");
    beep(2, 150, 700);
    delay(2000);
    return;
  }

  if (!isActive) {
    showScreen("CARD BLOCKED HAI!", custName, "Plant Operator Se", "Sampark Karein");
    beep(3, 180, 700);
    delay(2500);
    return;
  }

  if (tankWaterRemaining <= 2.0) {
    showScreen("TANKI KHALI HAI!", "Pani Uplabdh Nahi", "Bal: Rs." + String(effectiveBal, 1), "Pls Refill Tank");
    beep(3, 200, 600);
    delay(2500);
    return;
  }

  // 🛑 ZERO / INSUFFICIENT BALANCE CHECK (Direct Show & Titik Exit without asking for Litres!)
  if (effectiveBal < price1L) {
    beep(3, 80, 1000); // 3 titik sounds
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(4, 2);
    display.println("BALANCE KHATAM HAI!");
    display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

    display.setCursor(2, 16);
    display.print("Nam: ");
    display.println(custName.substring(0, 14));

    display.setCursor(2, 28);
    display.setTextSize(2);
    display.print("Rs. ");
    display.println(effectiveBal, 2);

    display.setTextSize(1);
    display.setCursor(2, 48);
    display.println("Kripya Recharge Karein");
    display.display();

    delay(2200);
    return;
  }

  float litresToGive = preselectedLitres;
  float costRs = 0.0;

  if (autoModeEnabled && preselectedLitres <= 0.0) {
    // ⚡ AUTO DISPENSE MODE: Bina kisi button dabaye seedhe set paani aur amount!
    litresToGive = autoModeLitres;
    costRs = (autoModeAmountRs > 0.0) ? autoModeAmountRs : getQuantityCost(autoModeLitres);

    if (effectiveBal < costRs) {
      beep(3, 80, 900); // 3 titik sounds
      showScreen("BALANCE KAM HAI!", custName, "Bal: Rs." + String(effectiveBal, 1), "Chahiye: Rs." + String(costRs, 1));
      delay(2200);
      return;
    }

    // Direct approve without waiting for button!
    showScreen("AUTO DISPENSE", custName, "Bal: Rs." + String(effectiveBal - costRs, 1), String(litresToGive, 0) + "L Paani Chalu..");
    beep(2, 60, 1800);
    delay(600);
  } else {
    // Manual Mode: Select quantity button, then wait for Green Start button!
    if (litresToGive <= 0.0) {
      litresToGive = selectCardQuantity(custName, effectiveBal);
      if (litresToGive <= 0.0) {
        return;
      }
    }

    costRs = getQuantityCost(litresToGive);

    if (effectiveBal < costRs) {
      beep(3, 80, 900); // 3 titik sounds
      showScreen("BALANCE KAM HAI!", custName, "Bal: Rs." + String(effectiveBal, 1), "Chahiye: Rs." + String(costRs, 1));
      delay(2200);
      return;
    }

    if (!waitForStartButton(custName, litresToGive)) {
      return;
    }

    showScreen("APPROVED", custName, "Bal: Rs." + String(effectiveBal - costRs, 1), String(litresToGive, 0) + "L Paani Chalu");
    delay(600);
  }

  float afterBal = effectiveBal - costRs;

  if (onlineSuccess) {
    HTTPClient dspHttp;
    
    beginHttp(dspHttp, "/api/dispense");
    dspHttp.addHeader("Content-Type", "application/json");
    dspHttp.setTimeout(2500);

    JsonDocument dspDoc;
    dspDoc["card_uid"] = uidClean;
    dspDoc["litres"] = litresToGive;
    String dspPayload;
    serializeJson(dspDoc, dspPayload);
    dspHttp.POST(dspPayload);
    dspHttp.end();
  } else {
    int offCount = prefs.getInt("off_count", 0);
    prefs.putString(("off_u_" + String(offCount)).c_str(), uidClean);
    prefs.putFloat(("off_l_" + String(offCount)).c_str(), litresToGive);
    prefs.putInt("off_count", offCount + 1);
  }

  prefs.putFloat(("b_" + uidClean).c_str(), afterBal);

  float actualDispensed = dispenseWater(custName, litresToGive);

  if (actualDispensed < 0.2) {
    prefs.putFloat(("b_" + uidClean).c_str(), effectiveBal);
    if (onlineSuccess) {
      HTTPClient refHttp;
      
      beginHttp(refHttp, "/api/card-dispense-refund");
      refHttp.addHeader("Content-Type", "application/json");

      JsonDocument refDoc;
      refDoc["card_uid"] = uidClean;
      refDoc["deducted_litres"] = litresToGive;
      refDoc["actual_litres"] = 0.0;
      String refPayload;
      serializeJson(refDoc, refPayload);
      refHttp.POST(refPayload);
      refHttp.end();
    }
    showScreen("TANK KHALI HAI!", "Paisa Wapas Hua", "Bal: Rs." + String(effectiveBal, 1), "");
    beep(3, 150, 700);
    delay(2500);
  }

  // 🛡️ FULL POST-DISPENSE RFID RECOVERY
  rfid.PCD_AntennaOff();
  delay(25);
  rfid.PCD_Init();
  rfid.PCD_AntennaOn();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);

  // Clear trackers so tapping immediately after dispense is recognized as fresh
  lastScannedUID = "";
  lastScanTime = 0;
  isDispensing = false;

  // Instantly return OLED to Ready Screen
  lastOledRefresh = millis();
  String netStatus = (WiFi.status() == WL_CONNECTED) ? ("WiFi: " + WiFi.SSID().substring(0, 12)) : "Offline Ready";
  if (autoModeEnabled) {
    showScreen("ASHA RO WATER", netStatus, "⚡ AUTO: " + String((int)autoModeLitres) + "L (Rs." + String((int)autoModeAmountRs) + ")", "Scan QR / Tap Card");
  } else {
    showScreen("ASHA RO WATER", netStatus, "Scan QR / Tap Card", "1L|2L|5L|10L|15L|20L");
  }
  beep(1, 60, 1800);
}

// =================================================================
// 📶 SYNC CARDS & OFFLINE QUEUE
// =================================================================
void syncCardsAndOfflineQueue() {
  if (WiFi.status() != WL_CONNECTED || isDispensing) return;

  // 1. Upload offline queue
  int offCount = prefs.getInt("off_count", 0);
  if (offCount > 0) {
    Serial.printf("[SYNC] Uploading %d offline transactions...\n", offCount);
    JsonDocument uploadDoc;
    JsonArray records = uploadDoc["records"].to<JsonArray>();
    for (int i = 0; i < offCount; i++) {
      JsonObject rec = records.add<JsonObject>();
      rec["card_uid"] = prefs.getString(("off_u_" + String(i)).c_str(), "");
      rec["litres"]   = prefs.getFloat(("off_l_" + String(i)).c_str(), 0.0);
    }
    String uploadJson;
    serializeJson(uploadDoc, uploadJson);

    HTTPClient upHttp;
    beginHttp(upHttp, "/api/machine/offline-sync");
    upHttp.addHeader("Content-Type", "application/json");
    upHttp.setTimeout(3000);
    int upCode = upHttp.POST(uploadJson);
    upHttp.end();
    if (upCode == 200) {
      prefs.putInt("off_count", 0);
      Serial.println("[SYNC] Offline queue cleared!");
    }
  }

  // 2. Download authorized cards & tier prices
  HTTPClient syncHttp;
  beginHttp(syncHttp, "/api/machine/sync-cards");
  syncHttp.setTimeout(3500);
  int syncCode = syncHttp.GET();
  if (syncCode == 200) {
    String payload = syncHttp.getString();
    syncHttp.end();

    JsonDocument syncDoc;
    if (!deserializeJson(syncDoc, payload) && syncDoc["success"] == true) {
      if (syncDoc["flow_stall_seconds"].is<int>()) {
        flowStallSeconds = syncDoc["flow_stall_seconds"].as<int>();
        prefs.putInt("stall_sec", flowStallSeconds);
      }
      if (syncDoc["pulses_per_litre"].is<float>()) {
        pulsesPerLitre = syncDoc["pulses_per_litre"].as<float>();
        prefs.putFloat("pulse_per_l", pulsesPerLitre);
      }
      if (syncDoc["price_tiers"].is<JsonObject>()) {
        JsonObject pt = syncDoc["price_tiers"];
        if (pt["1"].is<float>())  { price1L  = pt["1"].as<float>();  prefs.putFloat("p_1l", price1L); }
        if (pt["2"].is<float>())  { price2L  = pt["2"].as<float>();  prefs.putFloat("p_2l", price2L); }
        if (pt["5"].is<float>())  { price5L  = pt["5"].as<float>();  prefs.putFloat("p_5l", price5L); }
        if (pt["10"].is<float>()) { price10L = pt["10"].as<float>(); prefs.putFloat("p_10l", price10L); }
        if (pt["15"].is<float>()) { price15L = pt["15"].as<float>(); prefs.putFloat("p_15l", price15L); }
        if (pt["20"].is<float>()) { price20L = pt["20"].as<float>(); prefs.putFloat("p_20l", price20L); }
      }

      // Sync Auto Mode from Cloud
      if (syncDoc["auto_mode"].is<bool>()) {
        autoModeEnabled = syncDoc["auto_mode"].as<bool>();
        prefs.putBool("auto_mode", autoModeEnabled);
      }
      if (syncDoc["auto_litres"].is<float>()) {
        autoModeLitres = syncDoc["auto_litres"].as<float>();
        prefs.putFloat("auto_l", autoModeLitres);
      }
      if (syncDoc["auto_amount_rs"].is<float>()) {
        autoModeAmountRs = syncDoc["auto_amount_rs"].as<float>();
        prefs.putFloat("auto_rs", autoModeAmountRs);
      }

      JsonArray cards = syncDoc["cards"].as<JsonArray>();
      for (JsonObject c : cards) {
        String cUid = c["uid"].as<String>();
        String cName = c["name"].as<String>();
        float cBal = c["balance_rs"].as<float>();
        String cStatus = c["status"] | "active";
        bool cActive = (cStatus != "blocked");
        prefs.putString(("n_" + cUid).c_str(), cName);
        prefs.putFloat(("b_" + cUid).c_str(), cBal);
        prefs.putBool(("k_" + cUid).c_str(), cActive);
      }
      Serial.printf("[SYNC] Synced %d cards to flash!\n", cards.size());
    }
  } else {
    syncHttp.end();
  }
}

// =================================================================
// 🚀 ARDUINO SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("=== ASHA RO WATER SYSTEM v2.5 STARTING ===");

  // 0. Preferences Flash Memory
  prefs.begin("asha_ro", false);
  pricePerLitreRs  = prefs.getFloat("price_per_l", 1.0);
  flowStallSeconds = prefs.getInt("stall_sec", 6);
  pulsesPerLitre   = prefs.getFloat("pulse_per_l", 450.0);
  price1L  = prefs.getFloat("p_1l", 1.0);
  price2L  = prefs.getFloat("p_2l", 2.0);
  price5L  = prefs.getFloat("p_5l", 5.0);
  price10L = prefs.getFloat("p_10l", 10.0);
  price15L = prefs.getFloat("p_15l", 15.0);
  price20L = prefs.getFloat("p_20l", 15.0);
  autoModeEnabled  = prefs.getBool("auto_mode", false);
  autoModeLitres   = prefs.getFloat("auto_l", 20.0);
  autoModeAmountRs = prefs.getFloat("auto_rs", 15.0);

  // 1. Relay & Buzzer
  pinMode(RELAY_PIN, OUTPUT);
  setMotor(false);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // 2. Flow Sensor
  pinMode(FLOW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, RISING);

  // 3. Push Buttons (Internal Pullups) & Start LED
  pinMode(BTN_START_PIN, INPUT_PULLUP);
  pinMode(BTN_CANCEL_PIN, INPUT_PULLUP);
  pinMode(LED_START_PIN, OUTPUT);
  digitalWrite(LED_START_PIN, LOW);

  pinMode(BTN_1L_PIN, INPUT_PULLUP);
  pinMode(BTN_2L_PIN, INPUT_PULLUP);
  pinMode(BTN_5L_PIN, INPUT_PULLUP);
  pinMode(BTN_10L_PIN, INPUT_PULLUP);
  pinMode(BTN_15L_PIN, INPUT_PULLUP);
  pinMode(BTN_20L_PIN, INPUT_PULLUP);
  pinMode(BTN_BAL_PIN, INPUT_PULLUP);

  // 4. OLED Display
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(400000); // 400kHz Fast I2C Mode!
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  showScreen("ASHA RO WATER", "Smart Dispenser", "Starting System...", "Please wait...");
  beep(1, 100, 1600);

  // 5. Setup RFID (RC522) with MAXIMUM ANTENNA GAIN (48dB)
  SPI.begin(18, 19, 23, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(30);
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  Serial.println("✅ RFID RC522 initialized with MAX Antenna Gain (48dB)!");

  // 6. Connect WiFi
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("AJAY KUSHWAHA", "77777777");            // Home Mobile Hotspot
  wifiMulti.addAP("Airtel_Asha Digital Center", "Ajay@6341#"); // Shop / Digital Center Wi-Fi

  int wifiTries = 0;
  while (wifiMulti.run() != WL_CONNECTED && wifiTries < 12) {
    delay(200);
    wifiTries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    showScreen("WIFI CONNECTED", WiFi.SSID().substring(0, 14), WiFi.localIP().toString(), "System Ready!");
    beep(2, 60, 1800);
    syncCardsAndOfflineQueue();
  } else {
    wifiConnected = false;
    showScreen("STANDALONE MODE", "Direct Card Access", "System Ready!", "");
    beep(1, 150, 1000);
  }
  delay(800);
}

// =================================================================
// 🔘 HARDWARE AUTO MODE CONFIGURATION MENU
// Long-press Balance Button (2 sec) toggles Auto Mode ON / OFF.
// When ON: Press 1L..20L button to set auto litres, Green/Red to adjust Rs.
// =================================================================
void handleAutoModeConfigMenu() {
  autoModeEnabled = !autoModeEnabled;
  prefs.putBool("auto_mode", autoModeEnabled);

  if (autoModeEnabled) {
    beep(2, 100, 1800); // 2 high happy beeps
  } else {
    beep(1, 200, 700);  // 1 low beep
  }

  unsigned long menuStart = millis();
  while (millis() - menuStart < 5000) { // 5-second interactive window
    unsigned long rem = 5 - ((millis() - menuStart) / 1000);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(4, 2);
    display.println("AUTO DISPENSE SETUP");
    display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

    display.setCursor(2, 16);
    display.print("Status: ");
    if (autoModeEnabled) {
      display.println("CHALU (ON)");
    } else {
      display.println("BAND (OFF)");
    }

    display.setCursor(2, 30);
    display.print("Set: ");
    display.print((int)autoModeLitres);
    display.print("L = Rs.");
    display.print((int)autoModeAmountRs);
    display.print(" (");
    display.print(rem);
    display.println("s)");

    display.setCursor(2, 46);
    display.println("1-20L=Badle | Lal=OK");
    display.display();

    // Check quantity buttons to adjust Auto Litres
    if (digitalRead(BTN_1L_PIN) == LOW)  { autoModeLitres = 1.0;  autoModeAmountRs = price1L;  beep(1, 50, 1800); delay(200); menuStart = millis(); }
    if (digitalRead(BTN_2L_PIN) == LOW)  { autoModeLitres = 2.0;  autoModeAmountRs = price2L;  beep(1, 50, 1800); delay(200); menuStart = millis(); }
    if (digitalRead(BTN_5L_PIN) == LOW)  { autoModeLitres = 5.0;  autoModeAmountRs = price5L;  beep(1, 50, 1800); delay(200); menuStart = millis(); }
    if (digitalRead(BTN_10L_PIN) == LOW) { autoModeLitres = 10.0; autoModeAmountRs = price10L; beep(1, 50, 1800); delay(200); menuStart = millis(); }
    if (digitalRead(BTN_15L_PIN) == LOW) { autoModeLitres = 15.0; autoModeAmountRs = price15L; beep(1, 50, 1800); delay(200); menuStart = millis(); }
    if (digitalRead(BTN_20L_PIN) == LOW) { autoModeLitres = 20.0; autoModeAmountRs = price20L; beep(1, 50, 1800); delay(200); menuStart = millis(); }

    // Start Button: +1 Rs
    if (digitalRead(BTN_START_PIN) == LOW) {
      autoModeAmountRs += 1.0;
      beep(1, 50, 2000);
      delay(200);
      menuStart = millis();
    }

    // Cancel Button: Save & Exit immediately
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      break;
    }

    delay(20);
  }

  prefs.putFloat("auto_l", autoModeLitres);
  prefs.putFloat("auto_rs", autoModeAmountRs);

  showScreen("SETTING SAVED", autoModeEnabled ? "AUTO MODE: ON" : "AUTO MODE: OFF", String((int)autoModeLitres) + "L = Rs." + String((int)autoModeAmountRs), "Surakshit Save Hua");
  beep(2, 80, 1600);
  delay(1200);
}

// =================================================================
// 🔄 LIGHTNING-FAST MAIN LOOP
// =================================================================
void loop() {
  if (isDispensing) return;

  // 🛠️ REMOTE MAINTENANCE / OFFLINE MODE
  if (isMachineInMaintenance) {
    unsigned long now = millis();
    if (now - lastOledRefresh >= 3000) {
      lastOledRefresh = now;
      showScreen("ASHA RO WATER", "MACHINE ABHI BAND", "HAI (OFFLINE)", "Maintenance Mode");
    }
    if (now - lastQRPollTime >= 1500) {
      lastQRPollTime = now;
      checkPendingQROrders();
    }
    String mUid = "";
    if (readCardClean(mUid)) {
      beep(3, 150, 700);
      showScreen("MACHINE BAND HAI", "Maintenance Chalu Hai", "Card Tap Na Karein", "Thodi Der Me Chalu");
      delay(2000);
    }
    delay(10);
    return;
  }

  // ⚡ 1. RFID SENSING (Clean 100% Detection with REQA + WUPA Fallback)
  String scannedUid = "";
  if (readCardClean(scannedUid)) {
    unsigned long now = millis();
    if (scannedUid != lastScannedUID || (now - lastScanTime) > 1000) {
      lastScannedUID = scannedUid;
      lastScanTime = now;
      handleCardScanned(scannedUid);
    }
    return;
  } else {
    // When no card is present for 400ms, reset lastScannedUID so immediate re-tap works on 1st touch!
    if (millis() - lastScanTime > 400) {
      lastScannedUID = "";
    }
  }

  // 🔘 2. CARD BALANCE ENQUIRY & AUTO MODE TOGGLE BUTTON (Pin D25)
  // Short Press (<2s) = Balance Enquiry
  // Long Press (>=2s)  = Toggle Auto Mode & Configure Litres/Rs
  if (digitalRead(BTN_BAL_PIN) == LOW) {
    delay(40);
    if (digitalRead(BTN_BAL_PIN) == LOW) {
      unsigned long pressStart = millis();
      bool longPressed = false;
      while (digitalRead(BTN_BAL_PIN) == LOW) {
        if (millis() - pressStart >= 2000) {
          longPressed = true;
          handleAutoModeConfigMenu();
          break;
        }
        delay(20);
      }
      if (!longPressed) {
        handleBalanceEnquiry();
      }
      return;
    }
  }

  // 🔘 3. PRE-SELECTION QUANTITY BUTTONS (1L, 2L, 5L, 10L, 15L, 20L)
  if (digitalRead(BTN_1L_PIN) == LOW)  { handlePreSelection(1.0);  return; }
  if (digitalRead(BTN_2L_PIN) == LOW)  { handlePreSelection(2.0);  return; }
  if (digitalRead(BTN_5L_PIN) == LOW)  { handlePreSelection(5.0);  return; }
  if (digitalRead(BTN_10L_PIN) == LOW) { handlePreSelection(10.0); return; }
  if (digitalRead(BTN_15L_PIN) == LOW) { handlePreSelection(15.0); return; }
  if (digitalRead(BTN_20L_PIN) == LOW) { handlePreSelection(20.0); return; }

  unsigned long now = millis();

  // 4. WiFi Status Check every 8 seconds (non-blocking)
  checkWiFiConnection();

  // 5. Fast QR Order Poll every 1.5 seconds (Lightning-Fast Response!)
  if (now - lastQRPollTime >= 2500) {
    lastQRPollTime = now;
    checkPendingQROrders();
  }

  // 6. Background Sync every 30 seconds
  if (now - lastCardSyncTime >= 30000) {
    lastCardSyncTime = now;
    syncCardsAndOfflineQueue();
  }

  // 7. Ready Screen Refresh every 4 seconds
  if (now - lastOledRefresh >= 4000) {
    lastOledRefresh = now;
    String netStatus = (WiFi.status() == WL_CONNECTED) ? ("WiFi: " + WiFi.SSID().substring(0, 12)) : "Offline Ready";
    if (autoModeEnabled) {
      showScreen("ASHA RO WATER", netStatus, "⚡ AUTO: " + String((int)autoModeLitres) + "L (Rs." + String((int)autoModeAmountRs) + ")", "Scan QR / Tap Card");
    } else {
      showScreen("ASHA RO WATER", netStatus, "Scan QR / Tap Card", "1L|2L|5L|10L|15L|20L");
    }
  }

  delay(5);
}
