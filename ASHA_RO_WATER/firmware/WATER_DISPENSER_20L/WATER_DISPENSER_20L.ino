/**
 * =================================================================
 *        🚰 ASHA RO WATER - ULTRA-FAST SMART DISPENSER
 * =================================================================
 * Features:
 *   1. ⚡ Lightning-Fast RFID: Max antenna gain (48dB), instant card sensing.
 *   2. 🛡️ Smart Offline Fallback: If WiFi/Server lags, uses local flash memory.
 *      Never says 'Unregistered' on network glitches!
 *   3. 🏷️ Quantity-Wise Tiered Pricing: 1L=₹1, 5L=₹5, 10L=₹10, 20L=₹15.
 *   4. 🔘 Smart Start & Cancel Push Buttons (Jar Waste Protection).
 *   5. 🌊 ZJ-S201 Flow Sensor (K-Factor Pulse Calibration) & Dry-Run Shield.
 *   6. 📶 Auto-Sync: Cards & offline deductions synced in background.
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
// ⚙️ SERVER & SYSTEM CONFIGURATION
// =================================================================
const char* SERVER_IP       = "192.168.1.8";     // Local Computer LAN IP
const int   SERVER_PORT     = 3000;              // Server Port

float defaultCardLitres     = 1.0;
float pricePerLitreRs       = 1.0;
int   flowStallSeconds      = 6;
float pulsesPerLitre        = 450.0;
const bool RELAY_ACTIVE_LOW = false;

// Quantity-Wise Tiered Prices
float price1L               = 1.0;
float price5L               = 5.0;
float price10L              = 10.0;
float price20L              = 15.0;

// Admin Settings
String dispenseMode         = "button"; // "button" or "auto"
int    waitButtonSeconds    = 60;
bool   multiQuantityEnable  = true;

// =================================================================
// 📌 PIN DEFINITIONS
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
#define LED_START_PIN  25   // Green LED Indicator

#define BTN_1L_PIN     17   // 1 Litre Button
#define BTN_5L_PIN     27   // 5 Litres Button
#define BTN_10L_PIN    32   // 10 Litres Button
#define BTN_20L_PIN    33   // 20 Litres Button

// Hardware Objects
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Flow Sensor Interrupt
volatile unsigned long flowPulseCount = 0;
void IRAM_ATTR flowPulseISR() {
  flowPulseCount++;
}

// Runtime State
bool isDispensing         = false;
String lastScannedUID     = "";
unsigned long lastScanTime = 0;
bool wifiConnected        = false;

unsigned long lastQRPollTime   = 0;
unsigned long lastCardSyncTime = 0;
unsigned long lastWiFiCheck    = 0;
unsigned long lastOledRefresh  = 0;

// Helper: Calculate price based on selected litres
float getQuantityCost(float litres) {
  if (litres == 1.0)  return price1L;
  if (litres == 5.0)  return price5L;
  if (litres == 10.0) return price10L;
  if (litres == 20.0) return price20L;
  return litres * pricePerLitreRs;
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
    // Only attempt reconnect if disconnected
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
  if (dispenseMode == "auto") {
    return true; // Auto mode: start immediately
  }

  Serial.println("[START_BTN] Waiting for start button...");
  unsigned long startWaitTime = millis();
  unsigned long lastBlinkTime = 0;
  bool ledState = false;
  unsigned long lastBeepTime = millis();

  while (true) {
    unsigned long now = millis();
    long remainSec = waitButtonSeconds - ((now - startWaitTime) / 1000);

    if (remainSec <= 0) {
      digitalWrite(LED_START_PIN, LOW);
      showScreen("TIME KHATAM!", "Jar Nahi Lagaya", "Order Cancelled", "Paisa Refund!");
      beep(3, 150, 700);
      delay(2000);
      return false;
    }

    // Blink Green LED
    if (now - lastBlinkTime >= 280) {
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
    if (now - lastPromptUpdate >= 300) {
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

    // Start Button
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

    delay(15);
  }
}

// =================================================================
// 🔘 QUANTITY SELECTION BUTTONS (1L, 5L, 10L, 20L)
// =================================================================
float selectCardQuantity(String custName, float balanceRs) {
  if (!multiQuantityEnable) {
    return defaultCardLitres;
  }

  Serial.println("[QTY] Select quantity: 1L, 5L, 10L, 20L");
  unsigned long startWait = millis();

  while (millis() - startWait < 8000) {
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

    // Cancel
    if (digitalRead(BTN_CANCEL_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_CANCEL_PIN) == LOW) {
        showScreen("RADD KIYA GAYA", custName, "Process Cancelled", "");
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
        delay(700);
        return 1.0;
      }
    }

    // 5 Litres
    if (digitalRead(BTN_5L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_5L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 5 LITER", custName, "Rupaye: Rs." + String(price5L, 1), "5L Selected");
        delay(700);
        return 5.0;
      }
    }

    // 10 Litres
    if (digitalRead(BTN_10L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_10L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 10 LITER", custName, "Rupaye: Rs." + String(price10L, 1), "10L Selected");
        delay(700);
        return 10.0;
      }
    }

    // 20 Litres (Offer Jar)
    if (digitalRead(BTN_20L_PIN) == LOW) {
      delay(40);
      if (digitalRead(BTN_20L_PIN) == LOW) {
        beep(1, 80, 1800);
        showScreen("CHUNA: 20 LITER", custName, "Offer: Rs." + String(price20L, 1), "20L Jar Selected!");
        delay(700);
        return 20.0;
      }
    }

    delay(15);
  }

  return defaultCardLitres;
}

// =================================================================
// 🌊 WATER DISPENSER CONTROLLER (Accurate Flow Calibration & Dry-Run Shield)
// =================================================================
float dispenseWater(String customerName, float litresToDispense) {
  isDispensing = true;
  flowPulseCount = 0;
  unsigned long targetPulses = (unsigned long)(litresToDispense * pulsesPerLitre);

  Serial.println(">>> STARTING WATER DISPENSING <<<");
  beep(2, 80, 1500);

  // Turn Motor ON
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
      display.print("User: ");
      display.println(customerName.substring(0, 14));

      display.setTextSize(2);
      display.setCursor(4, 28);
      display.print(currentLitres, 1);
      display.print("/");
      display.print(litresToDispense, 1);
      display.print("L");

      display.drawRect(0, 48, 128, 12, SSD1306_WHITE);
      int fillWidth = (progressPercent * 124) / 100;
      if (fillWidth > 0) {
        display.fillRect(2, 50, fillWidth, 8, SSD1306_WHITE);
      }
      display.display();
    }

    yield();
  }

  // Turn Motor OFF
  setMotor(false);
  isDispensing = false;
  float actualLitres = (float)flowPulseCount / pulsesPerLitre;

  if (dryRunDetected) {
    showScreen("! TANK KHALI HAI !", "Paani Nahi Nikla", "Nikla: " + String(actualLitres, 1) + "L", "Paisa Safe/Refund");
    beep(4, 120, 750);
    delay(3000);
    return actualLitres;
  }

  // Dispense complete success screen
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

  beep(1, 800, 1800);
  delay(2500);
  return actualLitres;
}

// =================================================================
// 📱 CHECK PENDING QR ORDERS (Non-blocking, 3.5s interval, 800ms timeout)
// =================================================================
void checkPendingQROrders() {
  if (!wifiConnected || isDispensing) return;

  HTTPClient http;
  String checkUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/pending";
  http.begin(checkUrl);
  http.setTimeout(800); // 800ms fast timeout so loop never freezes!

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

      Serial.println("[QR] Payment received!");
      showScreen("QR PAYMENT SUCCESS", "User: " + buyerName, "Amount: Rs." + String(amtRs, 1), "Litres: " + String(orderLitres, 1) + "L");
      beep(2, 150, 1600);
      delay(1200);

      // Jar Safety Check
      bool startPressed = waitForStartButton(buyerName, orderLitres);

      if (!startPressed) {
        HTTPClient cancelHttp;
        String cancelUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/complete";
        cancelHttp.begin(cancelUrl);
        cancelHttp.addHeader("Content-Type", "application/json");
        cancelHttp.setTimeout(1500);
        String postBody = "{"order_id":"" + orderId + "","actual_litres":0.0,"status":"cancelled_user"}";
        cancelHttp.POST(postBody);
        cancelHttp.end();

        showScreen("ORDER CANCELLED", "Jar Nahi Lagaya Gaya", "Paisa Turant Refund!", "");
        beep(2, 150, 700);
        delay(2500);
        return;
      }

      float actualDispensed = dispenseWater(buyerName, orderLitres);

      HTTPClient doneHttp;
      String doneUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/complete";
      doneHttp.begin(doneUrl);
      doneHttp.addHeader("Content-Type", "application/json");
      doneHttp.setTimeout(2000);
      String orderStatus = (actualDispensed >= (orderLitres * 0.8)) ? "completed" : "failed_no_water";
      String postBody = "{"order_id":"" + orderId + "","actual_litres":"" + String(actualDispensed, 1) + "","status":"" + orderStatus + ""}";
      doneHttp.POST(postBody);
      doneHttp.end();
    }
  } else {
    http.end();
  }
}

// =================================================================
// 📶 OFFLINE MEMORY & AUTO-SYNC ENGINE
// =================================================================
void syncCardsAndOfflineQueue() {
  if (!wifiConnected || isDispensing) return;

  // 1. Upload offline records if any
  int offCount = prefs.getInt("off_count", 0);
  if (offCount > 0) {
    JsonDocument syncDoc;
    JsonArray records = syncDoc["records"].to<JsonArray>();

    for (int i = 0; i < offCount; i++) {
      String cardU = prefs.getString(("off_u_" + String(i)).c_str(), "");
      float lit = prefs.getFloat(("off_l_" + String(i)).c_str(), defaultCardLitres);
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
    syncHttp.setTimeout(2500);
    int syncRes = syncHttp.POST(syncPayload);
    syncHttp.end();

    if (syncRes == 200) {
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
  getHttp.setTimeout(2500);

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
        prefs.putInt("stall_sec", flowStallSeconds);
      }
      if (doc["pulses_per_litre"]) {
        pulsesPerLitre = doc["pulses_per_litre"].as<float>();
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

      // Save each authorized card into local flash memory
      JsonArray cards = doc["cards"].as<JsonArray>();
      for (JsonObject c : cards) {
        String u = c["uid"].as<String>();
        u.replace(" ", "");
        u.toUpperCase();
        String name = c["name"].as<String>();
        float bal = c["balance_rs"].as<float>();
        bool active = (c["status"].as<String>() != "blocked");
        bool offAllowed = c["allow_offline"] | true;

        prefs.putString(("n_" + u).c_str(), name);
        prefs.putFloat(("b_" + u).c_str(), bal);
        prefs.putBool(("k_" + u).c_str(), active);
        prefs.putBool(("o_" + u).c_str(), offAllowed);
      }
      Serial.print("✅ Synced "); Serial.print(cards.size()); Serial.println(" cards from Admin!");
    }
  } else {
    getHttp.end();
  }
}

// =================================================================
// 💳 PROCESS CARD SCAN (Instant local flash check + Graceful online verify)
// =================================================================
void handleCardScanned(String uid) {
  Serial.print("[CARD] Detected: "); Serial.println(uid);

  String uidClean = uid;
  uidClean.replace(" ", "");
  uidClean.toUpperCase();

  // Instant touch beep (40ms micro-beep)
  beep(1, 40, 1800);

  // 1. FAST LOCAL FLASH LOOKUP (<1ms)
  String nKey = "n_" + uidClean;
  bool isKnownLocally = prefs.isKey(nKey.c_str());
  String custName     = isKnownLocally ? prefs.getString(nKey.c_str(), "Customer") : "Customer";
  float localBal      = isKnownLocally ? prefs.getFloat(("b_" + uidClean).c_str(), 0.0) : 0.0;
  bool isActive       = isKnownLocally ? prefs.getBool(("k_" + uidClean).c_str(), true) : true;
  bool offAllowed     = isKnownLocally ? prefs.getBool(("o_" + uidClean).c_str(), true) : true;

  if (isKnownLocally) {
    showScreen("CARD PEHCHANA", custName, "Bal: Rs." + String(localBal, 1), "Checking...");
  } else {
    showScreen("CARD SCAN HUA", "UID: " + uidClean, "Server pe jaanch...", "Please wait...");
  }

  // 2. Try Online Server Query if WiFi connected
  bool onlineSuccess = false;
  float serverBalRs = localBal;

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String checkUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/card/" + uidClean;
    http.begin(checkUrl);
    http.setTimeout(1800); // 1.8s max timeout

    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      http.end();

      JsonDocument doc;
      if (!deserializeJson(doc, payload) && doc["success"] == true) {
        onlineSuccess = true;
        custName = doc["data"]["name"].as<String>();
        String status = doc["data"]["status"] | "active";
        isActive = (status != "blocked");
        long balPaise = doc["data"]["balance_paise"] | 0;
        serverBalRs = (float)balPaise / 100.0;
        localBal = serverBalRs; // sync locally

        // Update local flash memory
        prefs.putString(nKey.c_str(), custName);
        prefs.putFloat(("b_" + uidClean).c_str(), localBal);
        prefs.putBool(("k_" + uidClean).c_str(), isActive);
      }
    } else if (httpCode == 404 && !isKnownLocally) {
      http.end();
      // True unregistered card (neither on server nor locally)
      showScreen("UNREGISTERED CARD", "UID: " + uidClean, "Admin me Card jodein", "");
      beep(3, 120, 700);
      delay(2000);
      return;
    } else {
      http.end();
      // Server unreachable or connection timeout -> Seamless offline fallback!
      Serial.println("⚠️ Server unreachable, falling back to local flash memory!");
    }
  }

  // 3. Fallback to Local Flash Memory Check
  if (!isKnownLocally && !onlineSuccess) {
    showScreen("OFFLINE: ANJAAN CARD", "Online WiFi me", "Ek bar tap karein", "");
    beep(2, 150, 700);
    delay(2000);
    return;
  }

  // Check if Blocked
  if (!isActive) {
    showScreen("CARD BLOCKED HAI!", custName, "Plant Operator Se", "Sampark Karein");
    beep(3, 180, 700);
    delay(2500);
    return;
  }

  // 4. Quantity Selection (1L, 5L, 10L, 20L)
  float effectiveBal = onlineSuccess ? serverBalRs : localBal;
  float litresToGive = defaultCardLitres;
  if (multiQuantityEnable) {
    litresToGive = selectCardQuantity(custName, effectiveBal);
    if (litresToGive <= 0.0) {
      return; // Cancelled by user
    }
  }

  float costRs = getQuantityCost(litresToGive);

  // 5. Check Balance
  if (effectiveBal < costRs) {
    showScreen("BALANCE KAM HAI!", custName, "Bal: Rs." + String(effectiveBal, 1), "Chahiye: Rs." + String(costRs, 1));
    beep(3, 150, 800);
    delay(2500);
    return;
  }

  // 6. Jar Safety Start Button Check
  if (!waitForStartButton(custName, litresToGive)) {
    return; // User cancelled or didn't place jar
  }

  // 7. Deduct Balance
  float afterBal = effectiveBal - costRs;

  if (onlineSuccess) {
    // Deduct on Server
    HTTPClient dspHttp;
    String dspUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/dispense";
    dspHttp.begin(dspUrl);
    dspHttp.addHeader("Content-Type", "application/json");
    dspHttp.setTimeout(2500);
    String postData = "{"card_uid":"" + uidClean + "","litres":" + String(litresToGive, 1) + "}";
    dspHttp.POST(postData);
    dspHttp.end();
  } else {
    // Queue for Offline Sync
    int offCount = prefs.getInt("off_count", 0);
    prefs.putString(("off_u_" + String(offCount)).c_str(), uidClean);
    prefs.putFloat(("off_l_" + String(offCount)).c_str(), litresToGive);
    prefs.putInt("off_count", offCount + 1);
  }

  // Always update flash memory balance
  prefs.putFloat(("b_" + uidClean).c_str(), afterBal);

  showScreen("APPROVED", custName, "Bal: Rs." + String(afterBal, 1), String(litresToGive, 0) + "L Paani Chalu");
  delay(800);

  // 8. Dispense Water
  float actualDispensed = dispenseWater(custName, litresToGive);

  // 9. Dry-run Protection: Restore balance if pump failed
  if (actualDispensed < 0.2) {
    prefs.putFloat(("b_" + uidClean).c_str(), effectiveBal);
    if (onlineSuccess) {
      HTTPClient refHttp;
      String refUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/card-dispense-refund";
      refHttp.begin(refUrl);
      refHttp.addHeader("Content-Type", "application/json");
      refHttp.POST("{"card_uid":"" + uidClean + "","deducted_litres":" + String(litresToGive, 1) + ","actual_litres":0.0}");
      refHttp.end();
    }
    showScreen("TANK KHALI HAI!", "Paisa Wapas Hua", "Bal: Rs." + String(effectiveBal, 1), "");
    beep(3, 150, 700);
    delay(2500);
  }
}

// =================================================================
// 🚀 ARDUINO SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("=== ASHA RO WATER SYSTEM STARTING ===");

  // 0. Preferences
  prefs.begin("asha_ro", false);
  pricePerLitreRs     = prefs.getFloat("price_per_l", 1.0);
  defaultCardLitres   = prefs.getFloat("def_litres", 1.0);
  flowStallSeconds    = prefs.getInt("stall_sec", 6);
  pulsesPerLitre      = prefs.getFloat("pulse_per_l", 450.0);
  dispenseMode        = prefs.getString("disp_mode", "button");
  waitButtonSeconds   = prefs.getInt("wait_btn_sec", 60);
  multiQuantityEnable = prefs.getBool("multi_qty", true);
  price1L  = prefs.getFloat("p_1l", 1.0);
  price5L  = prefs.getFloat("p_5l", 5.0);
  price10L = prefs.getFloat("p_10l", 10.0);
  price20L = prefs.getFloat("p_20l", 15.0);

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
  pinMode(BTN_5L_PIN, INPUT_PULLUP);
  pinMode(BTN_10L_PIN, INPUT_PULLUP);
  pinMode(BTN_20L_PIN, INPUT_PULLUP);

  // 4. OLED Display
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(400000); // 400kHz Fast I2C Mode!
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  showScreen("ASHA RO WATER", "Smart Dispenser", "Starting System...", "Please wait...");
  beep(1, 100, 1600);

  // 5. Setup RFID (RC522) with MAXIMUM ANTENNA GAIN
  SPI.begin(18, 19, 23, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(30);
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max); // ⚡ Boost antenna power to MAX (48dB) for instant long-range read!
  Serial.println("✅ RFID RC522 initialized with MAX Antenna Gain (48dB)!");

  // 6. Connect WiFi
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP("AJAY KUSHWAHA", "77777777");            // Home Mobile Hotspot
  wifiMulti.addAP("Airtel_Asha Digital Center", "Ajay@6341#"); // Shop / Digital Center Wi-Fi

  int wifiTries = 0;
  while (wifiMulti.run() != WL_CONNECTED && wifiTries < 15) {
    delay(250);
    wifiTries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    showScreen("WIFI CONNECTED", WiFi.SSID().substring(0, 14), WiFi.localIP().toString(), "System Ready!");
    beep(2, 60, 1800);
    // Initial sync
    syncCardsAndOfflineQueue();
  } else {
    wifiConnected = false;
    showScreen("STANDALONE MODE", "Direct Card Access", "System Ready!", "");
    beep(1, 200, 1000);
  }
  delay(1000);
}

// =================================================================
// 🔄 LIGHTNING-FAST MAIN LOOP
// =================================================================
void loop() {
  if (isDispensing) return;

  // ⚡ 1. RFID SENSING FIRST (Microsecond execution, zero delay)
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (i > 0) uid += " ";
      if (rfid.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    unsigned long now = millis();
    // Fast debounce (1.2s instead of 3s)
    if (uid != lastScannedUID || (now - lastScanTime) > 1200) {
      lastScannedUID = uid;
      lastScanTime = now;
      handleCardScanned(uid);
    }
    return;
  }

  unsigned long now = millis();

  // 2. WiFi Status Check every 8 seconds (non-blocking)
  checkWiFiConnection();

  // 3. QR Order Poll every 3.5 seconds
  if (now - lastQRPollTime >= 3500) {
    lastQRPollTime = now;
    checkPendingQROrders();
  }

  // 4. Background Sync every 30 seconds
  if (now - lastCardSyncTime >= 30000) {
    lastCardSyncTime = now;
    syncCardsAndOfflineQueue();
  }

  // 5. Ready Screen Refresh every 4 seconds
  if (now - lastOledRefresh >= 4000) {
    lastOledRefresh = now;
    String netStatus = (WiFi.status() == WL_CONNECTED) ? ("WiFi: " + WiFi.SSID().substring(0, 12)) : "Offline Ready";
    showScreen("ASHA RO WATER", netStatus, "Scan QR / Tap Card", "Auto Water Dispenser");
  }

  delay(5); // tiny rest to keep CPU cool and watchdog happy
}
