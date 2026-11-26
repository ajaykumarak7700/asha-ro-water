/**
 * =================================================================
 *        🚰 ASHA RO WATER - SMART WATER DISPENSER SYSTEM
 * =================================================================
 * 
 * Features:
 *   1. 📱 QR Code UPI Payment:
 *      Jitna rupaye pay kiya, website rate ke hisab se utna Litre paani.
 * 
 *   2. 🔘 Smart Start Button & Jar Safety:
 *      Payment ya card ke baad pani turant nahi girega;
 *      Green Button dabane par hi pani chalu hoga (Jar waste protection).
 *      Red Cancel button dabane par refund/cancel.
 *      Admin toggle: 'button' ya 'auto'.
 * 
 *   3. 🔘 Multi-Quantity Buttons (1L, 5L, 10L, 20L):
 *      Card scan karne par customer manchahe pani ka button daba sakega
 *      aur utna hi balance katega. Admin toggle: on / off.
 * 
 *   4. 💳 RFID Card Tap Dispense:
 *      Online & Offline mode me balance check & auto-sync.
 * 
 *   5. 🖥️ Live OLED Display & Buzzer:
 *      Real-time Litre progress bar, user name, balance, beep sounds.
 * 
 *   6. 🌊 ZJ-S201 Flow Sensor (K-Factor Pulse Calibration) & 12V Relay:
 *      Pin D34 se pulse count, Pin D26 se Relay ON/OFF.
 * =================================================================
 */

#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

WiFiMulti wifiMulti;
Preferences prefs;

// =================================================================
// ⚙️ USER CONFIGURATION
// =================================================================
const char* SERVER_IP     = "192.168.1.8";     // Web Server IP (Computer LAN IP)
const int   SERVER_PORT   = 3000;              // Server Port

float defaultCardLitres   = 2.0;               // Default card dispense quantity
float pricePerLitreRs     = 1.0;               // Price per Litre (Server se auto sync)
int   flowStallSeconds    = 6;                 // Flow stall & dry-run timeout in seconds (Admin sync)
float pulsesPerLitre      = 450.0;             // ZJ-S201 Sensor calibration pulses/L (Admin sync)
const bool  RELAY_ACTIVE_LOW = false;          // False: Active HIGH relay (HIGH par relay ON, LOW par OFF)

// =================================================================
// 📌 PIN DEFINITIONS
// =================================================================
#define RFID_SS_PIN   5
#define RFID_RST_PIN  22

#define OLED_SDA_PIN  21
#define OLED_SCL_PIN  15
#define OLED_ADDR     0x3C

#define BUZZER_PIN    13
#define FLOW_PIN      34
#define RELAY_PIN     26

// 🔘 PUSH BUTTONS & INDICATOR LED PINS
#define BTN_START_PIN      4    // Green Push Button (Start Water Dispense)
#define BTN_CANCEL_PIN     16   // Red Push Button (Cancel / Emergency Stop)
#define LED_START_PIN      25   // Green LED Indicator for Start Button (Active HIGH)

#define BTN_1L_PIN         17   // 1 Litre Select Button (Active LOW with internal PULLUP)
#define BTN_5L_PIN         27   // 5 Litres Select Button (Active LOW with internal PULLUP)
#define BTN_10L_PIN        32   // 10 Litres Select Button (Active LOW with internal PULLUP)
#define BTN_20L_PIN        33   // 20 Litres Select Button (Active LOW with internal PULLUP)

// Configuration variables (Synced from Admin Dashboard)
String dispenseMode        = "button"; // "button" (wait for Start press) or "auto" (immediate)
int    waitButtonSeconds   = 60;       // Seconds to wait for Start button before auto-cancelling
bool   multiQuantityEnable = true;     // 1L, 5L, 10L, 20L selection buttons active

// Quantity-Wise Tiered Prices (Admin sync - e.g. 20L @ Rs.15)
float price1L              = 1.0;
float price5L              = 5.0;
float price10L             = 10.0;
float price20L             = 15.0;

float getQuantityCost(float litres) {
  if (litres == 1.0)  return price1L;
  if (litres == 5.0)  return price5L;
  if (litres == 10.0) return price10L;
  if (litres == 20.0) return price20L;
  return litres * pricePerLitreRs;
}

// Objects
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Flow sensor variables
volatile unsigned long flowPulseCount = 0;
void IRAM_ATTR flowPulseISR() {
  flowPulseCount++;
}

// State tracking
bool isDispensing = false;
String lastScannedUID = "";
unsigned long lastScanTime = 0;
bool wifiConnected = false;
unsigned long lastQRPollTime = 0;

// =================================================================
// 🔔 BUZZER & SOUND FUNCTIONS
// =================================================================
void beep(int count, int durationMs, int freq = 1200) {
  for (int i = 0; i < count; i++) {
    tone(BUZZER_PIN, freq, durationMs);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(70);
  }
}

// =================================================================
// ⚡ RELAY CONTROLLER
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
// 🌐 WIFI HELPER (Multi-Hotspot: Home Mobile + Shop Wi-Fi)
// =================================================================
void checkWiFiConnection() {
  if (wifiMulti.run() == WL_CONNECTED) {
    wifiConnected = true;
  } else {
    wifiConnected = false;
  }
}

// =================================================================
// 🔘 WAIT FOR START BUTTON PRESS (Jar Placement Protection)
// Returns true: Start pressed (or mode is auto). Returns false: Cancel pressed or Timeout.
// =================================================================
bool waitForStartButton(String promptTitle, float litresToDispense) {
  if (dispenseMode == "auto") {
    return true; // Auto mode: start immediately without waiting
  }

  Serial.println("\n⏳ WAITING FOR START BUTTON (JAR PLACEMENT)...");
  unsigned long startWaitTime = millis();
  unsigned long lastBlinkTime = 0;
  bool ledState = false;
  unsigned long lastBeepTime = millis();

  while (true) {
    unsigned long now = millis();
    unsigned long elapsedSec = (now - startWaitTime) / 1000;
    long remainSec = waitButtonSeconds - elapsedSec;

    if (remainSec <= 0) {
      // Timeout: Jar not placed in time
      digitalWrite(LED_START_PIN, LOW);
      Serial.println("⚠️ TIMEOUT: Start button not pressed in time.");
      showScreen("TIME KHATAM!", "Jar Nahi Lagaya", "Order Radd Hua", "Paisa Wapas / Refund");
      beep(3, 200, 700);
      delay(2500);
      return false;
    }

    // Blink Green LED on Start button
    if (now - lastBlinkTime >= 300) {
      lastBlinkTime = now;
      ledState = !ledState;
      digitalWrite(LED_START_PIN, ledState ? HIGH : LOW);
    }

    // Soft alert beep every 3.5 seconds
    if (now - lastBeepTime >= 3500) {
      lastBeepTime = now;
      beep(1, 40, 1800);
    }

    // Update OLED Prompt
    static unsigned long lastOledRefresh = 0;
    if (now - lastOledRefresh >= 300) {
      lastOledRefresh = now;
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
      display.print("Lal=Radd (Bacha ");
      display.print(remainSec);
      display.println("s)");
      display.display();
    }

    // Check CANCEL Button (GPIO 16)
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      delay(50); // Debounce
      if (digitalRead(BTN_CANCEL_PIN) == LOW) {
        digitalWrite(LED_START_PIN, LOW);
        Serial.println("❌ USER CANCELLED DISPENSE!");
        showScreen("RADD KIYA GAYA!", "Lal Button Dabaya", "Order Cancelled", "Paisa Surakshit");
        beep(3, 150, 700);
        delay(2200);
        return false;
      }
    }

    // Check START Button (GPIO 4)
    if (digitalRead(BTN_START_PIN) == LOW) {
      delay(50); // Debounce
      if (digitalRead(BTN_START_PIN) == LOW) {
        digitalWrite(LED_START_PIN, HIGH); // Solid ON
        Serial.println("✅ START BUTTON PRESSED! Jar Confirmed.");
        beep(2, 80, 1600);
        showScreen("JAR CONFIRMED", "Pani Chalu Ho Raha..", String(litresToDispense, 1) + " Litres", "Starting Motor...");
        delay(800);
        digitalWrite(LED_START_PIN, LOW);
        return true;
      }
    }

    delay(20);
  }
}

// =================================================================
// 🔘 CARD QUANTITY SELECTION (1L, 5L, 10L, 20L Push Buttons)
// Returns chosen litres. If 0.0 -> Cancelled by user.
// =================================================================
float selectCardQuantity(String custName, float balanceRs) {
  if (!multiQuantityEnable) {
    return defaultCardLitres; // Admin disabled multi-quantity
  }

  Serial.println("\n🔘 WAITING FOR QUANTITY SELECTION (1L, 5L, 10L, 20L)...");
  unsigned long startWait = millis();

  while (millis() - startWait < 8000) { // 8 seconds selection window
    unsigned long remainSec = 8 - ((millis() - startWait) / 1000);

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
      display.println("1L | 5L | 10L | 20L");

      display.setCursor(2, 46);
      display.println("Button Chunein (Lal=X)");
      display.display();
    }

    // Cancel Button
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_CANCEL_PIN) == LOW) {
        showScreen("RADD KIYA GAYA", custName, "Card Process Cancel", "");
        beep(2, 120, 700);
        delay(1500);
        return 0.0;
      }
    }

    // 1 Litre Button
    if (digitalRead(BTN_1L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_1L_PIN) == LOW) {
        beep(1, 100, 1800);
        showScreen("CHUNA: 1 LITER", custName, "Rupaye: Rs." + String(price1L, 1), "1L Selected");
        delay(900);
        return 1.0;
      }
    }

    // 5 Litres Button
    if (digitalRead(BTN_5L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_5L_PIN) == LOW) {
        beep(1, 100, 1800);
        showScreen("CHUNA: 5 LITER", custName, "Rupaye: Rs." + String(price5L, 1), "5L Selected");
        delay(900);
        return 5.0;
      }
    }

    // 10 Litres Button
    if (digitalRead(BTN_10L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_10L_PIN) == LOW) {
        beep(1, 100, 1800);
        showScreen("CHUNA: 10 LITER", custName, "Rupaye: Rs." + String(price10L, 1), "10L Selected");
        delay(900);
        return 10.0;
      }
    }

    // 20 Litres Button
    if (digitalRead(BTN_20L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_20L_PIN) == LOW) {
        beep(1, 100, 1800);
        showScreen("CHUNA: 20 LITER", custName, "Offer: Rs." + String(price20L, 1), "20L Jar Offer!");
        delay(900);
        return 20.0;
      }
    }

    delay(20);
  }

  // Timeout -> defaultCardLitres
  return defaultCardLitres;
}

// =================================================================
// 🌊 WATER DISPENSER CONTROLLER (Accurate Flow Calibration & Dry-Run Shield)
// =================================================================
float dispenseWater(String customerName, float litresToDispense) {
  isDispensing = true;
  flowPulseCount = 0;
  unsigned long targetPulses = (unsigned long)(litresToDispense * pulsesPerLitre);

  Serial.println("\n>>> STARTING WATER DISPENSING <<<");
  Serial.print("Customer: "); Serial.println(customerName);
  Serial.print("Litres Target: "); Serial.println(litresToDispense);
  Serial.print("Pulses Per Litre: "); Serial.println(pulsesPerLitre);
  Serial.print("Target Pulses: "); Serial.println(targetPulses);

  // Beep start confirmation
  beep(2, 100, 1500);

  // Turn Motor ON
  setMotor(true);
  unsigned long startTime = millis();
  unsigned long lastDisplayUpdate = 0;
  unsigned long lastPulseTime = millis();
  unsigned long lastCheckedPulseCount = 0;

  // 🛡️ DRY-RUN / NO-WATER FLOW PROTECTION (Admin Configurable)
  const unsigned long STARTUP_GRACE_MS = (unsigned long)flowStallSeconds * 1000UL;
  const unsigned long MID_STALL_TIMEOUT_MS = (unsigned long)max(3, flowStallSeconds - 1) * 1000UL;
  bool dryRunDetected = false;

  while (flowPulseCount < targetPulses) {
    unsigned long now = millis();

    // Track pulse activity
    if (flowPulseCount > lastCheckedPulseCount) {
      lastCheckedPulseCount = flowPulseCount;
      lastPulseTime = now;
    }

    // 🛑 Emergency Cancel Button pressed while water is flowing
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_CANCEL_PIN) == LOW) {
        Serial.println("🛑 EMERGENCY CANCEL PRESSED! Stopping pump immediately.");
        break;
      }
    }

    // 1. Initial Dry-Run Check: Motor ON for >flowStallSeconds but no pulses (<5 pulses)!
    if (now - startTime > STARTUP_GRACE_MS && flowPulseCount < 5) {
      dryRunDetected = true;
      Serial.print("🚨 DRY-RUN DETECTED! No water flow in first ");
      Serial.print(flowStallSeconds);
      Serial.println("s. Motor stopped immediately to protect pump!");
      break;
    }

    // 2. Midway Tank Empty Check: Water was flowing, but stopped for >MID_STALL_TIMEOUT_MS!
    if (flowPulseCount >= 5 && (now - lastPulseTime > MID_STALL_TIMEOUT_MS)) {
      dryRunDetected = true;
      Serial.println("🚨 TANK EMPTY MIDWAY! Water flow stopped. Motor stopped to prevent dry running!");
      break;
    }

    // 3. Safety timeout: 10 minutes max
    if (now - startTime > 10UL * 60UL * 1000UL) {
      dryRunDetected = true;
      Serial.println("⚠️ Safety timeout reached! Stopping motor.");
      break;
    }

    // Update screen every 250ms
    if (now - lastDisplayUpdate >= 250) {
      lastDisplayUpdate = now;
      float currentLitres = (float)flowPulseCount / pulsesPerLitre;
      int progressPercent = (int)((currentLitres / litresToDispense) * 100.0);
      if (progressPercent > 100) progressPercent = 100;

      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);

      // Title
      display.setTextSize(1);
      display.setCursor(4, 2);
      display.println("PANI BHAR RAHA HAI");
      display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

      // Customer name
      display.setCursor(0, 16);
      display.print("User: ");
      display.println(customerName.substring(0, 14));

      // Litre counter (Large Text)
      display.setTextSize(2);
      display.setCursor(4, 28);
      display.print(currentLitres, 1);
      display.print("/");
      display.print(litresToDispense, 1);
      display.print("L");

      // Progress bar outline
      display.drawRect(0, 48, 128, 12, SSD1306_WHITE);
      // Progress bar fill
      int fillWidth = (progressPercent * 124) / 100;
      if (fillWidth > 0) {
        display.fillRect(2, 50, fillWidth, 8, SSD1306_WHITE);
      }

      display.display();
    }

    yield(); // keep ESP32 watchdog happy
  }

  // Turn Motor OFF immediately
  setMotor(false);
  isDispensing = false;
  float actualLitres = (float)flowPulseCount / pulsesPerLitre;

  if (dryRunDetected) {
    Serial.println(">>> ❌ DRY RUN: PUMP SHUTDOWN (TANK EMPTY OR MOTOR ERROR) <<<");
    Serial.print("Actual Litres Dispensed: "); Serial.println(actualLitres, 2);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(6, 4);
    display.println("! TANK KHALI HAI !");
    display.drawFastHLine(0, 15, 128, SSD1306_WHITE);

    display.setCursor(4, 20);
    display.println("Paani Nahi Nikla");
    display.setCursor(4, 34);
    display.print("Nikla: ");
    display.print(actualLitres, 1);
    display.println(" Litre");

    display.setCursor(4, 48);
    display.println("Paisa Wapas Hoga");
    display.display();

    // 4 warning beeps
    beep(4, 150, 750);
    delay(4000);
    return actualLitres;
  }

  Serial.println(">>> ✅ DISPENSE COMPLETE! MOTOR OFF <<<");

  // Success screen
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(8, 8);
  display.print(litresToDispense, 1);
  display.println(" LITER PURA HUA!");

  display.setTextSize(2);
  display.setCursor(14, 24);
  display.println("THANK YOU");

  display.setTextSize(1);
  display.setCursor(12, 48);
  display.println("Apna Jar Le Lijie");
  display.display();

  // Long success beep
  beep(1, 1000, 1800);
  delay(3500);
  return actualLitres;
}

// =================================================================
// 📱 CHECK PENDING QR ORDERS FROM SERVER
// =================================================================
void checkPendingQROrders() {
  if (!wifiConnected || isDispensing) return;

  HTTPClient http;
  String checkUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/pending";
  http.begin(checkUrl);
  http.setTimeout(2500);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["has_order"] == true) {
      String orderId    = doc["order_id"].as<String>();
      float orderLitres = doc["litres"].as<float>();
      String buyerName  = doc["customer_name"].as<String>();
      float amtRs       = doc["amount_rs"] | 0.0;

      Serial.println("\n🎉 QR PAYMENT RECEIVED!");
      Serial.print("Order: "); Serial.println(orderId);
      Serial.print("Amount: ₹"); Serial.println(amtRs);
      Serial.print("Litres: "); Serial.println(orderLitres);

      showScreen("QR PAYMENT SUCCESS", "User: " + buyerName, "Amount: Rs." + String(amtRs, 1), "Litres: " + String(orderLitres, 1) + "L");
      beep(2, 200, 1600);
      delay(1500);

      // 🔘 JAR SAFETY PROTECTION: Wait for user to place jar & press Green Button!
      bool startPressed = waitForStartButton(buyerName, orderLitres);

      if (!startPressed) {
        // Order Cancelled or Timeout: Notify server to auto-refund buyer!
        Serial.println("❌ QR Order cancelled before dispensing. Requesting auto-refund...");
        HTTPClient cancelHttp;
        String cancelUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/complete";
        cancelHttp.begin(cancelUrl);
        cancelHttp.addHeader("Content-Type", "application/json");
        String postBody = "{\"order_id\":\"" + orderId + "\",\"actual_litres\":0.0,\"status\":\"cancelled_user\"}";
        cancelHttp.POST(postBody);
        cancelHttp.end();

        showScreen("ORDER CANCELLED", "Jar Nahi Lagaya Gaya", "Paisa Turant Refund!", "");
        beep(3, 200, 700);
        delay(3000);
        return;
      }

      // Dispense water for QR order (with Dry-Run Auto-Detection)
      float actualDispensed = dispenseWater(buyerName, orderLitres);

      // Notify Server Order Completed or Failed (Tank Dry / Motor Error)
      HTTPClient doneHttp;
      String doneUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/complete";
      doneHttp.begin(doneUrl);
      doneHttp.addHeader("Content-Type", "application/json");

      String orderStatus = (actualDispensed >= (orderLitres * 0.8)) ? "completed" : "failed_no_water";
      String postBody = "{\"order_id\":\"" + orderId + "\",\"actual_litres\":\"" + String(actualDispensed, 1) + "\",\"status\":\"" + orderStatus + "\"}";
      doneHttp.POST(postBody);
      doneHttp.end();
    }
  } else {
    http.end();
  }
}

// =================================================================
// 📶 OFFLINE MEMORY & AUTO-SYNC ENGINE (Preferences NVS Flash)
// =================================================================
unsigned long lastCardSyncTime = 0;

void syncCardsAndOfflineQueue() {
  if (!wifiConnected) return;

  // 1. Upload any pending offline dispenses first!
  int offCount = prefs.getInt("off_count", 0);
  if (offCount > 0) {
    Serial.print("\n📶 Syncing "); Serial.print(offCount); Serial.println(" offline dispenses to server...");
    JsonDocument syncDoc;
    JsonArray records = syncDoc["records"].to<JsonArray>();

    for (int i = 0; i < offCount; i++) {
      String uKey = "off_u_" + String(i);
      String lKey = "off_l_" + String(i);
      String cardU = prefs.getString(uKey.c_str(), "");
      float lit = prefs.getFloat(lKey.c_str(), defaultCardLitres);
      if (cardU.length() > 0) {
        JsonObject obj = records.add<JsonObject>();
        obj["card_uid"] = cardU;
        obj["litres"] = lit;
      }
    }

    String syncPayload;
    serializeJson(syncDoc, syncPayload);

    HTTPClient syncHttp;
    String syncUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/offline-sync";
    syncHttp.begin(syncUrl);
    syncHttp.addHeader("Content-Type", "application/json");
    int syncRes = syncHttp.POST(syncPayload);
    syncHttp.end();

    if (syncRes == 200) {
      Serial.println("✅ Offline records synced successfully. Clearing local queue.");
      for (int i = 0; i < offCount; i++) {
        prefs.remove(("off_u_" + String(i)).c_str());
        prefs.remove(("off_l_" + String(i)).c_str());
      }
      prefs.putInt("off_count", 0);
    }
  }

  // 2. Download latest cards & settings from Server
  HTTPClient getHttp;
  String getUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/sync-cards";
  getHttp.begin(getUrl);
  getHttp.setTimeout(3500);

  int getCode = getHttp.GET();
  if (getCode == 200) {
    String payload = getHttp.getString();
    getHttp.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["success"] == true) {
      if (doc["default_card_litres"]) defaultCardLitres = doc["default_card_litres"].as<float>();
      if (doc["price_per_litre_rs"]) pricePerLitreRs = doc["price_per_litre_rs"].as<float>();
      if (doc["flow_stall_seconds"]) {
        flowStallSeconds = doc["flow_stall_seconds"].as<int>();
        if (flowStallSeconds < 2) flowStallSeconds = 2;
        if (flowStallSeconds > 60) flowStallSeconds = 60;
        prefs.putInt("stall_sec", flowStallSeconds);
      }
      if (doc["pulses_per_litre"]) {
        pulsesPerLitre = doc["pulses_per_litre"].as<float>();
        if (pulsesPerLitre < 50.0) pulsesPerLitre = 50.0;
        if (pulsesPerLitre > 2000.0) pulsesPerLitre = 2000.0;
        prefs.putFloat("pulse_per_l", pulsesPerLitre);
      }
      if (doc["dispense_mode"]) {
        dispenseMode = doc["dispense_mode"].as<String>();
        prefs.putString("disp_mode", dispenseMode);
      }
      if (doc["wait_button_seconds"]) {
        waitButtonSeconds = doc["wait_button_seconds"].as<int>();
        prefs.putInt("wait_btn_sec", waitButtonSeconds);
      }
      if (doc.containsKey("multi_quantity_enabled")) {
        multiQuantityEnable = doc["multi_quantity_enabled"].as<bool>();
        prefs.putBool("multi_qty", multiQuantityEnable);
      }
      if (doc["price_tiers"]) {
        JsonObject pt = doc["price_tiers"].as<JsonObject>();
        if (pt["1"]) price1L = pt["1"].as<float>();
        if (pt["5"]) price5L = pt["5"].as<float>();
        if (pt["10"]) price10L = pt["10"].as<float>();
        if (pt["20"]) price20L = pt["20"].as<float>();
        prefs.putFloat("p_1l", price1L);
        prefs.putFloat("p_5l", price5L);
        prefs.putFloat("p_10l", price10L);
        prefs.putFloat("p_20l", price20L);
      }

      prefs.putFloat("def_litres", defaultCardLitres);
      prefs.putFloat("price_per_l", pricePerLitreRs);

      JsonArray cards = doc["cards"].as<JsonArray>();
      for (JsonObject c : cards) {
        String u = c["uid"].as<String>();
        String name = c["name"].as<String>();
        float bal = c["balance_rs"].as<float>();
        bool active = (c["status"].as<String>() != "blocked");
        bool offAllowed = c["allow_offline"] | true;

        prefs.putString(("n_" + u).c_str(), name);
        prefs.putFloat(("b_" + u).c_str(), bal);
        prefs.putBool(("k_" + u).c_str(), active);
        prefs.putBool(("o_" + u).c_str(), offAllowed);
      }
      Serial.print("✅ Synced "); Serial.print(cards.size()); Serial.println(" cards & settings for offline security!");
    }
  } else {
    getHttp.end();
  }
}

// =================================================================
// 💳 PROCESS CARD SCAN (Online Server Check + Strict Offline Balance)
// =================================================================
void handleCardScanned(String uid) {
  Serial.print("\n💳 CARD SCANNED: "); Serial.println(uid);

  String uidClean = uid;
  uidClean.replace(" ", "");
  uidClean.toUpperCase();

  // Short tap beep
  beep(1, 80, 1600);

  showScreen("CARD SCAN HUA", "UID: " + uid, "Verifying Balance...", "Please wait...");

  checkWiFiConnection();

  // ── OFFLINE MODE: STRICT BALANCE & PERMISSION CHECK ──
  if (!wifiConnected) {
    Serial.println("⚡ Running in Offline Mode with Local Balance Verification...");

    // 1. Check if card exists in local flash memory
    String nKey = "n_" + uidClean;
    if (!prefs.isKey(nKey.c_str())) {
      showScreen("OFFLINE: ANJAAN CARD", "Online me pehle", "Ek bar tap karein", "Pani Mana Hai!");
      beep(3, 200, 700);
      delay(3000);
      return;
    }

    String custName = prefs.getString(nKey.c_str(), "Customer");
    bool isActive = prefs.getBool(("k_" + uidClean).c_str(), true);
    bool offAllowed = prefs.getBool(("o_" + uidClean).c_str(), true);
    float localBal = prefs.getFloat(("b_" + uidClean).c_str(), 0.0);

    // 2. Check if Blocked
    if (!isActive) {
      showScreen("CARD BLOCKED HAI!", custName, "Van Operator Se", "Sampark Karein");
      beep(3, 250, 700);
      delay(3500);
      return;
    }

    // 3. Check if Offline Access Allowed
    if (!offAllowed) {
      showScreen("OFFLINE MANAA HAI", custName, "Sirf Online Chalega", "Pani Mana Hai!");
      beep(3, 200, 700);
      delay(3000);
      return;
    }

    // 🔘 Multi-Quantity Selection if enabled (1L, 5L, 10L, 20L)
    float litresToGive = defaultCardLitres;
    if (multiQuantityEnable) {
      litresToGive = selectCardQuantity(custName, localBal);
      if (litresToGive <= 0.0) {
        return; // Cancelled
      }
    }
    float costRs = getQuantityCost(litresToGive);

    // 4. Check Balance
    if (localBal < costRs) {
      showScreen("BALANCE KHATAM!", custName, "Bal: Rs." + String(localBal, 1), "Chahiye: Rs." + String(costRs, 1));
      beep(3, 200, 800);
      delay(3500);
      return;
    }

    // 🔘 JAR SAFETY CHECK: Wait for start button before deducting balance!
    if (!waitForStartButton(custName, litresToGive)) {
      return; // Cancelled, no balance deducted
    }

    // 5. Approved -> Deduct locally & queue for auto-sync!
    float newBal = localBal - costRs;
    prefs.putFloat(("b_" + uidClean).c_str(), newBal);

    int offCount = prefs.getInt("off_count", 0);
    prefs.putString(("off_u_" + String(offCount)).c_str(), uidClean);
    prefs.putFloat(("off_l_" + String(offCount)).c_str(), litresToGive);
    prefs.putInt("off_count", offCount + 1);

    showScreen("OFFLINE APPROVED", custName, "Naya Bal: Rs." + String(newBal, 1), String(litresToGive, 0) + "L Paani Chalu");
    delay(1000);

    float actualDispensed = dispenseWater(custName, litresToGive);

    // 🛡️ OFFLINE DRY-RUN REFUND PROTECTION
    if (actualDispensed < 0.2) {
      prefs.putFloat(("b_" + uidClean).c_str(), localBal);
      if (offCount >= 0) prefs.putInt("off_count", offCount);
      showScreen("TANK KHALI HAI!", "Paisa Wapas Hua", "Bal: Rs." + String(localBal, 1), "0L Paani Nikla");
      beep(3, 200, 700);
      delay(3000);
    } else if (actualDispensed < (litresToGive * 0.9)) {
      float actualCost = actualDispensed * pricePerLitreRs;
      float correctedBal = localBal - actualCost;
      prefs.putFloat(("b_" + uidClean).c_str(), correctedBal);
      prefs.putFloat(("off_l_" + String(offCount)).c_str(), actualDispensed);
      showScreen("PARTIAL DISPENSE", "Nikla: " + String(actualDispensed, 1) + "L", "Bal: Rs." + String(correctedBal, 1), "Bacha Paisa Refund!");
      delay(3000);
    }
    return;
  }

  // ── ONLINE MODE: SERVER QUERY ──
  HTTPClient http;
  String uidUrl = uid;
  uidUrl.replace(" ", "%20");
  String checkUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/card/" + uidUrl;
  http.begin(checkUrl);
  http.setTimeout(4000);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["success"] == true) {
      String custName = doc["data"]["name"].as<String>();
      String custStatus = doc["data"]["status"] | "active";

      // Check if Card is Blocked
      if (custStatus == "blocked") {
        Serial.println("⛔ CARD IS BLOCKED BY ADMIN!");
        prefs.putBool(("k_" + uidClean).c_str(), false);
        showScreen("CARD BLOCKED HAI!", custName, "Van Operator Se", "Sampark Karein");
        beep(3, 250, 700);
        delay(3500);
        return;
      }

      long balPaise = doc["data"]["balance_paise"].as<long>();
      float balRs = (float)balPaise / 100.0;
      
      float litresToGive = doc["default_card_litres"] | defaultCardLitres;
      defaultCardLitres = litresToGive;

      // 🔘 Multi-Quantity Selection if enabled (1L, 5L, 10L, 20L)
      if (multiQuantityEnable) {
        float chosenLitres = selectCardQuantity(custName, balRs);
        if (chosenLitres <= 0.0) {
          return; // Cancelled
        }
        litresToGive = chosenLitres;
      }

      long ratePerLitrePaise = doc["paise_per_litre"] | 100;
      float costRs = getQuantityCost(litresToGive);

      if (balRs >= costRs) {
        // 🔘 JAR SAFETY CHECK: Wait for start button!
        if (!waitForStartButton(custName, litresToGive)) {
          return; // Cancelled, no money deducted
        }

        HTTPClient dspHttp;
        String dspUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/dispense";
        dspHttp.begin(dspUrl);
        dspHttp.addHeader("Content-Type", "application/json");

        String postData = "{\"card_uid\":\"" + uid + "\",\"litres\":" + String(litresToGive, 1) + "}";
        int dspCode = dspHttp.POST(postData);
        dspHttp.end();

        if (dspCode == 200) {
          float afterBal = balRs - costRs;
          bool offAllowed = doc["data"]["allow_offline"] | true;
          prefs.putString(("n_" + uidClean).c_str(), custName);
          prefs.putFloat(("b_" + uidClean).c_str(), afterBal);
          prefs.putBool(("k_" + uidClean).c_str(), true);
          prefs.putBool(("o_" + uidClean).c_str(), offAllowed);

          showScreen("PAYMENT SUCCESS", custName, "Bal: Rs." + String(afterBal, 1), String(litresToGive, 0) + "L Approved!");
          delay(1000);
          float actualDispensed = dispenseWater(custName, litresToGive);

          // 🛡️ ONLINE DRY-RUN REFUND PROTECTION
          if (actualDispensed < (litresToGive * 0.9)) {
            Serial.println("🚨 Dry-Run in Online Mode! Calling server auto-refund...");
            HTTPClient refHttp;
            String refUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/card-dispense-refund";
            refHttp.begin(refUrl);
            refHttp.addHeader("Content-Type", "application/json");
            String refData = "{\"card_uid\":\"" + uid + "\",\"deducted_litres\":" + String(litresToGive, 1) + ",\"actual_litres\":" + String(actualDispensed, 1) + "}";
            refHttp.POST(refData);
            refHttp.end();

            float restoredBal = balRs - (actualDispensed * (ratePerLitrePaise / 100.0));
            prefs.putFloat(("b_" + uidClean).c_str(), restoredBal);

            showScreen("TANK KHALI HAI!", "Nikla: " + String(actualDispensed, 1) + "L", "Card Par Refund Hua!", "Bal: Rs." + String(restoredBal, 1));
            beep(3, 200, 700);
            delay(3500);
          }
        } else {
          showScreen("ERROR", "Server dispense fail", "Try again", "");
          beep(3, 150, 600);
          delay(2500);
        }
      } else {
        // Insufficient balance
        showScreen("BALANCE KAM HAI!", custName, "Bal: Rs." + String(balRs, 1), "Chahiye: Rs." + String(costRs, 1));
        beep(3, 200, 800);
        delay(3500);
      }
    }
  } else {
    http.end();
    // Card not registered on server
    Serial.println("Card not found on server. Forwarding to bridge...");
    
    HTTPClient scanHttp;
    scanHttp.begin("http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/card-scan");
    scanHttp.addHeader("Content-Type", "application/json");
    scanHttp.POST("{\"uid\":\"" + uid + "\"}");
    scanHttp.end();

    showScreen("UNREGISTERED CARD", "UID: " + uid, "Web App pe Card", "Add / Link Karein");
    beep(2, 250, 700);
    delay(3000);
  }
}

// =================================================================
// 🚀 ARDUINO SETUP
// =================================================================
void setup() {
  Serial.begin(9600);
  delay(500);
  Serial.println("\n============================================");
  Serial.println("   ASHA RO WATER - SMART DISPENSER SYSTEM   ");
  Serial.println("============================================");

  // 0. Initialize Flash Preferences (Offline Balance & Card Storage)
  prefs.begin("asha_ro", false);

  pricePerLitreRs     = prefs.getFloat("price_per_l", 1.0);
  defaultCardLitres   = prefs.getFloat("def_litres", 2.0);
  flowStallSeconds    = prefs.getInt("stall_sec", 6);
  pulsesPerLitre      = prefs.getFloat("pulse_per_l", 450.0);
  dispenseMode        = prefs.getString("disp_mode", "button");
  waitButtonSeconds   = prefs.getInt("wait_btn_sec", 60);
  multiQuantityEnable = prefs.getBool("multi_qty", true);
  price1L  = prefs.getFloat("p_1l", 1.0);
  price5L  = prefs.getFloat("p_5l", 5.0);
  price10L = prefs.getFloat("p_10l", 10.0);
  price20L = prefs.getFloat("p_20l", 15.0);

  // 1. Setup Relay (Turn OFF immediately)
  pinMode(RELAY_PIN, OUTPUT);
  setMotor(false);

  // 2. Setup Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // 3. Setup Flow Sensor (GPIO 34 Interrupt)
  pinMode(FLOW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, RISING);

  // 4. Setup Push Buttons (Internal Pullups) & Start LED
  pinMode(BTN_START_PIN, INPUT_PULLUP);
  pinMode(BTN_CANCEL_PIN, INPUT_PULLUP);
  pinMode(LED_START_PIN, OUTPUT);
  digitalWrite(LED_START_PIN, LOW);

  pinMode(BTN_1L_PIN, INPUT_PULLUP);
  pinMode(BTN_5L_PIN, INPUT_PULLUP);
  pinMode(BTN_10L_PIN, INPUT_PULLUP);
  pinMode(BTN_20L_PIN, INPUT_PULLUP);

  // 5. Setup OLED Display
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("⚠️ OLED display init error!");
  } else {
    Serial.println("✅ OLED Connected (0x3C)");
  }

  // Welcome Screen
  showScreen("ASHA RO WATER", "Smart Dispenser", "Starting System...", "Please wait...");
  beep(1, 150, 1400);
  delay(1500);

  // 6. Setup RFID (RC522)
  SPI.begin(18, 19, 23, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(50);
  Serial.println("✅ RFID RC522 Initialized");

  // 7. Connect WiFi (Auto Home Mobile or Shop Wi-Fi)
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("AJAY KUSHWAHA", "77777777");            // Home Mobile Hotspot
  wifiMulti.addAP("Airtel_Asha Digital Center", "Ajay@6341#"); // Shop / Digital Center Wi-Fi

  showScreen("WIFI CONNECTING", "Searching Hotspot...", "Mobile / Shop Wi-Fi", "Please wait...");

  int wifiTries = 0;
  while (wifiMulti.run() != WL_CONNECTED && wifiTries < 20) {
    delay(400);
    Serial.print(".");
    wifiTries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    String connectedSSID = WiFi.SSID();
    Serial.println("\n✅ WiFi Connected: " + connectedSSID + " (IP: " + WiFi.localIP().toString() + ")");
    showScreen("WIFI CONNECTED", "WiFi: " + connectedSSID.substring(0, 14), "IP: " + WiFi.localIP().toString(), "System Ready!");
    beep(2, 80, 1600);
    // Initial sync of cards & offline queue
    syncCardsAndOfflineQueue();
  } else {
    wifiConnected = false;
    Serial.println("\n⚠️ WiFi Timeout - Standalone Mode.");
    showScreen("STANDALONE MODE", "Hotspot Not Found", "Direct Card Access", "System Ready!");
    beep(1, 300, 1000);
  }
  delay(2000);
}

// =================================================================
// 🔄 MAIN LOOP
// =================================================================
void loop() {
  if (isDispensing) return;

  // Check WiFi status periodically
  checkWiFiConnection();

  // 1. Check for Pending QR Orders from Web App (every 1.5 seconds)
  if (millis() - lastQRPollTime >= 1500) {
    lastQRPollTime = millis();
    checkPendingQROrders();
  }

  // Periodic card balance & offline queue sync (every 45 seconds if connected)
  if (millis() - lastCardSyncTime >= 45000) {
    lastCardSyncTime = millis();
    syncCardsAndOfflineQueue();
  }

  // 2. Ready Screen Refresh every 1.5 second
  static unsigned long lastReadyRefresh = 0;
  if (millis() - lastReadyRefresh >= 1500) {
    lastReadyRefresh = millis();
    String netStatus = wifiConnected ? ("WiFi: " + WiFi.SSID().substring(0, 12)) : "Offline (Direct Card)";
    showScreen("ASHA RO WATER", netStatus, "Scan QR / Tap Card", "Auto Water Dispenser");
  }

  // 3. RFID Card Detection
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  // Read Card UID
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i > 0) uid += " ";
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // Debounce card scan
  unsigned long now = millis();
  if (uid == lastScannedUID && (now - lastScanTime) < 3000) {
    return;
  }
  lastScannedUID = uid;
  lastScanTime = now;

  // Process Card
  handleCardScanned(uid);
}
