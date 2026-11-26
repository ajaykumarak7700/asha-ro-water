# 🚰 ASHA RO WATER — MASTER PROJECT CONVERSATION & MEMORY FILE
> **Notice to AI Agent / Developer:** This file preserves the complete project history, architectural decisions, hardware pinouts, cloud configuration, and operational context developed by Ajay Kushwaha and Antigravity AI. When resuming work on any new computer, read this file first!

---

## 1. Project Owner & Business Profile
- **Business Name:** ASHA RO WATER / ASHA ENTERPRISES
- **Owner / Operator:** Ajay Kushwaha
- **Primary Contact Email:** `ajaykumarak7700@gmail.com`
- **Primary Mobile / User ID:** `7700828989`
- **Project Purpose:** 20 Litre Smart Automatic Water ATM Van & Storefront Dispenser with RFID Card, Direct UPI QR Recharge, and Cloud Control.

---

## 2. Hardware Architecture & Pin Connections (ESP32)
- **Microcontroller:** ESP32 DevKit V1 (30-Pin)
- **RFID Reader (RC522 - SPI):**
  - SDA (SS): GPIO 5
  - SCK: GPIO 18 (shared / dedicated)
  - MOSI: GPIO 23
  - MISO: GPIO 19
  - RST: GPIO 22
  - 3.3V & GND
- **OLED Display (SSD1306 128x64 - I2C):**
  - SDA: GPIO 21
  - SCL: GPIO 22 (or hardware I2C default)
  - VCC: 3.3V / 5V, GND
- **Water Flow Sensor:** ZJ-S201 Hall-effect sensor on GPIO 18 (or GPIO 34 input, calibrated to ~450 pulses/Litre)
- **Pump / Solenoid Valve Control:** 12V 20A Relay Module on GPIO 26 (Active LOW / HIGH configured in firmware)
- **Audio Buzzer:** 5V Active Buzzer on GPIO 25
- **Firmware Location:** `firmware/WATER_DISPENSER_20L/WATER_DISPENSER_20L.ino`
- **Wi-Fi Multi Auto-Connect:**
  - `AJAY KUSHWAHA` (Pass: `77777777`)
  - `Airtel_Asha Digital Center` (Pass: `Ajay@6341#`)
- **Offline Resilience:** Built-in `Preferences.h` flash memory storing card balances offline so water dispenses even during internet outages, syncing automatically upon reconnection.

---

## 3. Cloud Infrastructure & Hosting
- **Live Web Service:** Render.com
  - Domain: `https://asha-ro-water.onrender.com`
  - GitHub Repo: `https://github.com/ajaykumarak7700/asha-ro-water` (Branch: `main`)
  - Runtime: Node.js (Express)
  - Auto-Deploy: Active on git push to `main`
- **Permanent Cloud Database:** Supabase (PostgreSQL)
  - Project: `ASHA RO WATER` (`thotwxnqsuarvfqjabyc.supabase.co`)
  - Region: AWS Mumbai / Sydney
  - Pricing Plan: Free Tier (500MB, Permanent, Never Expires)
  - Database Table: `app_state` (JSONB storing customers, wallets, transactions, cashback)
  - Connection Config: Injected via `DATABASE_URL` environment variable on Render.
- **Local Development:**
  - Local server runs on `http://localhost:3000` via `node server.js`
  - Local database file: `data/db.json` (auto-synced from Supabase if `DATABASE_URL` is configured).

---

## 4. Payment Gateway & UPI Recharge
- **Provider:** Razorpay Live Gateway (ASHA ENTERPRISES)
- **Key ID:** `rzp_live_U42Ce8kw6IIhK9`
- **Secret:** Encrypted on backend
- **Recharge Mechanism:**
  - Every card has a static Direct UPI QR code.
  - Generates sharp 1:1 square QR encoding `upi://pay?cu=INR&mc=5817&mode=19&pa=ashaenterprises673694.rzp@rxairtel&tn=Payment%20To%20ASHA%20ENTERPRISES&tr=${qrId}qrv2`.
  - Works natively with PhonePe, Google Pay, Paytm, BHIM UPI.
  - Razorpay Webhook notifies Render server `/api/razorpay/webhook`, which instantly credits the customer wallet.

---

## 5. Security & Authentication
- **Admin Portal URL:** `https://asha-ro-water.onrender.com/admin`
- **Admin User ID:** `7700828989`
- **Admin Password:** `Ajay@6341#`
- **Security Design:**
  - Server-side verification (`/api/admin/login`).
  - No credentials stored or visible in frontend HTML/JS (Developer Mode / Inspect Element safe).
  - Issues 64-character cryptographic Bearer token (`activeAdminTokens`).
  - Protected endpoints: `/api/recharge`, `/api/settings`, `/api/price`.

---

## 6. Key Features & Workflow History
1. **🎁 Cashback Wallet:**
   - Every 20L water purchase awards ₹1 cashback (100 paise) directly into the customer's cashback wallet.
   - Customers can tap `[ 🎁 Claim Cashback ]` on their passbook page (`/pay/:customer_id`) to transfer cashback into their active water wallet.
2. **🚨 Real-Time Unregistered Card Scan Detection:**
   - When an unassigned RFID card is tapped on the ESP32 machine, the machine alerts and calls `/api/card-scan` and `/api/card/:uid`.
   - The server catches it and alerts the Admin Panel in real time.
   - An alert banner pops up on the admin screen: `⚠️ Naya Unregistered Card Detect Hua! Card UID: XX XX XX XX`.
   - Clicking `[ ➕ Add This Card ]` opens the registration modal with the UID **already pre-filled**, allowing instant registration.
3. **🖨️ 2-Sided Card Stickers:**
   - Standard ISO ID-1 size: 85mm x 54mm.
   - Front: Brand logo, Customer Name, UID, and Passbook QR code.
   - Back: Direct UPI QR with PhonePe, GPay, Paytm badges, and "SCAN & RECHARGE" banner.
   - Static balance removed so cards remain permanently reusable without re-stickering.
   - Printable directly from Admin Panel or `C:\Users\Dell\Desktop\PRINT_CARD_STICKER_AJAY.html`.
4. **🛡️ Offline Dispense Protection & Expired Order Refund Control:**
   - When a customer buys water via QR but the ESP32 is offline/disconnected, orders expire automatically after a configurable safety timer (default 90s, adjustable to 30s, 60s, 90s, 120s directly in Admin Settings).
   - Prevents runaway dispenses (spilling 20L on an empty floor when reconnecting minutes/hours later).
   - Expired orders trigger a red alert card in the Admin Panel (`#expiredOrdersCard`) with sound chime.
   - Admin can view customer name, phone, amount (₹20), and payment ID.
   - Two 1-click action buttons:
     - `[ 💵 Cash Refund Diya ]`: Updates order to `refunded_cash` and records negative amount in ledger.
     - `[ 🚰 Abhi Paani Nikalo ]`: Resets order to pending so ESP32 immediately dispenses water when the customer is ready with their jar.
5. **🛡️ Flow Sensor Dry-Run Protection (Tank Empty / Motor Fault Auto-Detection):**
   - During water dispensing, ESP32 monitors flow pulses in real time.
   - If no water flows in the first few seconds (<5 pulses) or flow stalls midway:
     - Relay/Motor is turned OFF immediately (Dry-Run Protection, saves pump).
     - ESP32 OLED displays: `! TANK KHALI HAI ! Paani Nahi Nikla / Paisa Wapas Hoga`.
     - For RFID cards: In online mode, calls `/api/card-dispense-refund` to auto-credit wallet. In offline mode, flash memory balance is restored immediately.
     - For QR Payments: Sends `status: "failed_no_water"` to `/api/machine/complete`.
     - Admin Panel shows `🚨 टंकी खाली / मोटर एरर (0L Nikla)` with auto-refund / 1-click cash refund button.
6. **💳 Instant Razorpay Auto-Refund & Dynamic Safety Timers:**
   - When an unserved QR payment order expires or fails due to an empty tank, the backend automatically triggers `triggerRazorpayRefund(order.payment_id, ...)` to return the money directly to the customer's UPI account.
   - Admin can adjust safety timers in Admin Settings:
     - Machine Offline Expire Timer: 30s, 60s, 90s, 120s (default 90s).
     - Flow Sensor Dry-Run / Stall Timer: 2s to 60s (default 6s, auto-synced with ESP32).
     - Auto-Refund UPI Toggle (ON/OFF).
7. **☁️ Supabase PostgreSQL Cloud Persistence & 1-Click Backup / Restore:**
   - Automatic IPv4 Connection Pooler probing (`aws-0-ap-south-1.pooler.supabase.com:6543/5432`) prevents Render free-tier IPv6 `ENETUNREACH` disconnections.
   - Customer data is automatically merged and synchronized between PostgreSQL and local memory.
   - Admin Settings provides 1-click Full Database JSON Backup (`/api/admin/export-db`) and 1-click JSON Restore (`/api/admin/import-db`), guaranteeing customer cards are never lost during server redeployments.

---

## 7. How Any AI on a New Computer Reads This
When starting a session on a new computer, the AI assistant reads this `PROJECT_MEMORY.md` file and instantly possesses the exact full memory and context of the entire project history, technical choices, wiring, and credentials.
