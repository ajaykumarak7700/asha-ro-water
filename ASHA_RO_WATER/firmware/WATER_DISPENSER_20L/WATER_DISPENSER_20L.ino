/**
 * ═════════════════════════════════════════════════════════════════
 *        🚰 ASHA RO WATER - SMART WATER DISPENSER SYSTEM
 * ═════════════════════════════════════════════════════════════════
 * 
 * Features:
 *   1. 📱 QR Code UPI Payment:
 *      Jitna rupaye pay kiya (e.g. Rs 10 ya Rs 20),
 *      website rate ke hisab se utna Litre paani turant dispense hota hai!
 * 
 *   2. 💳 RFID Card Tap Dispense:
 *      Card tap karne par Admin Dashboard se set Default Litres
 *      (e.g. 20L, 10L) dispense hota hai aur balance deduct hota hai!
 * 
 *   3. 🖥️ Live OLED Display & Buzzer:
 *      Real-time Litre progress bar, user name, balance, beep sounds.
 * 
 *   4. 🌊 ZJ-S201 Flow Sensor & 12V Relay:
 *      Pin D34 se pulse count, Pin D26 se Relay ON/OFF.
 * ═════════════════════════════════════════════════════════════════
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

// ═════════════════════════════════════════════════════════════════
// ⚙️ USER CONFIGURATION
// ═════════════════════════════════════════════════════════════════
const char* SERVER_IP     = "192.168.1.8";     // Web Server IP (Computer LAN IP)
const int   SERVER_PORT   = 3000;              // Server Port

float defaultCardLitres   = 2.0;               // Test ke liye 2L set hai (Admin setting se bhi badal sakte hain)
float pricePerLitreRs     = 1.0;               // Price per Litre (Server se auto sync)
const float PULSES_PER_L  = 450.0;             // ZJ-S201 Sensor calibration (~450 pulses/L)
const bool  RELAY_ACTIVE_LOW = false;          // False: Active HIGH relay (HIGH par relay ON, LOW par OFF)

// ═════════════════════════════════════════════════════════════════
// 📌 PIN DEFINITIONS
// ═════════════════════════════════════════════════════════════════
#define RFID_SS_PIN   5
#define RFID_RST_PIN  22

#define OLED_SDA_PIN  21
#define OLED_SCL_PIN  15
#define OLED_ADDR     0x3C

#define BUZZER_PIN    13
#define FLOW_PIN      34
#define RELAY_PIN     26

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

// ═════════════════════════════════════════════════════════════════
// 🔔 BUZZER & SOUND FUNCTIONS
// ═════════════════════════════════════════════════════════════════
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

// ═════════════════════════════════════════════════════════════════
// ⚡ RELAY CONTROLLER
// ═════════════════════════════════════════════════════════════════
void setMotor(bool on) {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
}

// ═════════════════════════════════════════════════════════════════
// 🖥️ OLED SCREEN HELPER
// ═════════════════════════════════════════════════════════════════
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

// ═════════════════════════════════════════════════════════════════
// 🌐 WIFI HELPER (Multi-Hotspot: Home Mobile + Shop Wi-Fi)
// ═════════════════════════════════════════════════════════════════
void checkWiFiConnection() {
  if (wifiMulti.run() == WL_CONNECTED) {
    wifiConnected = true;
  } else {
    wifiConnected = false;
  }
}

// ═════════════════════════════════════════════════════════════════
// 💧 DISPENSE WATER PROCESS (With Dry-Run / Tank Empty Auto-Protection)
// ═════════════════════════════════════════════════════════════════
float dispenseWater(String customerName, float litresToDispense) {
  isDispensing = true;
  flowPulseCount = 0;
  unsigned long targetPulses = (unsigned long)(litresToDispense * PULSES_PER_L);

  Serial.println("\n>>> STARTING WATER DISPENSING <<<");
  Serial.print("Customer: "); Serial.println(customerName);
  Serial.print("Litres Target: "); Serial.println(litresToDispense);
  Serial.print("Target Pulses: "); Serial.println(targetPulses);

  // Beep start confirmation
  beep(2, 100, 1500);

  // Turn Motor ON
  setMotor(true);
  unsigned long startTime = millis();
  unsigned long lastDisplayUpdate = 0;
  unsigned long lastPulseTime = millis();
  unsigned long lastCheckedPulseCount = 0;

  // 🛡️ DRY-RUN / NO-WATER FLOW PROTECTION
  const unsigned long STARTUP_GRACE_MS = 6000;     // 6 seconds for initial water to reach sensor
  const unsigned long MID_STALL_TIMEOUT_MS = 5000;  // 5 seconds if flow stops midway (tank dry)
  bool dryRunDetected = false;

  while (flowPulseCount < targetPulses) {
    unsigned long now = millis();

    // Track pulse activity
    if (flowPulseCount > lastCheckedPulseCount) {
      lastCheckedPulseCount = flowPulseCount;
      lastPulseTime = now;
    }

    // 1. Initial Dry-Run Check: Motor ON for >6s but no pulses detected (<5 pulses)!
    if (now - startTime > STARTUP_GRACE_MS && flowPulseCount < 5) {
      dryRunDetected = true;
      Serial.println("🚨 DRY-RUN DETECTED! No water flow in first 6s. Motor stopped immediately to protect pump!");
      break;
    }

    // 2. Midway Tank Empty Check: Water was flowing, but stopped for >5s!
    if (flowPulseCount >= 5 && (now - lastPulseTime > MID_STALL_TIMEOUT_MS)) {
      dryRunDetected = true;
      Serial.println("🚨 TANK EMPTY MIDWAY! Water flow stopped for >5s. Motor stopped to prevent dry running!");
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
      float currentLitres = (float)flowPulseCount / PULSES_PER_L;
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
  float actualLitres = (float)flowPulseCount / PULSES_PER_L;

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

// ═════════════════════════════════════════════════════════════════
// 📱 CHECK PENDING QR ORDERS FROM SERVER
// ═════════════════════════════════════════════════════════════════
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
      String orderId   = doc["order_id"].as<String>();
      float orderLitres = doc["litres"].as<float>();
      String buyerName = doc["customer_name"].as<String>();
      float amtRs      = doc["amount_rs"] | 0.0;

      Serial.println("\n🎉 QR PAYMENT RECEIVED!");
      Serial.print("Order: "); Serial.println(orderId);
      Serial.print("Amount: ₹"); Serial.println(amtRs);
      Serial.print("Litres: "); Serial.println(orderLitres);

      showScreen("QR PAYMENT SUCCESS", "User: " + buyerName, "Amount: Rs." + String(amtRs, 1), "Litres: " + String(orderLitres, 1) + "L");
      beep(2, 200, 1600);
      delay(2000);

      // Dispense water for QR order (with Dry-Run Auto-Detection)
      float actualDispensed = dispenseWater(buyerName, orderLitres);

      // Notify Server Order Completed or Failed (Tank Dry / Motor Error)
      HTTPClient doneHttp;
      String doneUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/complete";
      doneHttp.begin(doneUrl);
      doneHttp.addHeader("Content-Type", "application/json");

      String orderStatus = (actualDispensed >= (orderLitres * 0.8)) ? "completed" : "failed_no_water";
      String postBody = "{\"order_id\":\"" + orderId + "\",\"actual_litres\":" + String(actualDispensed, 1) + ",\"status\":\"" + orderStatus + "\"}";
      doneHttp.POST(postBody);
      doneHttp.end();
    }
  } else {
    http.end();
  }
}

// ═════════════════════════════════════════════════════════════════
// 📶 OFFLINE MEMORY & AUTO-SYNC ENGINE (Preferences NVS Flash)
// ═════════════════════════════════════════════════════════════════
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

    String jsonStr;
    serializeJson(syncDoc, jsonStr);

    HTTPClient syncHttp;
    String syncUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/offline-sync";
    syncHttp.begin(syncUrl);
    syncHttp.addHeader("Content-Type", "application/json");
    syncHttp.setTimeout(3000);
    int code = syncHttp.POST(jsonStr);
    syncHttp.end();

    if (code == 200) {
      Serial.println("✅ Offline queue synced successfully! Clearing local offline records.");
      for (int i = 0; i < offCount; i++) {
        prefs.remove(("off_u_" + String(i)).c_str());
        prefs.remove(("off_l_" + String(i)).c_str());
      }
      prefs.putInt("off_count", 0);
    }
  }

  // 2. Download latest cards, balances & settings from server
  HTTPClient getHttp;
  String getUrl = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/machine/sync-cards";
  getHttp.begin(getUrl);
  getHttp.setTimeout(3500);
  int gCode = getHttp.GET();
  if (gCode == 200) {
    String payload = getHttp.getString();
    getHttp.end();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["success"] == true) {
      if (doc["default_card_litres"]) defaultCardLitres = doc["default_card_litres"].as<float>();
      if (doc["price_per_litre_rs"]) pricePerLitreRs = doc["price_per_litre_rs"].as<float>();
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
      Serial.print("✅ Synced "); Serial.print(cards.size()); Serial.println(" cards for secure offline tracking!");
    }
  } else {
    getHttp.end();
  }
}

// ═════════════════════════════════════════════════════════════════
// 💳 PROCESS CARD SCAN (Online Server Check + Strict Offline Balance)
// ═════════════════════════════════════════════════════════════════
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
    float costRs = defaultCardLitres * pricePerLitreRs;

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

    // 4. Check Balance: 100 Rs se zyada use nahi kar payega!
    if (localBal < costRs) {
      showScreen("BALANCE KHATAM!", custName, "Bal: Rs." + String(localBal, 1), "Chahiye: Rs." + String(costRs, 1));
      beep(3, 200, 800);
      delay(3500);
      return;
    }

    // 5. Approved -> Deduct locally & queue for auto-sync!
    float newBal = localBal - costRs;
    prefs.putFloat(("b_" + uidClean).c_str(), newBal);

    int offCount = prefs.getInt("off_count", 0);
    prefs.putString(("off_u_" + String(offCount)).c_str(), uidClean);
    prefs.putFloat(("off_l_" + String(offCount)).c_str(), defaultCardLitres);
    prefs.putInt("off_count", offCount + 1);

    showScreen("OFFLINE APPROVED", custName, "Naya Bal: Rs." + String(newBal, 1), String(defaultCardLitres, 0) + "L Paani Chalu");
    delay(1200);

    float actualDispensed = dispenseWater(custName, defaultCardLitres);

    // 🛡️ OFFLINE DRY-RUN REFUND PROTECTION
    if (actualDispensed < 0.2) {
      // Tank was completely dry / pump failed: Restore 100% balance in flash memory!
      prefs.putFloat(("b_" + uidClean).c_str(), localBal);
      if (offCount >= 0) prefs.putInt("off_count", offCount); // cancel queued deduction
      showScreen("TANK KHALI HAI!", "Paisa Wapas Hua", "Bal: Rs." + String(localBal, 1), "0L Paani Nikla");
      beep(3, 200, 700);
      delay(3000);
    } else if (actualDispensed < (defaultCardLitres * 0.9)) {
      // Partial dispense: only charge for what was actually dispensed!
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

      // ── CHECK IF CARD IS BLOCKED BY ADMIN ──
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
      
      // Dynamic litres & rate set by Admin in Web Dashboard
      float litresToGive = doc["default_card_litres"] | defaultCardLitres;
      defaultCardLitres = litresToGive; // sync locally

      long ratePerLitrePaise = doc["paise_per_litre"] | 100;
      long costPaise = (long)(litresToGive * ratePerLitrePaise);
      float costRs = (float)costPaise / 100.0;

      Serial.print("Customer: "); Serial.println(custName);
      Serial.print("Balance: Rs. "); Serial.println(balRs);
      Serial.print("Cost for "); Serial.print(litresToGive); Serial.print("L: Rs. "); Serial.println(costRs);

      if (balPaise >= costPaise) {
        // Sufficient Balance -> Deduct on server
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
          // Sync to local flash memory so offline balance is always up to date!
          prefs.putString(("n_" + uidClean).c_str(), custName);
          prefs.putFloat(("b_" + uidClean).c_str(), afterBal);
          prefs.putBool(("k_" + uidClean).c_str(), true);
          prefs.putBool(("o_" + uidClean).c_str(), offAllowed);

          showScreen("PAYMENT SUCCESS", custName, "Bal: Rs." + String(afterBal, 1), String(litresToGive, 0) + "L Approved!");
          delay(1200);
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

// ═════════════════════════════════════════════════════════════════
// 🚀 ARDUINO SETUP
// ═════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(9600);
  delay(500);
  Serial.println("\n============================================");
  Serial.println("   ASHA RO WATER - SMART DISPENSER SYSTEM   ");
  Serial.println("============================================");

  // 0. Initialize Flash Preferences (Offline Balance & Card Storage)
  prefs.begin("asha_ro", false);
  pricePerLitreRs = prefs.getFloat("price_per_l", 1.0);
  defaultCardLitres = prefs.getFloat("def_litres", 2.0);

  // 1. Setup Relay (Turn OFF immediately)
  pinMode(RELAY_PIN, OUTPUT);
  setMotor(false);

  // 2. Setup Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // 3. Setup Flow Sensor (GPIO 34 Interrupt)
  pinMode(FLOW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, RISING);

  // 4. Setup OLED Display
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

  // 5. Setup RFID (RC522)
  SPI.begin(18, 19, 23, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(50);
  Serial.println("✅ RFID RC522 Initialized");

  // 6. Connect WiFi (Auto Home Mobile or Shop Wi-Fi)
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

// ═════════════════════════════════════════════════════════════════
// 🔄 MAIN LOOP
// ═════════════════════════════════════════════════════════════════
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
