/**
 * ASHA RO WATER - Main Server
 * Node.js + Express + JSON File Storage (no native compilation needed!)
 * Sab data local JSON files mein save hoga: data/db.json
 */

const express = require('express');
const cors    = require('cors');
const path    = require('path');
const fs      = require('fs');
const crypto  = require('crypto');
const QRCode  = require('qrcode');
const { v4: uuidv4 } = require('uuid');
const Razorpay = require('razorpay');

const app  = express();
const PORT = process.env.PORT || 3000;

// ─── RAZORPAY OFFICIAL LIVE CONFIGURATION ────────────────────
const DEFAULT_RZP_KEY    = Buffer.from('cnpwX2xpdmVfVTQyQ2U4a3c2SUloSzk=', 'base64').toString('ascii'); // rzp_live_U42Ce8kw6IIhK9
const DEFAULT_RZP_SECRET = Buffer.from('SDhhbjdxdTZPNVRlTzF6QlNJNm1xbjRJ', 'base64').toString('ascii'); // H8an7qu6O5TeO1zBSI6mqn4I

let RAZORPAY_KEY_ID     = process.env.RAZORPAY_KEY_ID || DEFAULT_RZP_KEY;
let RAZORPAY_KEY_SECRET = process.env.RAZORPAY_KEY_SECRET || DEFAULT_RZP_SECRET;

let razorpay = new Razorpay({
  key_id: RAZORPAY_KEY_ID,
  key_secret: RAZORPAY_KEY_SECRET
});

function initRazorpayFromDB() {
  const db = loadDB();
  if (db.settings && db.settings.razorpay_key_id && db.settings.razorpay_key_secret) {
    RAZORPAY_KEY_ID = db.settings.razorpay_key_id;
    RAZORPAY_KEY_SECRET = db.settings.razorpay_key_secret;
    razorpay = new Razorpay({
      key_id: RAZORPAY_KEY_ID,
      key_secret: RAZORPAY_KEY_SECRET
    });
    console.log(`💳 Razorpay re-initialized with Key: ${RAZORPAY_KEY_ID}`);
  }
}

// ─── IN-MEMORY: Last scanned & unregistered cards ──────
let lastScannedCard = { uid: null, scanned_at: null };
let unregisteredCards = []; // [{ uid, scanned_at, time_str }]

function formatUID(raw) {
  if (!raw) return '';
  const clean = raw.replace(/[^A-Za-z0-9]/g, '').toUpperCase();
  const chunks = clean.match(/.{1,2}/g);
  return chunks ? chunks.join(' ') : raw.toUpperCase().trim();
}

function recordUnregisteredCard(rawUid) {
  const uid = formatUID(rawUid);
  if (!uid || uid.length < 4) return null;
  const db = loadDB();
  const searchUid = uid.replace(/\s+/g, '').toUpperCase();
  const exists = db.customers.some(c => c.card_uid && c.card_uid.replace(/\s+/g, '').toUpperCase() === searchUid);
  if (exists) return null; // Already registered, ignore

  const now = Date.now();
  const time_str = nowIST();

  lastScannedCard = { uid, scanned_at: now };

  // Remove duplicate if already in list
  unregisteredCards = unregisteredCards.filter(c => c.uid.replace(/\s+/g, '') !== searchUid);
  const entry = { uid, scanned_at: now, time_str };
  unregisteredCards.unshift(entry);
  if (unregisteredCards.length > 15) unregisteredCards.pop();

  console.log(`🚨 UNREGISTERED CARD DETECTED: [${uid}] at ${time_str}`);
  return entry;
}

// ─── POSTGRESQL CLOUD DATABASE CONFIGURATION ───────────────
let Pool = null;
try {
  Pool = require('pg').Pool;
} catch (e) {
  console.warn('⚠️ pg module not found, fallback to disk storage');
}
let pgPool = null;
let activePgHost = 'none';
let activePgError = null;

async function tryConnectPg(connStr, description) {
  if (!Pool || !connStr) return null;
  try {
    const isLocal = connStr.includes('localhost') || connStr.includes('127.0.0.1');
    const p = new Pool({
      connectionString: connStr,
      ssl: isLocal ? false : { rejectUnauthorized: false },
      connectionTimeoutMillis: 4000,
      idleTimeoutMillis: 10000
    });
    // Test with a quick 3s race query
    const q = p.query('SELECT 1');
    const to = new Promise((_, rej) => setTimeout(() => rej(new Error('Connect timeout (3.5s)')), 3500));
    await Promise.race([q, to]);
    console.log(`🐘 PostgreSQL connected successfully (${description})!`);
    return p;
  } catch (err) {
    console.warn(`⚠️ PostgreSQL connection attempt failed (${description}):`, err.message);
    activePgError = `${description}: ${err.message}`;
    return null;
  }
}

async function establishDatabaseConnection() {
  if (!Pool) return null;
  const NEON_DEFAULT_URL = 'postgresql://neondb_owner:npg_L4Fhi8CdQnYo@ep-morning-glitter-bibjfdx1-pooler.c-5.ap-southeast-1.aws.neon.tech/neondb?sslmode=require';
  const rawUrl = process.env.DATABASE_URL || NEON_DEFAULT_URL;
  if (!rawUrl) return null;

  // 1. Direct connection test
  let pool = await tryConnectPg(rawUrl, 'direct DATABASE_URL');
  if (pool) {
    try {
      const u = new URL(rawUrl);
      activePgHost = u.hostname;
    } catch(e) { activePgHost = 'direct'; }
    activePgError = null;
    return pool;
  }

  // 2. Probing IPv4 Supabase Poolers (if direct is Supabase IPv6 host)
  try {
    const u = new URL(rawUrl);
    if (u.hostname.startsWith('db.') && u.hostname.endsWith('.supabase.co')) {
      const projectRef = u.hostname.split('.')[1];
      const baseUser = u.username.includes('.') ? u.username : `${u.username}.${projectRef}`;
      const dbName = u.pathname || '/postgres';
      const password = u.password;
      console.log(`ℹ️ Supabase IPv6 host detected (${u.hostname}). Probing IPv4 connection poolers for project '${projectRef}'...`);

      // Try AWS pooler regions (Mumbai ap-south-1 first, then Singapore, Sydney, US)
      const regions = ['ap-south-1', 'ap-southeast-1', 'ap-southeast-2', 'us-east-1'];
      for (const reg of regions) {
        // Try Transaction pooler (port 6543)
        const poolerUrl6543 = `postgresql://${encodeURIComponent(baseUser)}:${encodeURIComponent(password)}@aws-0-${reg}.pooler.supabase.com:6543${dbName}`;
        pool = await tryConnectPg(poolerUrl6543, `Supabase IPv4 Pooler ${reg}:6543`);
        if (pool) {
          activePgHost = `aws-0-${reg}.pooler.supabase.com:6543`;
          activePgError = null;
          return pool;
        }

        // Try Session pooler (port 5432)
        const poolerUrl5432 = `postgresql://${encodeURIComponent(baseUser)}:${encodeURIComponent(password)}@aws-0-${reg}.pooler.supabase.com:5432${dbName}`;
        pool = await tryConnectPg(poolerUrl5432, `Supabase IPv4 Pooler ${reg}:5432`);
        if (pool) {
          activePgHost = `aws-0-${reg}.pooler.supabase.com:5432`;
          activePgError = null;
          return pool;
        }
      }
    }
  } catch (e) {
    console.error('Error parsing DATABASE_URL for pooler candidate:', e.message);
  }

  return null;
}

// ─── ADMIN AUTHENTICATION CONFIGURATION ────────────────────
const ADMIN_USER = process.env.ADMIN_USER || '7700828989';
const ADMIN_PASS = process.env.ADMIN_PASS || 'Ajay@6341#';
const activeAdminTokens = new Set();

function generateAdminToken() {
  return crypto.randomBytes(32).toString('hex');
}

function verifyAdmin(req) {
  const authHeader = req.headers['authorization'] || '';
  const token = authHeader.replace(/^Bearer\s+/i, '').trim();
  if (token && (activeAdminTokens.has(token) || token === ADMIN_PASS)) return true;

  const bodyPass = req.body?.admin_pin || req.body?.admin_pass;
  if (bodyPass && (bodyPass === ADMIN_PASS || activeAdminTokens.has(bodyPass))) return true;

  return false;
}
const DATA_DIR = path.join(__dirname, 'data');
const DB_FILE  = path.join(DATA_DIR, 'db.json');
if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });

let memoryDB = null;

function getPriceTiers(db) {
  return db?.settings?.price_tiers || { "1": 1.0, "5": 5.0, "10": 10.0, "20": 15.0 };
}

function getLitresForAmount(amt, db) {
  const tiers = getPriceTiers(db);
  for (const [lStr, price] of Object.entries(tiers)) {
    if (Math.abs(amt - price) < 0.05) {
      return parseFloat(lStr);
    }
  }
  const rate_per_litre = (db.price?.paise_per_litre || 100) / 100.0;
  return Math.round((amt / rate_per_litre) * 10) / 10;
}

function getCostForLitres(litres, db) {
  const tiers = getPriceTiers(db);
  const lKey = (Math.round(litres * 10) / 10).toString();
  if (tiers[lKey] !== undefined) {
    return parseFloat(tiers[lKey]);
  }
  const intKey = Math.round(litres).toString();
  if (tiers[intKey] !== undefined) {
    return parseFloat(tiers[intKey]);
  }
  const rate_per_litre = (db.price?.paise_per_litre || 100) / 100.0;
  return litres * rate_per_litre;
}


function loadDiskDB() {
  if (!fs.existsSync(DB_FILE)) {
    const empty = {
      customers: [],
      wallets: {},
      transactions: [],
      price: { paise_per_litre: 100 },
      settings: { default_card_litres: 20 },
      pending_orders: [],
      cashback_wallets: {}
    };
    try { fs.writeFileSync(DB_FILE, JSON.stringify(empty, null, 2)); } catch(e){}
    return empty;
  }
  try {
    const db = JSON.parse(fs.readFileSync(DB_FILE, 'utf8'));
    if (!db.settings) db.settings = { default_card_litres: 20 };
    if (!db.pending_orders) db.pending_orders = [];
    if (!db.cashback_wallets) db.cashback_wallets = {};
    return db;
  } catch(e) {
    console.error('Error reading db.json:', e.message);
    return { customers: [], wallets: {}, transactions: [], price: { paise_per_litre: 100 }, settings: { default_card_litres: 20 }, pending_orders: [], cashback_wallets: {} };
  }
}

function loadDB() {
  if (memoryDB) return memoryDB;
  memoryDB = loadDiskDB();
  return memoryDB;
}

function saveDB(db) {
  memoryDB = db;
  // 1. Save to local disk cache
  try {
    fs.writeFileSync(DB_FILE, JSON.stringify(db, null, 2));
  } catch (e) {
    console.error('Disk save error:', e.message);
  }
  // 2. Save asynchronously to PostgreSQL Cloud Database
  if (pgPool) {
    const jsonStr = JSON.stringify(db);
    pgPool.query(
      `INSERT INTO app_state (key, data, updated_at) VALUES ($1, $2::jsonb, NOW())
       ON CONFLICT (key) DO UPDATE SET data = $2::jsonb, updated_at = NOW()`,
      ['asha_ro_water_db', jsonStr]
    ).then(() => {
      console.log(`💾 Successfully saved state to Supabase Cloud! (${db.customers ? db.customers.length : 0} customers)`);
    }).catch(err => {
      console.error('❌ PostgreSQL Cloud save error:', err.message);
    });
  }
}

async function initDatabase() {
  pgPool = await establishDatabaseConnection();
  const diskDB = loadDiskDB();

  if (pgPool) {
    try {
      await pgPool.query(`
        CREATE TABLE IF NOT EXISTS app_state (
          key VARCHAR(50) PRIMARY KEY,
          data JSONB NOT NULL,
          updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
      `);
      const res = await pgPool.query("SELECT data FROM app_state WHERE key = 'asha_ro_water_db'");
      if (res.rows.length > 0 && res.rows[0].data && res.rows[0].data.customers) {
        const cloudDB = res.rows[0].data;
        if (!cloudDB.settings) cloudDB.settings = { default_card_litres: 20 };
        if (!cloudDB.pending_orders) cloudDB.pending_orders = [];
        if (!cloudDB.cashback_wallets) cloudDB.cashback_wallets = {};

        // Merge disk customers into cloud if disk has any newly registered customer
        const cloudCustIds = new Set(cloudDB.customers.map(c => c.id));
        const cloudUids = new Set(cloudDB.customers.map(c => (c.card_uid || '').replace(/\s+/g, '').toUpperCase()));
        let merged = false;

        for (const dc of (diskDB.customers || [])) {
          const dUid = (dc.card_uid || '').replace(/\s+/g, '').toUpperCase();
          if (!cloudCustIds.has(dc.id) && dUid && !cloudUids.has(dUid)) {
            cloudDB.customers.push(dc);
            if (diskDB.wallets && diskDB.wallets[dc.id]) {
              cloudDB.wallets[dc.id] = diskDB.wallets[dc.id];
            }
            merged = true;
            console.log(`🔄 Merged local card into cloud database: ${dc.name} (${dc.card_uid})`);
          }
        }

        memoryDB = cloudDB;
        if (merged) {
          saveDB(memoryDB);
        }
        console.log(`✅ Loaded persistent database from PostgreSQL Cloud (${activePgHost})! (${memoryDB.customers.length} customers)`);
        try { fs.writeFileSync(DB_FILE, JSON.stringify(memoryDB, null, 2)); } catch(e){}
      } else {
        await pgPool.query(
          `INSERT INTO app_state (key, data, updated_at) VALUES ($1, $2::jsonb, NOW())
           ON CONFLICT (key) DO UPDATE SET data = $2::jsonb, updated_at = NOW()`,
          ['asha_ro_water_db', JSON.stringify(diskDB)]
        );
        memoryDB = diskDB;
        console.log(`🚀 Initialized PostgreSQL Cloud Database with ${diskDB.customers ? diskDB.customers.length : 0} initial customers!`);
      }
    } catch(err) {
      console.error('❌ PostgreSQL query error in initDatabase:', err.message);
      activePgError = err.message;
      memoryDB = diskDB;
    }
  } else {
    console.log('📁 Using local disk storage (data/db.json).');
    memoryDB = diskDB;
  }
  initRazorpayFromDB();
}

// ─── MIDDLEWARE ─────────────────────────────────────────────
app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.use(express.static(path.join(__dirname, 'public'), { index: false }));

// ── Admin Login Endpoint (Server-Side Secure) ──
app.post('/api/admin/login', (req, res) => {
  const { username, password } = req.body || {};
  if (username === ADMIN_USER && password === ADMIN_PASS) {
    const token = generateAdminToken();
    activeAdminTokens.add(token);
    console.log(`🔐 Admin login successful for user: ${username}`);
    return res.json({ success: true, token, message: '✅ लॉगिन सफल!' });
  }
  console.warn(`⚠️ Failed admin login attempt for user: ${username}`);
  return res.status(401).json({ success: false, message: '❌ ग़लत यूजर आईडी या पासवर्ड!' });
});

// ── Admin Check Session Endpoint ──
app.get('/api/admin/check-session', (req, res) => {
  if (verifyAdmin(req)) {
    return res.json({ success: true, user: ADMIN_USER });
  }
  return res.status(401).json({ success: false, message: 'सत्र समाप्त' });
});

// ── Admin Logout Endpoint ──
app.post('/api/admin/logout', (req, res) => {
  const authHeader = req.headers['authorization'] || '';
  const token = authHeader.replace(/^Bearer\s+/i, '').trim();
  if (token) activeAdminTokens.delete(token);
  res.json({ success: true, message: 'लॉगआउट सफल' });
});

// ─── GET LOCAL NETWORK IP ───────────────────────────────────
function getServerIP() {
  const { networkInterfaces } = require('os');
  const nets = networkInterfaces();
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) return net.address;
    }
  }
  return 'localhost';
}

function nowIST() {
  return new Date().toLocaleString('en-IN', { timeZone: 'Asia/Kolkata' });
}

// ══════════════════════════════════════════════════════════════
//  API ROUTES
// ══════════════════════════════════════════════════════════════

// ── 24/7 Keep-Alive Ping Endpoint (Never Sleep) ──
app.get('/api/ping', (req, res) => {
  res.json({
    status: 'alive',
    service: 'ASHA RO WATER',
    time: nowIST(),
    message: 'Server is 100% active and awake!'
  });
});

// ── Health & Database Cloud Status Endpoint ──
app.get('/api/cloud-status', async (req, res) => {
  let pgStatus = 'disconnected';
  let pgError = activePgError;
  let pgCount = 0;
  let pgCustomers = [];
  if (pgPool) {
    try {
      // 5-second query timeout
      const queryPromise = pgPool.query("SELECT data FROM app_state WHERE key = 'asha_ro_water_db'");
      const timeoutPromise = new Promise((_, reject) => setTimeout(() => reject(new Error('PostgreSQL Query Timeout (5s) - Could not connect to host')), 5000));
      const qRes = await Promise.race([queryPromise, timeoutPromise]);
      if (qRes && qRes.rows && qRes.rows.length > 0 && qRes.rows[0].data) {
        pgStatus = 'connected';
        pgCount = qRes.rows[0].data.customers ? qRes.rows[0].data.customers.length : 0;
        pgCustomers = qRes.rows[0].data.customers ? qRes.rows[0].data.customers.map(c => ({ name: c.name, uid: c.card_uid })) : [];
      } else {
        pgStatus = 'empty_table';
      }
    } catch(e) {
      pgStatus = 'query_error';
      pgError = e.message;
    }
  } else {
    pgStatus = activePgError ? `failed (${activePgError})` : 'no_pool (DATABASE_URL missing or Pool failed)';
  }
  const providerName = (activePgHost && activePgHost.includes('neon.tech')) ? 'Neon Cloud PostgreSQL' : 'PostgreSQL Cloud';
  res.json({
    success: true,
    provider: providerName,
    has_db_url: !!process.env.DATABASE_URL,
    active_host: activePgHost,
    pg_status: pgStatus,
    pg_error: pgError,
    cloud_customers_count: pgCount,
    cloud_customers: pgCustomers,
    supabase_customers_count: pgCount,
    supabase_customers: pgCustomers,
    memory_customers_count: memoryDB ? (memoryDB.customers ? memoryDB.customers.length : 0) : 0
  });
});

// ── Admin Sync Directly from Cloud Database into Local Cache ──
app.post('/api/admin/sync-from-cloud', async (req, res) => {
  if (!verifyAdmin(req)) {
    return res.status(403).json({ success: false, message: '❌ Unauthorized! एडमिन पासवर्ड से लॉगिन करें।' });
  }
  if (!pgPool) {
    return res.status(500).json({ success: false, message: '❌ Neon Cloud database connect nahi hai.' });
  }
  try {
    const qRes = await pgPool.query("SELECT data FROM app_state WHERE key = 'asha_ro_water_db'");
    if (qRes && qRes.rows && qRes.rows.length > 0 && qRes.rows[0].data) {
      memoryDB = qRes.rows[0].data;
      try { fs.writeFileSync(DB_FILE, JSON.stringify(memoryDB, null, 2)); } catch(e){}
      return res.json({ success: true, count: memoryDB.customers ? memoryDB.customers.length : 0, message: 'Neon Cloud se latest data sync ho gaya!' });
    } else {
      return res.json({ success: false, message: 'Cloud database me koi data nahi mila.' });
    }
  } catch(e) {
    return res.status(500).json({ success: false, message: 'Sync error: ' + e.message });
  }
});

// ── Admin Export Complete Database (JSON Backup) ──
app.get('/api/admin/export-db', (req, res) => {
  const db = loadDB();
  const dateStr = new Date().toISOString().slice(0, 10);
  res.setHeader('Content-disposition', `attachment; filename=asha_ro_water_backup_${dateStr}.json`);
  res.setHeader('Content-type', 'application/json');
  res.send(JSON.stringify(db, null, 2));
});

// ── Admin Import / Restore Database (JSON Backup) ──
app.post('/api/admin/import-db', (req, res) => {
  if (!verifyAdmin(req)) {
    return res.status(403).json({ success: false, message: '❌ Unauthorized! एडमिन पासवर्ड से लॉगिन करें।' });
  }
  const importData = req.body?.data || req.body;
  if (!importData || !Array.isArray(importData.customers)) {
    return res.status(400).json({ success: false, message: '❌ अमान्य बैकअप फ़ाइल! Customers सूची नहीं मिली।' });
  }
  if (!importData.wallets) importData.wallets = {};
  if (!importData.transactions) importData.transactions = [];
  if (!importData.price) importData.price = { paise_per_litre: 100 };
  if (!importData.settings) importData.settings = { default_card_litres: 20 };
  if (!importData.pending_orders) importData.pending_orders = [];
  if (!importData.cashback_wallets) importData.cashback_wallets = {};

  saveDB(importData);
  res.json({
    success: true,
    message: `✅ डेटाबेस सफलतापूर्वक रिस्टोर हो गया! (${importData.customers.length} ग्राहक)`,
    customers_count: importData.customers.length
  });
});

// ── GET all customers ──
app.get('/api/customers', (req, res) => {
  const db = loadDB();
  const data = db.customers.map(c => ({
    ...c,
    balance_paise: db.wallets[c.id] || 0
  }));
  res.json({ success: true, data });
});

// ── GET single customer by ID ──
app.get('/api/customers/:id', (req, res) => {
  const db = loadDB();
  const c  = db.customers.find(x => x.id === req.params.id);
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });
  res.json({ success: true, data: { ...c, balance_paise: db.wallets[c.id] || 0 } });
});

// ── GET customer by CARD UID (ESP32 use karega) ──
app.get('/api/card/:uid', (req, res) => {
  const db = loadDB();
  const rawParam = decodeURIComponent(req.params.uid || '');
  const searchUid = rawParam.replace(/\s+/g, '').toUpperCase();
  const c = db.customers.find(x => x.card_uid && x.card_uid.replace(/\s+/g, '').toUpperCase() === searchUid);
  if (!c) {
    const unreg = recordUnregisteredCard(rawParam);
    console.log(`⚠️ Card not found for UID: "${rawParam}" (clean: "${searchUid}")`);
    return res.status(404).json({ success: false, message: 'Card registered nahi hai', unreg_card: unreg });
  }
  res.json({
    success: true,
    data: { ...c, balance_paise: db.wallets[c.id] || 0 },
    paise_per_litre: db.price.paise_per_litre,
    price_tiers: getPriceTiers(db),
    default_card_litres: (db.settings && db.settings.default_card_litres) ? db.settings.default_card_litres : 2
  });
});

// ── POST create new customer ──
app.post('/api/customers', (req, res) => {
  const db = loadDB();
  const { name, phone, address, card_uid, opening_balance } = req.body;
  if (!name || !name.trim()) return res.status(400).json({ success: false, message: 'Naam zaroori hai' });

  const uid = card_uid ? card_uid.toUpperCase().trim() : null;
  if (uid && db.customers.find(x => x.card_uid === uid))
    return res.status(400).json({ success: false, message: 'Yeh card UID pehle se registered hai!' });

  const id      = uuidv4();
  const opening = Math.round((parseFloat(opening_balance) || 0) * 100);

  const newCustomer = {
    id, name: name.trim(),
    phone: phone || null,
    address: address || null,
    card_uid: uid,
    status: 'active',
    created_at: nowIST()
  };

  db.customers.unshift(newCustomer);
  db.wallets[id] = opening;

  if (opening > 0) {
    db.transactions.unshift({
      id: uuidv4(), customer_id: id, customer_name: name.trim(),
      card_uid: uid, type: 'RECHARGE', amount_paise: opening,
      note: 'Opening Balance', payment_ref: null, created_at: nowIST()
    });
  }
  if (uid) {
    const cleanAdd = uid.replace(/\s+/g, '').toUpperCase();
    unregisteredCards = unregisteredCards.filter(x => x.uid.replace(/\s+/g, '') !== cleanAdd);
  }
  saveDB(db);
  res.json({ success: true, id, message: 'Customer register ho gaya!' });
});

// ── PATCH update customer ──
app.patch('/api/customers/:id', (req, res) => {
  const db = loadDB();
  const idx = db.customers.findIndex(x => x.id === req.params.id);
  if (idx === -1) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  const { name, phone, address, card_uid, status } = req.body;
  const uid = card_uid ? card_uid.toUpperCase().trim() : db.customers[idx].card_uid;

  // Check UID uniqueness (excluding this customer)
  if (uid && db.customers.find((x, i) => x.card_uid === uid && i !== idx))
    return res.status(400).json({ success: false, message: 'Yeh card UID kisi aur ka hai!' });

  db.customers[idx] = {
    ...db.customers[idx],
    name: name || db.customers[idx].name,
    phone: phone !== undefined ? phone : db.customers[idx].phone,
    address: address !== undefined ? address : db.customers[idx].address,
    card_uid: uid,
    status: status || db.customers[idx].status
  };
  saveDB(db);
  res.json({ success: true, message: 'Customer update ho gaya!' });
});

// ── POST 1-Click Toggle Block / Unblock ──
app.post('/api/customers/:id/toggle-block', (req, res) => {
  const db = loadDB();
  const c = db.customers.find(x => x.id === req.params.id);
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  c.status = (c.status === 'blocked') ? 'active' : 'blocked';
  saveDB(db);
  const isBlocked = (c.status === 'blocked');
  res.json({
    success: true,
    status: c.status,
    message: isBlocked ? `⛔ Card Block kar diya gaya hai!` : `✅ Card Unblock (Active) ho gaya hai!`
  });
});

// ── GET single customer full transaction history ──
app.get('/api/customers/:id/history', (req, res) => {
  const db = loadDB();
  const c = db.customers.find(x => x.id === req.params.id);
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  const history = db.transactions.filter(t => t.customer_id === c.id || (c.card_uid && t.card_uid === c.card_uid));
  const totalRechargePaise = history.filter(t => t.type === 'RECHARGE').reduce((s, t) => s + (t.amount_paise || 0), 0);
  const totalDispensePaise = history.filter(t => t.type === 'DISPENSE').reduce((s, t) => s + (t.amount_paise || 0), 0);

  res.json({
    success: true,
    customer: { ...c, balance_paise: db.wallets[c.id] || 0 },
    stats: {
      balance_rs: ((db.wallets[c.id] || 0) / 100).toFixed(2),
      total_recharge_rs: (totalRechargePaise / 100).toFixed(2),
      total_dispense_rs: (totalDispensePaise / 100).toFixed(2),
      total_transactions: history.length
    },
    transactions: history
  });
});

// ── POST recharge wallet (Admin/Driver) ──
app.post('/api/recharge', (req, res) => {
  const { customer_id, amount_rs, note } = req.body;
  if (!verifyAdmin(req))
    return res.status(403).json({ success: false, message: '❌ Unauthorized! कृपया सही एडमिन पासवर्ड से लॉगिन करें।' });

  const amount_paise = Math.round((parseFloat(amount_rs) || 0) * 100);
  if (amount_paise <= 0)
    return res.status(400).json({ success: false, message: 'Valid amount dijiye' });

  const db = loadDB();
  const c  = db.customers.find(x => x.id === customer_id);
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  db.wallets[customer_id] = (db.wallets[customer_id] || 0) + amount_paise;
  const txnId = uuidv4();
  db.transactions.unshift({
    id: txnId, customer_id, customer_name: c.name, card_uid: c.card_uid,
    type: 'RECHARGE', amount_paise, note: note || 'Admin Recharge',
    payment_ref: null, created_at: nowIST()
  });
  saveDB(db);

  res.json({
    success: true,
    message: `✅ ₹${amount_rs} recharge successful!`,
    new_balance_rs: (db.wallets[customer_id] / 100).toFixed(2),
    txn_id: txnId
  });
});

// ── POST dispense water (ESP32 ya Admin trigger karta hai) ──
app.post('/api/dispense', (req, res) => {
  const { card_uid, customer_id, litres = 20 } = req.body;
  const db = loadDB();
  
  let customer = null;
  if (card_uid) {
    const sUid = card_uid.replace(/\s+/g, '').toUpperCase();
    customer = db.customers.find(x => x.card_uid && x.card_uid.replace(/\s+/g, '').toUpperCase() === sUid);
  } else if (customer_id) {
    customer = db.customers.find(x => x.id === customer_id);
  }

  if (!customer) {
    return res.status(404).json({ success: false, message: 'Customer/Card registered nahi hai!' });
  }

  if (customer.status === 'blocked') {
    return res.status(403).json({ success: false, message: 'Card blocked hai!' });
  }

  const cost_rs = getCostForLitres(litres, db);
  const cost_paise = Math.round(cost_rs * 100);
  const current_bal = db.wallets[customer.id] || 0;

  if (current_bal < cost_paise) {
    return res.status(400).json({
      success: false,
      message: '❌ Balance kam hai!',
      current_balance_rs: (current_bal / 100).toFixed(2),
      required_rs: (cost_paise / 100).toFixed(2)
    });
  }

  // Deduct
  db.wallets[customer.id] = current_bal - cost_paise;

  // 🎁 20L Cashback: Har 20L paani khareedne par ₹1 cashback wallet mein add hoga
  db.cashback_wallets = db.cashback_wallets || {};
  let cashback_earned_paise = 0;
  if (litres >= 20) {
    cashback_earned_paise = Math.floor(litres / 20) * 100; // 100 paise = ₹1.00
    db.cashback_wallets[customer.id] = (db.cashback_wallets[customer.id] || 0) + cashback_earned_paise;
    console.log(`🎁 Cashback Added: ₹${cashback_earned_paise/100} to ${customer.name}. Total Cashback: ₹${db.cashback_wallets[customer.id]/100}`);
  }

  const txnId = 'DSP_' + uuidv4().substring(0, 8).toUpperCase();
  db.transactions.unshift({
    id: txnId,
    customer_id: customer.id,
    customer_name: customer.name,
    card_uid: customer.card_uid,
    type: 'DISPENSE',
    amount_paise: cost_paise,
    note: `${litres} Liter Dispensed${cashback_earned_paise > 0 ? ` (+₹${cashback_earned_paise/100} Cashback Earned)` : ''}`,
    payment_ref: txnId,
    created_at: nowIST()
  });
  saveDB(db);

  console.log(`💧 Dispensed ${litres}L to ${customer.name}. New Balance: Rs.${(db.wallets[customer.id]/100).toFixed(2)}`);

  res.json({
    success: true,
    message: `✅ ${litres} Liter Dispense Approved!`,
    customer_name: customer.name,
    litres: litres,
    deducted_rs: (cost_paise / 100).toFixed(2),
    new_balance_rs: (db.wallets[customer.id] / 100).toFixed(2),
    cashback_earned_rs: (cashback_earned_paise / 100).toFixed(2),
    total_cashback_rs: (((db.cashback_wallets && db.cashback_wallets[customer.id]) || 0) / 100).toFixed(2)
  });
});

// ── POST Claim Cashback (Cashback wallet se Water wallet mein transfer) ──
app.post('/api/claim-cashback', (req, res) => {
  try {
    const { customer_id } = req.body;
    if (!customer_id) return res.status(400).json({ success: false, message: 'Customer ID zaroori hai' });

    const db = loadDB();
    const c = db.customers.find(x => x.id === customer_id || (x.card_uid && x.card_uid.replace(/\s+/g,'').toUpperCase() === customer_id.replace(/\s+/g,'').toUpperCase()));
    if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

    db.cashback_wallets = db.cashback_wallets || {};
    const cbPaise = db.cashback_wallets[c.id] || 0;

    if (cbPaise <= 0) {
      return res.status(400).json({ success: false, message: 'Filhal claim karne ke liye koi cashback nahi hai. 20L paani lene par ₹1 cashback milta hai!' });
    }

    // Transfer cashback to main water wallet
    db.cashback_wallets[c.id] = 0;
    db.wallets[c.id] = (db.wallets[c.id] || 0) + cbPaise;

    const txnId = 'CB_' + uuidv4().substring(0, 8).toUpperCase();
    db.transactions.unshift({
      id: txnId,
      customer_id: c.id,
      customer_name: c.name,
      card_uid: c.card_uid || 'CARD',
      type: 'CASHBACK',
      amount_paise: cbPaise,
      note: `🎁 ₹${(cbPaise / 100).toFixed(2)} Cashback Claimed (Added to Water Wallet)`,
      payment_ref: txnId,
      created_at: nowIST()
    });

    saveDB(db);
    console.log(`🎉 ${c.name} claimed ₹${(cbPaise/100).toFixed(2)} cashback! New Balance: ₹${(db.wallets[c.id]/100).toFixed(2)}`);

    res.json({
      success: true,
      message: `🎉 ₹${(cbPaise/100).toFixed(2)} Cashback aapke Paani Wallet mein jud gaya hai!`,
      claimed_rs: (cbPaise / 100).toFixed(2),
      new_balance_rs: (db.wallets[c.id] / 100).toFixed(2),
      cashback_rs: "0.00"
    });
  } catch (err) {
    console.error('Error claiming cashback:', err);
    res.status(500).json({ success: false, message: err.message });
  }
});

// ── POST simulate UPI payment (TEST MODE) ──
app.post('/api/pay/upi-simulate', (req, res) => {
  const { customer_id, amount_rs } = req.body;
  const amount_paise = Math.round((parseFloat(amount_rs) || 0) * 100);
  if (amount_paise <= 0)
    return res.status(400).json({ success: false, message: 'Valid amount dijiye' });

  const db = loadDB();
  const c  = db.customers.find(x => x.id === customer_id);
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  const txnId = 'SIM_' + uuidv4().substring(0, 8).toUpperCase();
  db.wallets[customer_id] = (db.wallets[customer_id] || 0) + amount_paise;
  db.transactions.unshift({
    id: txnId, customer_id, customer_name: c.name, card_uid: c.card_uid,
    type: 'RECHARGE', amount_paise, note: 'UPI Payment (TEST MODE)',
    payment_ref: txnId, created_at: nowIST()
  });
  saveDB(db);

  res.json({
    success: true,
    message: '✅ UPI Payment Simulated!',
    txn_id: txnId,
    new_balance_rs: (db.wallets[customer_id] / 100).toFixed(2)
  });
});

// ══════════════════════════════════════════════════════════════
// 💳 RAZORPAY OFFICIAL PAYMENT INTEGRATION
// ══════════════════════════════════════════════════════════════

// ── GET Razorpay Public Key ──
app.get('/api/razorpay/config', (req, res) => {
  res.json({ success: true, key_id: RAZORPAY_KEY_ID });
});

// ── Create Razorpay Order ──
app.post('/api/razorpay/create-order', async (req, res) => {
  try {
    const { amount_rs, customer_name, customer_id } = req.body;
    const amt = parseFloat(amount_rs);
    if (isNaN(amt) || amt <= 0) {
      return res.status(400).json({ success: false, message: 'Valid amount dalein' });
    }

    const amount_paise = Math.round(amt * 100);
    const options = {
      amount: amount_paise, // amount in paise
      currency: "INR",
      receipt: "rcpt_" + Date.now().toString().slice(-8),
      notes: {
        customer_name: customer_name || 'Customer',
        customer_id: customer_id || 'Instant'
      }
    };

    const order = await razorpay.orders.create(options);
    console.log(`💳 Razorpay Order Created: ${order.id} for ₹${amt}`);

    let payment_link = null;
    try {
      const pl = await razorpay.paymentLink.create({
        amount: amount_paise,
        currency: "INR",
        accept_partial: false,
        description: `RO Paani Dispense: Rs ${amt}`,
        callback_url: `https://asha-ro-water.onrender.com/pay/${customer_id || ''}?dispense=success&amt=${amt}`,
        callback_method: 'get',
        notes: {
          type: 'WATER_DISPENSE',
          customer_name: customer_name || 'Customer',
          customer_id: customer_id || 'Instant',
          order_id: order.id
        }
      });
      payment_link = pl.short_url;
    } catch(e) {
      console.warn('Hosted link warning in create-order:', e.message);
    }

    res.json({
      success: true,
      order_id: order.id,
      amount: order.amount,
      currency: order.currency,
      key_id: RAZORPAY_KEY_ID,
      payment_link: payment_link
    });
  } catch (error) {
    console.error('❌ Razorpay order error:', error);
    res.status(500).json({ success: false, message: error.message });
  }
});

// ── Create Razorpay Order for Customer Card Recharge (Direct Hosted Link - No Domain Block!) ──
app.post('/api/razorpay/create-recharge-order', async (req, res) => {
  try {
    const { amount_rs, customer_id } = req.body;
    const amt = parseFloat(amount_rs);
    if (isNaN(amt) || amt < 5) {
      return res.status(400).json({ success: false, message: 'Kam se kam ₹5 ka recharge karein' });
    }

    const db = loadDB();
    const c = db.customers.find(x => x.id === customer_id);
    if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

    const amount_paise = Math.round(amt * 100);

    const pl = await razorpay.paymentLink.create({
      amount: amount_paise,
      currency: "INR",
      accept_partial: false,
      description: `Card Recharge: Rs ${amt} (${c.name})`,
      customer: {
        name: c.name,
        contact: c.phone || '7700828989'
      },
      callback_url: `https://asha-ro-water.onrender.com/pay/${c.id}?recharge=success&amt=${amt}`,
      callback_method: 'get',
      notes: {
        type: 'CARD_RECHARGE',
        customer_id: c.id,
        customer_name: c.name,
        card_uid: c.card_uid || ''
      }
    });

    console.log(`💳 Razorpay Payment Link Created: ${pl.short_url} for ₹${amt} (${c.name})`);

    res.json({
      success: true,
      payment_link: pl.short_url,
      customer_name: c.name,
      card_uid: c.card_uid
    });
  } catch (error) {
    console.error('❌ Razorpay recharge link error:', error);
    res.status(500).json({ success: false, message: error.message });
  }
});

// ── Verify Razorpay Recharge & Credit Card Balance ──
app.post('/api/razorpay/verify-recharge', (req, res) => {
  try {
    const {
      razorpay_order_id,
      razorpay_payment_id,
      razorpay_signature,
      amount_rs,
      customer_id
    } = req.body;

    const amt = parseFloat(amount_rs);
    const body = razorpay_order_id + "|" + razorpay_payment_id;

    const expectedSignature = crypto
      .createHmac('sha256', RAZORPAY_KEY_SECRET)
      .update(body.toString())
      .digest('hex');

    if (expectedSignature !== razorpay_signature) {
      console.warn('❌ Invalid Razorpay Signature in recharge!');
      return res.status(400).json({ success: false, message: 'Recharge verification fail ho gaya' });
    }

    const db = loadDB();
    const c = db.customers.find(x => x.id === customer_id);
    if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

    const amount_paise = Math.round(amt * 100);
    db.wallets[customer_id] = (db.wallets[customer_id] || 0) + amount_paise;

    const txnId = 'REC_' + razorpay_payment_id.slice(-8);
    db.transactions.unshift({
      id: txnId,
      customer_id: c.id,
      customer_name: c.name,
      card_uid: c.card_uid || 'CARD',
      type: 'RECHARGE',
      amount_paise: amount_paise,
      note: `Online UPI Recharge via Razorpay (Pay ID: ${razorpay_payment_id})`,
      payment_ref: razorpay_payment_id,
      created_at: nowIST()
    });

    saveDB(db);
    console.log(`✅ Card Recharge Successful: ₹${amt} added for ${c.name}. New Balance: ₹${(db.wallets[customer_id]/100).toFixed(2)}`);

    res.json({
      success: true,
      message: `✅ ₹${amt} recharge successful!`,
      new_balance_rs: (db.wallets[customer_id] / 100).toFixed(2),
      txn_id: txnId
    });
  } catch (error) {
    console.error('❌ Verify recharge error:', error);
    res.status(500).json({ success: false, message: error.message });
  }
});

// ── Verify Razorpay Payment & Trigger Water Dispense ──
app.post('/api/razorpay/verify-payment', (req, res) => {
  try {
    const {
      razorpay_order_id,
      razorpay_payment_id,
      razorpay_signature,
      amount_rs,
      customer_name,
      customer_id
    } = req.body;

    const amt = parseFloat(amount_rs);
    const body = razorpay_order_id + "|" + razorpay_payment_id;

    const expectedSignature = crypto
      .createHmac('sha256', RAZORPAY_KEY_SECRET)
      .update(body.toString())
      .digest('hex');

    const isAuthentic = expectedSignature === razorpay_signature;

    if (!isAuthentic) {
      console.warn('❌ Invalid Razorpay Signature!');
      return res.status(400).json({ success: false, message: 'Payment verification failed (Invalid Signature)' });
    }

    console.log(`✅ Razorpay Payment VERIFIED! Payment ID: ${razorpay_payment_id}, Amount: ₹${amt}`);

    const db = loadDB();
    const rate_per_litre = (db.price.paise_per_litre || 100) / 100.0;
    const litres = Math.round((amt / rate_per_litre) * 10) / 10;

    const buyerName = customer_name ? customer_name.trim() : (customer_id ? (db.customers.find(c => c.id === customer_id)?.name || 'QR Customer') : 'QR Customer');

    // Create machine dispense order
    const orderId = 'RZP_' + razorpay_payment_id.slice(-8);
    const newOrder = {
      id: orderId,
      amount_rs: amt,
      litres: litres,
      customer_name: buyerName,
      customer_id: customer_id || null,
      payment_id: razorpay_payment_id,
      status: 'pending', // ESP32 will pick this up!
      created_at: nowIST(),
      created_at_ms: Date.now()
    };

    db.pending_orders.push(newOrder);

    // Record Transaction
    db.transactions.unshift({
      id: orderId,
      customer_id: customer_id || 'RAZORPAY_BUYER',
      customer_name: buyerName,
      card_uid: 'RAZORPAY_UPI',
      type: 'DISPENSE',
      amount_paise: Math.round(amt * 100),
      note: `Razorpay UPI: ₹${amt} -> ${litres}L Water (Pay ID: ${razorpay_payment_id})`,
      payment_ref: razorpay_payment_id,
      created_at: nowIST()
    });

    saveDB(db);

    console.log(`🚰 Machine Dispense Scheduled: ${litres} Litres for ${buyerName}`);

    res.json({
      success: true,
      message: `✅ ₹${amt} Payment Success! Machine ${litres} Liter paani dispense kar rahi hai.`,
      order_id: orderId,
      litres: litres,
      amount_rs: amt,
      payment_id: razorpay_payment_id
    });
  } catch (error) {
    console.error('❌ Verify payment error:', error);
    res.status(500).json({ success: false, message: error.message });
  }
});

// ── Razorpay Webhook (Direct Cloud Capture for Razorpay QR & Payments) ──
app.post('/api/razorpay/webhook', (req, res) => {
  try {
    const event = req.body.event;
    console.log(`🔔 Razorpay Webhook Event: ${event}`);

    if (event === 'payment.captured' || event === 'order.paid' || event === 'qr_code.credited') {
      const payment = req.body.payload?.payment?.entity || {};
      const amount_rs = (payment.amount || 0) / 100.0;
      const payment_id = payment.id || ('RZP_' + Date.now());
      const buyerPhone = payment.contact || payment.notes?.phone || '';
      let buyerName = payment.notes?.customer_name || payment.notes?.name || 'Razorpay User';
      if (buyerPhone && buyerName === 'Razorpay User') {
        buyerName = `User (${buyerPhone})`;
      }

      console.log(`🔔 Razorpay Webhook received: ₹${amount_rs} (Pay ID: ${payment_id}, Phone: ${buyerPhone})`);

      if (amount_rs > 0) {
        const db = loadDB();

        // 💳 CHECK IF THIS IS A CARD WALLET RECHARGE
        const qrEntity = req.body.payload?.qr_code?.entity || {};
        const plinkEntity = req.body.payload?.payment_link?.entity || {};
        const allNotes = { ...(qrEntity.notes || {}), ...(plinkEntity.notes || {}), ...(payment.notes || {}) };

        if (allNotes.type === 'CARD_RECHARGE' && (allNotes.customer_id || allNotes.card_uid)) {
          const custId = allNotes.customer_id;
          const cardUid = (allNotes.card_uid || '').replace(/\s+/g,'').toUpperCase();
          const c = db.customers.find(x => (custId && x.id === custId) || (cardUid && x.card_uid && x.card_uid.replace(/\s+/g,'').toUpperCase() === cardUid));
          if (c && !db.transactions.find(t => t.payment_ref === payment_id)) {
            const amount_paise = Math.round(amount_rs * 100);
            db.wallets[c.id] = (db.wallets[c.id] || 0) + amount_paise;
            db.transactions.unshift({
              id: 'REC_' + payment_id.slice(-8),
              customer_id: c.id,
              customer_name: c.name,
              card_uid: c.card_uid || 'CARD',
              type: 'RECHARGE',
              amount_paise: amount_paise,
              note: `Online UPI Direct Recharge (Pay ID: ${payment_id})`,
              payment_ref: payment_id,
              created_at: nowIST()
            });
            saveDB(db);
            console.log(`✅ Webhook: Added ₹${amount_rs} recharge to wallet of ${c.name} (${c.card_uid})`);
          }
          return res.json({ status: 'ok' });
        }

        if (!db.pending_orders.find(o => o.payment_id === payment_id)) {
          const rate_per_litre = (db.price.paise_per_litre || 100) / 100.0;
          const litres = Math.round((amount_rs / rate_per_litre) * 10) / 10;

          const orderId = 'RZ_' + payment_id.slice(-8);
          db.pending_orders.push({
            id: orderId,
            amount_rs: amount_rs,
            litres: litres,
            customer_name: buyerName,
            customer_phone: buyerPhone,
            payment_id: payment_id,
            status: 'pending',
            created_at: nowIST(),
            created_at_ms: Date.now()
          });

          const phoneNote = buyerPhone ? ` (Mob: ${buyerPhone})` : '';
          db.transactions.unshift({
            id: orderId,
            customer_id: 'RAZORPAY_BUYER',
            customer_name: buyerName,
            customer_phone: buyerPhone,
            card_uid: 'RAZORPAY_QR',
            type: 'DISPENSE',
            amount_paise: Math.round(amount_rs * 100),
            note: `Razorpay UPI: ₹${amount_rs} -> ${litres}L Water${phoneNote}`,
            payment_ref: payment_id,
            created_at: nowIST()
          });

          saveDB(db);
          console.log(`🚰 Razorpay Webhook: Enqueued ${litres}L dispense for machine!`);
        }
      }
    }
    res.json({ status: 'ok' });
  } catch (err) {
    console.error('Razorpay Webhook error:', err);
    res.status(500).json({ error: err.message });
  }
});

// ── Cashfree Webhook (Direct Cloud Capture for Cashfree QR & Forms) ──
app.post('/api/cashfree/webhook', (req, res) => {
  try {
    const body = req.body || {};
    console.log('🔔 Cashfree Webhook received:', JSON.stringify(body));

    let payment_id = null;
    let amount_rs = 0;
    let buyerName = 'Cashfree User';
    let buyerPhone = '';

    if (body.data) {
      payment_id = body.data.payment?.cf_payment_id || body.data.order?.order_id || ('CF_' + Date.now());
      amount_rs = parseFloat(body.data.payment?.payment_amount || body.data.order?.order_amount || 0);
      buyerName = body.data.customer_details?.customer_name || 'Cashfree User';
      buyerPhone = body.data.customer_details?.customer_phone || body.data.payment?.customer_phone || '';
    } else if (body.orderId || body.referenceId) {
      payment_id = body.referenceId || body.orderId;
      amount_rs = parseFloat(body.orderAmount || 0);
      buyerName = body.customerName || 'Cashfree User';
      buyerPhone = body.customerPhone || '';
    }

    if (buyerPhone && (buyerName === 'Cashfree User' || !buyerName)) {
      buyerName = `User (${buyerPhone})`;
    }

    if (amount_rs > 0 && payment_id) {
      payment_id = String(payment_id);
      const db = loadDB();
      if (!db.pending_orders.find(o => o.payment_id === payment_id)) {
        const rate_per_litre = (db.price.paise_per_litre || 100) / 100.0;
        const litres = Math.round((amount_rs / rate_per_litre) * 10) / 10;

        const orderId = 'CF_' + payment_id.slice(-8);
        db.pending_orders.push({
          id: orderId,
          amount_rs: amount_rs,
          litres: litres,
          customer_name: buyerName,
          customer_phone: buyerPhone,
          payment_id: payment_id,
          status: 'pending',
          created_at: nowIST(),
          created_at_ms: Date.now()
        });

        const phoneNote = buyerPhone ? ` (Mob: ${buyerPhone})` : '';
        db.transactions.unshift({
          id: 'DSP_' + uuidv4().slice(0, 8).toUpperCase(),
          customer_id: 'CASHFREE_BUYER',
          customer_name: buyerName,
          customer_phone: buyerPhone,
          card_uid: 'CASHFREE_QR',
          type: 'DISPENSE',
          amount_paise: Math.round(amount_rs * 100),
          note: `Cashfree UPI: ₹${amount_rs} -> ${litres}L Water${phoneNote}`,
          payment_ref: payment_id,
          created_at: nowIST()
        });

        saveDB(db);
        console.log(`🚰 Cashfree Webhook: Enqueued ${litres}L dispense for machine!`);
      }
    }

    res.json({ status: 'ok' });
  } catch (err) {
    console.error('Cashfree Webhook error:', err);
    res.status(500).json({ error: err.message });
  }
});

// ══════════════════════════════════════════════════════════════
// 💧 QR PAYMENT WATER DISPENSE & MACHINE QUEUE
// ══════════════════════════════════════════════════════════════

// ── Customer QR se direct paani khareedta hai (e.g. ₹10 -> 10L, ₹20 -> 20L) ──
app.post('/api/qr-buy-water', (req, res) => {
  const { amount_rs, customer_name, customer_id } = req.body;
  const amt = parseFloat(amount_rs);
  if (isNaN(amt) || amt <= 0) {
    return res.status(400).json({ success: false, message: 'Valid amount dalein (e.g. 10 ya 20)' });
  }

  const db = loadDB();
  const rate_per_litre = (db.price.paise_per_litre || 100) / 100.0;
  // Calculate litres based on rate set in website
  const litres = Math.round((amt / rate_per_litre) * 10) / 10; // 1 decimal place

  const orderId = 'ORD_' + uuidv4().substring(0, 8).toUpperCase();
  const buyerName = customer_name ? customer_name.trim() : (customer_id ? (db.customers.find(c => c.id === customer_id)?.name || 'QR Customer') : 'QR Customer');

  const newOrder = {
    id: orderId,
    amount_rs: amt,
    litres: litres,
    customer_name: buyerName,
    customer_id: customer_id || null,
    status: 'pending', // pending -> in_progress -> completed -> expired
    created_at: nowIST(),
    created_at_ms: Date.now()
  };

  db.pending_orders.push(newOrder);

  // Record transaction
  db.transactions.unshift({
    id: orderId,
    customer_id: customer_id || 'QR_BUYER',
    customer_name: buyerName,
    card_uid: 'UPI_QR',
    type: 'DISPENSE',
    amount_paise: Math.round(amt * 100),
    note: `QR Pay: ₹${amt} -> ${litres}L Water`,
    payment_ref: orderId,
    created_at: nowIST()
  });

  saveDB(db);

  console.log(`🚰 New QR Order: ₹${amt} -> ${litres} Litres for ${buyerName} (Order: ${orderId})`);

  res.json({
    success: true,
    message: `✅ ₹${amt} payment successful! ${litres} Liter pani machine se bharega.`,
    order_id: orderId,
    litres: litres,
    amount_rs: amt
  });
});

// ── RAZORPAY AUTO-REFUND HELPER ──
async function triggerRazorpayRefund(paymentId, amountPaise, reason) {
  if (!razorpay || !paymentId) return null;
  if (!paymentId.startsWith('pay_')) {
    console.log(`ℹ️ Payment ID ${paymentId} is not a live Razorpay pay_id, skipping gateway API refund.`);
    return null;
  }
  try {
    const refund = await razorpay.payments.refund(paymentId, {
      amount: amountPaise,
      notes: { reason: reason || 'Water ATM Auto-Refund' }
    });
    console.log(`💳 Razorpay Auto-Refund SUCCESS! Refund ID: ${refund.id} for ₹${amountPaise/100}`);
    return refund;
  } catch (err) {
    console.error(`❌ Razorpay Auto-Refund Error for ${paymentId}:`, err.message);
    return null;
  }
}

// ── HELPER: Check and mark pending orders as expired if machine was offline > order_expiry_seconds ──
function checkExpiredOrders(db) {
  if (!db || !db.pending_orders) return false;
  const nowMs = Date.now();
  const expSec = parseInt((db.settings && db.settings.order_expiry_seconds) ? db.settings.order_expiry_seconds : 90, 10);
  const EXPIRE_TIMEOUT_MS = Math.max(15, isNaN(expSec) ? 90 : expSec) * 1000;
  const autoRefundEnabled = db.settings?.auto_refund_upi !== false;
  let changed = false;

  for (const o of db.pending_orders) {
    if (o.status === 'pending') {
      const orderMs = o.created_at_ms || (o.created_at ? new Date(o.created_at).getTime() : 0);
      if (orderMs && (nowMs - orderMs > EXPIRE_TIMEOUT_MS)) {
        o.status = 'expired';
        o.expired_at = nowIST();
        o.expire_reason = `ESP32 Machine Offline / Timeout (> ${Math.round(EXPIRE_TIMEOUT_MS/1000)}s)`;
        changed = true;
        console.log(`⚠️ Order ${o.id} EXPIRED! Machine was offline >${Math.round(EXPIRE_TIMEOUT_MS/1000)}s.`);

        // 💳 AUTO-REFUND VIA RAZORPAY IF ONLINE QR PAYMENT
        if (autoRefundEnabled && o.payment_id && o.payment_id.startsWith('pay_')) {
          const amtPaise = Math.round((o.amount_rs || 20) * 100);
          triggerRazorpayRefund(o.payment_id, amtPaise, 'Water Dispenser Offline Timeout')
            .then(ref => {
              if (ref) {
                o.status = 'auto_refunded';
                o.refund_id = ref.id;
                o.refunded_at = nowIST();
                o.refund_method = 'RAZORPAY_UPI';
                saveDB(db);
                console.log(`✅ Order ${o.id} AUTO-REFUNDED via Razorpay (Refund ID: ${ref.id})`);
              }
            }).catch(e => console.error('Refund background error:', e.message));
        }

        // Update corresponding transaction so it clearly marks expired
        if (db.transactions) {
          const txn = db.transactions.find(t => t.id === o.id || (o.payment_id && t.payment_ref === o.payment_id));
          if (txn && !txn.note.includes('[EXPIRED]') && !txn.note.includes('[AUTO-REFUNDED]')) {
            txn.note = `[EXPIRED - Water Not Dispensed] ` + txn.note;
            txn.status = 'EXPIRED';
          }
        }
      }
    }
  }
  return changed;
}

// ── ESP32 polls this endpoint to check if there is an active QR order ──
app.get('/api/machine/pending', (req, res) => {
  const db = loadDB();
  const changed = checkExpiredOrders(db);

  // Find first active unexpired pending order
  const order = db.pending_orders.find(o => o.status === 'pending');
  if (order) {
    order.status = 'in_progress';
    order.started_at = nowIST();
    saveDB(db);
    console.log(`🤖 Machine starting dispense for Order ${order.id}: ${order.litres}L`);
    return res.json({
      has_order: true,
      order_id: order.id,
      litres: order.litres,
      customer_name: order.customer_name,
      amount_rs: order.amount_rs
    });
  }

  if (changed) {
    saveDB(db);
  }
  res.json({ has_order: false });
});

// ── GET list of expired / offline / dry-run failed orders for Admin Dashboard ──
app.get('/api/admin/expired-orders', (req, res) => {
  const db = loadDB();
  const changed = checkExpiredOrders(db);
  if (changed) saveDB(db);

  const expiredList = (db.pending_orders || [])
    .filter(o => o.status === 'expired' || o.status === 'failed_no_water')
    .sort((a, b) => (b.created_at_ms || 0) - (a.created_at_ms || 0));

  res.json({
    success: true,
    count: expiredList.length,
    orders: expiredList
  });
});

// ── Admin resolves an expired order (Cash refund or immediate manual dispense retry) ──
app.post('/api/admin/resolve-order', (req, res) => {
  const { order_id, action, note } = req.body;
  if (!order_id || !action) {
    return res.status(400).json({ success: false, message: 'order_id aur action jaroori hain' });
  }

  const db = loadDB();
  checkExpiredOrders(db);

  const order = (db.pending_orders || []).find(o => o.id === order_id);
  if (!order) {
    return res.status(404).json({ success: false, message: 'Order nahi mila' });
  }

  if (action === 'cash_refund') {
    order.status = 'refunded_cash';
    order.resolved_at = nowIST();
    order.resolution = 'CASH_REFUND';
    order.refund_note = note || `₹${order.amount_rs} Cash refunded to customer by Admin`;

    // Record refund in transactions
    const refTxnId = 'REF_' + uuidv4().substring(0, 8).toUpperCase();
    db.transactions.unshift({
      id: refTxnId,
      customer_id: order.customer_id || 'QR_BUYER',
      customer_name: order.customer_name || 'Customer',
      card_uid: 'CASH_REFUND',
      type: 'REFUND',
      amount_paise: -Math.round((order.amount_rs || 20) * 100),
      note: `💵 Cash Refund: ₹${order.amount_rs} wapas kiya (Order ${order.id})`,
      payment_ref: order.id,
      created_at: nowIST()
    });

    saveDB(db);
    console.log(`💵 Order ${order.id} marked as CASH REFUNDED (₹${order.amount_rs})`);
    return res.json({
      success: true,
      message: `✅ ₹${order.amount_rs} Cash Refund record ho gaya!`,
      order
    });
  } else if (action === 'retry_dispense') {
    // Reset order to pending with fresh timestamp so machine picks it up immediately
    order.status = 'pending';
    order.created_at = nowIST();
    order.created_at_ms = Date.now();
    order.retried_by_admin = true;
    order.retried_at = nowIST();

    saveDB(db);
    console.log(`🚰 Order ${order.id} RETRIED by Admin for dispense (${order.litres}L)`);
    return res.json({
      success: true,
      message: `🚰 Machine ko ${order.litres}L paani nikalne ka signal bhej diya gaya hai!`,
      order
    });
  } else if (action === 'dismiss') {
    order.status = 'dismissed';
    order.resolved_at = nowIST();
    saveDB(db);
    return res.json({ success: true, message: 'Order dismiss ho gaya.', order });
  }

  res.status(400).json({ success: false, message: 'Unknown action' });
});

// ── Public Order Status Check (For Customer Mobile UI) ──
app.get('/api/order-status/:id', (req, res) => {
  const db = loadDB();
  checkExpiredOrders(db);
  const order = (db.pending_orders || []).find(o => o.id === req.params.id);
  if (!order) {
    return res.status(404).json({ success: false, message: 'Order nahi mila' });
  }
  res.json({
    success: true,
    order: {
      id: order.id,
      status: order.status,
      litres: order.litres,
      amount_rs: order.amount_rs,
      customer_name: order.customer_name,
      created_at: order.created_at,
      expired_at: order.expired_at,
      status_text: order.status === 'expired'
        ? '⚠️ मशीन ऑफ़लाइन थी, इसलिए पानी नहीं निकला। कृपया ऑपरेटर (अजय जी) को Payment ID दिखाकर ₹20 कैश वापस लें या पानी निकलवाएं।'
        : (order.status === 'completed'
            ? '✅ पानी निकल चुका है!'
            : (order.status === 'refunded_cash'
                ? '💵 इस आर्डर का ₹20 कैश रिफंड दिया जा चुका है।'
                : '⏳ मशीन प्रोसेस कर रही है...'))
    }
  });
});

// ── ESP32 confirms order completed or reports failure (No water / Dry-run) ──
app.post('/api/machine/complete', (req, res) => {
  const { order_id, actual_litres, status, reason } = req.body;
  const db = loadDB();
  const order = (db.pending_orders || []).find(o => o.id === order_id);
  if (order) {
    const actL = (actual_litres !== undefined) ? parseFloat(actual_litres) : order.litres;
    order.actual_dispensed_litres = isNaN(actL) ? 0 : actL;

    if (status === 'cancelled_user' || status === 'timeout_no_jar') {
      order.status = 'cancelled_user';
      order.failed_at = nowIST();
      order.failure_reason = reason || (status === 'cancelled_user' ? 'Customer Cancel Button Pressed' : 'Jar not placed / Start button timeout');
      console.log(`🛑 Order ${order_id} CANCELLED by user or button timeout! Triggering auto-refund.`);

      const autoRefundEnabled = db.settings?.auto_refund_upi !== false;
      if (autoRefundEnabled && order.payment_id && order.payment_id.startsWith('pay_')) {
        const amtPaise = Math.round((order.amount_rs || 20) * 100);
        triggerRazorpayRefund(order.payment_id, amtPaise, 'User Cancelled / Button Timeout')
          .then(ref => {
            if (ref) {
              order.status = 'auto_refunded';
              order.refund_id = ref.id;
              order.refunded_at = nowIST();
              order.refund_method = 'RAZORPAY_UPI';
              saveDB(db);
              console.log(`✅ Cancelled Order ${order_id} AUTO-REFUNDED via Razorpay (Refund ID: ${ref.id})`);
            }
          }).catch(e => console.error('Cancellation refund error:', e.message));
      }

      const txn = db.transactions.find(t => t.id === order_id || (order.payment_id && t.payment_ref === order.payment_id));
      if (txn && !txn.note.includes('[CANCELLED]') && !txn.note.includes('[AUTO-REFUNDED]')) {
        txn.note = `[CANCELLED - Water Not Dispensed] ` + txn.note;
        txn.status = 'CANCELLED';
      }
    } else if (status === 'failed_no_water' || (actL < (order.litres * 0.5) && order.litres >= 1)) {
      order.status = 'failed_no_water';
      order.failed_at = nowIST();
      order.failure_reason = reason || 'Tank khali / Motor error (Dry-run stopped)';
      console.log(`🚨 Machine reported NO WATER for Order ${order_id}! Motor stopped to protect pump. (Actual: ${order.actual_dispensed_litres}L)`);

      // 💳 AUTO-REFUND VIA RAZORPAY IF ONLINE QR
      const autoRefundEnabled = db.settings?.auto_refund_upi !== false;
      if (autoRefundEnabled && order.payment_id && order.payment_id.startsWith('pay_')) {
        const amtPaise = Math.round((order.amount_rs || 20) * 100);
        triggerRazorpayRefund(order.payment_id, amtPaise, 'Tank Empty / Dry-run No Flow')
          .then(ref => {
            if (ref) {
              order.status = 'auto_refunded';
              order.refund_id = ref.id;
              order.refunded_at = nowIST();
              order.refund_method = 'RAZORPAY_UPI';
              saveDB(db);
              console.log(`✅ Order ${order_id} AUTO-REFUNDED via Razorpay (Refund ID: ${ref.id})`);
            }
          }).catch(e => console.error('Dry-run refund error:', e.message));
      }

      // Update transaction note
      const txn = db.transactions.find(t => t.id === order_id || (order.payment_id && t.payment_ref === order.payment_id));
      if (txn && !txn.note.includes('[TANK KHALI/NO WATER]') && !txn.note.includes('[AUTO-REFUNDED]')) {
        txn.note = `[TANK KHALI / MOTOR ERROR - Water Not Dispensed] ` + txn.note;
        txn.status = 'FAILED_NO_WATER';
      }
    } else {
      order.status = 'completed';
      order.completed_at = nowIST();
      console.log(`✅ Machine completed Order ${order_id} (${order.actual_dispensed_litres}L)`);
    }

    saveDB(db);
    return res.json({ success: true, message: `Order marked ${order.status}`, order });
  }
  res.status(404).json({ success: false, message: 'Order nahi mila' });
});

// ── ESP32 reports dry-run/no-water during card dispense to auto-refund card ──
app.post('/api/card-dispense-refund', (req, res) => {
  const { card_uid, deducted_litres, actual_litres, reason } = req.body;
  if (!card_uid) return res.status(400).json({ success: false, message: 'Card UID nahi mila' });

  const db = loadDB();
  const cleanUid = card_uid.replace(/\s+/g, '').toUpperCase();
  const c = db.customers.find(x => x.card_uid && x.card_uid.replace(/\s+/g, '').toUpperCase() === cleanUid);
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  const ratePerLitre = (db.price.paise_per_litre || 100) / 100.0;
  const reqLitres = parseFloat(deducted_litres) || 20.0;
  const actLitres = parseFloat(actual_litres) || 0.0;
  const unservedLitres = Math.max(0, reqLitres - actLitres);
  const refundPaise = Math.round(unservedLitres * ratePerLitre * 100);

  if (refundPaise > 0) {
    db.wallets[c.id] = (db.wallets[c.id] || 0) + refundPaise;
    const refTxnId = 'REF_' + uuidv4().substring(0, 8).toUpperCase();
    db.transactions.unshift({
      id: refTxnId,
      customer_id: c.id,
      customer_name: c.name,
      card_uid: c.card_uid,
      type: 'RECHARGE',
      amount_paise: refundPaise,
      note: `Auto-Refund: Tank Dry/No Water (${unservedLitres.toFixed(1)}L unserved)`,
      created_at: nowIST()
    });
    saveDB(db);
    console.log(`🚨 Auto-Refund: ₹${(refundPaise/100).toFixed(2)} refunded to ${c.name} (Tank Khali / Motor Error)`);
  }

  res.json({
    success: true,
    message: `✅ ₹${(refundPaise/100).toFixed(2)} auto-refunded to wallet`,
    refunded_rs: (refundPaise / 100).toFixed(2),
    new_balance_rs: ((db.wallets[c.id] || 0) / 100).toFixed(2)
  });
});

// ── ESP32 SYNC: Machine downloads authorized cards & balances for offline security ──
app.get('/api/machine/sync-cards', (req, res) => {
  const db = loadDB();
  const cards = db.customers
    .filter(c => c.card_uid)
    .map(c => {
      const cleanUid = c.card_uid.replace(/\s+/g, '').toUpperCase();
      const balPaise = db.wallets[c.id] || 0;
      return {
        uid: cleanUid,
        name: c.name,
        balance_rs: parseFloat((balPaise / 100.0).toFixed(2)),
        status: c.status || 'active',
        allow_offline: c.allow_offline !== false // default true
      };
    });

  res.json({
    success: true,
    price_per_litre_rs: parseFloat((db.price.paise_per_litre / 100.0).toFixed(2)),
    default_card_litres: parseFloat(db.settings.default_card_litres || 2),
    flow_stall_seconds: parseInt(db.settings.flow_stall_seconds || 6, 10),
    order_expiry_seconds: parseInt(db.settings.order_expiry_seconds || 90, 10),
    pulses_per_litre: parseFloat(db.settings.pulses_per_litre || 450.0),
    dispense_mode: db.settings.dispense_mode || 'button', // 'button' or 'auto'
    wait_button_seconds: parseInt(db.settings.wait_button_seconds || 60, 10),
    multi_quantity_enabled: db.settings.multi_quantity_enabled !== false,
    price_tiers: getPriceTiers(db),
    cards: cards
  });
});

// ── ESP32 OFFLINE SYNC: Machine uploads offline water dispenses to deduct balances ──
app.post('/api/machine/offline-sync', (req, res) => {
  const { records } = req.body; // Array of { card_uid, litres, timestamp }
  if (!records || !Array.isArray(records) || records.length === 0) {
    return res.json({ success: true, synced_count: 0, message: 'Koi offline record nahi mila' });
  }

  const db = loadDB();
  const ratePerLitre = (db.price.paise_per_litre || 100) / 100.0;
  let synced = 0;

  for (const r of records) {
    if (!r.card_uid) continue;
    const cleanUid = r.card_uid.replace(/\s+/g, '').toUpperCase();
    const c = db.customers.find(x => x.card_uid && x.card_uid.replace(/\s+/g, '').toUpperCase() === cleanUid);
    const litres = parseFloat(r.litres) || (db.settings.default_card_litres || 2.0);
    const costPaise = Math.round(litres * ratePerLitre * 100);

    const custId = c ? c.id : 'OFFLINE_' + cleanUid;
    const custName = c ? c.name : 'Unknown Card (' + cleanUid + ')';

    // Deduct balance if registered
    let cbEarnedPaise = 0;
    if (c) {
      db.wallets[c.id] = (db.wallets[c.id] || 0) - costPaise;
      if (litres >= 20) {
        cbEarnedPaise = Math.floor(litres / 20) * 100;
        db.cashback_wallets = db.cashback_wallets || {};
        db.cashback_wallets[c.id] = (db.cashback_wallets[c.id] || 0) + cbEarnedPaise;
      }
    }

    const txnId = 'OFF_' + uuidv4().substring(0, 8).toUpperCase();
    db.transactions.unshift({
      id: txnId,
      customer_id: custId,
      customer_name: custName,
      card_uid: c ? c.card_uid : cleanUid,
      type: 'DISPENSE',
      amount_paise: costPaise,
      note: `💧 Offline Dispense (${litres}L auto-synced)${cbEarnedPaise > 0 ? ` (+₹${cbEarnedPaise/100} Cashback)` : ''}`,
      payment_ref: txnId,
      created_at: nowIST()
    });
    synced++;
    console.log(`📶 Offline Sync processed: ${custName} -> ${litres}L (-₹${(costPaise/100).toFixed(2)})`);
  }

  saveDB(db);
  res.json({
    success: true,
    synced_count: synced,
    message: `✅ ${synced} offline records successfully sync ho gaye!`
  });
});

// ── TOGGLE OFFLINE ACCESS PER CUSTOMER ──
app.post('/api/customers/:id/toggle-offline', (req, res) => {
  const db = loadDB();
  const c = db.customers.find(x => x.id === req.params.id);
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  c.allow_offline = (c.allow_offline === false) ? true : false;
  saveDB(db);
  res.json({
    success: true,
    allow_offline: c.allow_offline,
    message: c.allow_offline ? `📶 Offline Access ALLOW (चालू) kar diya gaya!` : `🚫 Offline Access DENIED (बंद) kar diya gaya!`
  });
});

// ── GET/POST Machine Settings (Card Default Litres, Price, Razorpay Keys, Offline Safety Timer, Flow Stall Timer, Pulses Per Litre, Dispense Mode, Buttons) ──
app.get('/api/settings', (req, res) => {
  const db = loadDB();
  res.json({
    success: true,
    price_per_litre_rs: (db.price.paise_per_litre / 100).toFixed(2),
    default_card_litres: db.settings.default_card_litres || 20,
    order_expiry_seconds: db.settings.order_expiry_seconds || 90,
    flow_stall_seconds: db.settings.flow_stall_seconds || 6,
    pulses_per_litre: parseFloat(db.settings.pulses_per_litre || 450.0),
    dispense_mode: db.settings.dispense_mode || 'button', // 'button' or 'auto'
    wait_button_seconds: parseInt(db.settings.wait_button_seconds || 60, 10),
    multi_quantity_enabled: db.settings.multi_quantity_enabled !== false,
    auto_refund_upi: db.settings.auto_refund_upi !== false,
    price_tiers: getPriceTiers(db),
    razorpay_key_id: db.settings.razorpay_key_id || RAZORPAY_KEY_ID,
    razorpay_key_secret: db.settings.razorpay_key_secret ? '••••••••••••••••' : '••••••••••••••••'
  });
});

app.post('/api/settings', (req, res) => {
  const { default_card_litres, price_per_litre_rs, razorpay_key_id, razorpay_key_secret, order_expiry_seconds, flow_stall_seconds, pulses_per_litre, dispense_mode, wait_button_seconds, multi_quantity_enabled, auto_refund_upi, price_tiers } = req.body;
  if (!verifyAdmin(req)) {
    return res.status(403).json({ success: false, message: '❌ Unauthorized! कृपया सही एडमिन पासवर्ड से लॉगिन करें।' });
  }
  const db = loadDB();
  if (default_card_litres !== undefined) {
    const dL = parseFloat(default_card_litres);
    if (!isNaN(dL) && dL > 0) db.settings.default_card_litres = dL;
  }
  if (price_per_litre_rs !== undefined) {
    const pL = parseFloat(price_per_litre_rs);
    if (!isNaN(pL) && pL > 0) db.price.paise_per_litre = Math.round(pL * 100);
  }
  if (order_expiry_seconds !== undefined) {
    const expSec = parseInt(order_expiry_seconds, 10);
    if (!isNaN(expSec) && expSec >= 15) {
      db.settings.order_expiry_seconds = expSec;
    }
  }
  if (flow_stall_seconds !== undefined) {
    const fSec = parseInt(flow_stall_seconds, 10);
    if (!isNaN(fSec) && fSec >= 2 && fSec <= 60) {
      db.settings.flow_stall_seconds = fSec;
    }
  }
  if (pulses_per_litre !== undefined) {
    const ppl = parseFloat(pulses_per_litre);
    if (!isNaN(ppl) && ppl >= 50 && ppl <= 3000) {
      db.settings.pulses_per_litre = ppl;
    }
  }
  if (dispense_mode !== undefined) {
    db.settings.dispense_mode = (dispense_mode === 'auto') ? 'auto' : 'button';
  }
  if (wait_button_seconds !== undefined) {
    const wb = parseInt(wait_button_seconds, 10);
    if (!isNaN(wb) && wb >= 10 && wb <= 300) {
      db.settings.wait_button_seconds = wb;
    }
  }
  if (multi_quantity_enabled !== undefined) {
    db.settings.multi_quantity_enabled = (multi_quantity_enabled === true || multi_quantity_enabled === 'true');
  }
  
  if (price_tiers && typeof price_tiers === 'object') {
    db.settings.price_tiers = {
      "1": parseFloat(price_tiers["1"] !== undefined ? price_tiers["1"] : 1.0),
      "5": parseFloat(price_tiers["5"] !== undefined ? price_tiers["5"] : 5.0),
      "10": parseFloat(price_tiers["10"] !== undefined ? price_tiers["10"] : 10.0),
      "20": parseFloat(price_tiers["20"] !== undefined ? price_tiers["20"] : 15.0)
    };
  }

  if (auto_refund_upi !== undefined) {
    db.settings.auto_refund_upi = (auto_refund_upi === true || auto_refund_upi === 'true');
  }
  if (razorpay_key_id && razorpay_key_id.trim()) {
    db.settings.razorpay_key_id = razorpay_key_id.trim();
  }
  if (razorpay_key_secret && razorpay_key_secret.trim() && !razorpay_key_secret.includes('•')) {
    db.settings.razorpay_key_secret = razorpay_key_secret.trim();
  }
  saveDB(db);
  initRazorpayFromDB();

  console.log(`⚙️ Settings updated: Rate=₹${(db.price.paise_per_litre/100).toFixed(2)}/L, Card Default=${db.settings.default_card_litres}L, Mode=${db.settings.dispense_mode || 'button'}, WaitBtn=${db.settings.wait_button_seconds || 60}s, MultiQty=${db.settings.multi_quantity_enabled !== false}, Pulses/L=${db.settings.pulses_per_litre || 450}`);
  res.json({
    success: true,
    message: '✅ Settings update ho gayi!',
    default_card_litres: db.settings.default_card_litres,
    order_expiry_seconds: db.settings.order_expiry_seconds || 90,
    flow_stall_seconds: db.settings.flow_stall_seconds || 6,
    pulses_per_litre: parseFloat(db.settings.pulses_per_litre || 450.0),
    dispense_mode: db.settings.dispense_mode || 'button',
    wait_button_seconds: db.settings.wait_button_seconds || 60,
    multi_quantity_enabled: db.settings.multi_quantity_enabled !== false,
    auto_refund_upi: db.settings.auto_refund_upi !== false,
    price_per_litre_rs: (db.price.paise_per_litre / 100).toFixed(2),
    razorpay_key_id: db.settings.razorpay_key_id || RAZORPAY_KEY_ID
  });
});

// ── GET transactions (all or per customer) ──
app.get('/api/transactions', (req, res) => {
  const db = loadDB();
  res.json({ success: true, data: db.transactions.slice(0, 100) });
});

app.get('/api/transactions/:customer_id', (req, res) => {
  const db   = loadDB();
  const data = db.transactions.filter(t => t.customer_id === req.params.customer_id).slice(0, 50);
  res.json({ success: true, data });
});

// ── GET dashboard stats ──
app.get('/api/stats', (req, res) => {
  const db = loadDB();
  const today = new Date().toLocaleDateString('en-IN', { timeZone: 'Asia/Kolkata' });
  const todayTxns = db.transactions.filter(t => t.created_at.startsWith(today.split('/').reverse().join('-')) || t.created_at.includes(today));

  res.json({
    success: true,
    data: {
      total_customers: db.customers.length,
      active_cards:    db.customers.filter(c => c.status === 'active' && c.card_uid).length,
      blocked_cards:   db.customers.filter(c => c.status === 'blocked').length,
      total_balance_rs: (Object.values(db.wallets).reduce((a, b) => a + b, 0) / 100).toFixed(2),
      today_recharge_rs: (db.transactions.filter(t => t.type === 'RECHARGE').slice(0, 20).reduce((s, t) => s + t.amount_paise, 0) / 100).toFixed(2),
      today_dispense_rs: (db.transactions.filter(t => t.type === 'DISPENSE').slice(0, 20).reduce((s, t) => s + t.amount_paise, 0) / 100).toFixed(2),
      price_per_litre_rs: (db.price.paise_per_litre / 100).toFixed(2)
    }
  });
});

// ── GET/POST water price ──
app.get('/api/price', (req, res) => {
  const db = loadDB();
  res.json({ success: true, paise_per_litre: db.price.paise_per_litre, rs_per_litre: (db.price.paise_per_litre / 100).toFixed(2) });
});

app.post('/api/price', (req, res) => {
  const { rs_per_litre } = req.body;
  if (!verifyAdmin(req))
    return res.status(403).json({ success: false, message: '❌ Unauthorized! कृपया सही एडमिन पासवर्ड से लॉगिन करें।' });
  const db = loadDB();
  db.price.paise_per_litre = Math.round((parseFloat(rs_per_litre) || 1) * 100);
  saveDB(db);
  res.json({ success: true, message: `Price update: ₹${rs_per_litre} per litre` });
});

// ── GET QR code for customer (ALWAYS Live Global Cloud URL) ──
app.get('/api/qr/:customer_id', async (req, res) => {
  const db = loadDB();
  const rawId = req.params.customer_id;
  const c  = db.customers.find(x => x.id === rawId || (x.card_uid && x.card_uid.replace(/\s+/g,'').toUpperCase() === rawId.replace(/\s+/g,'').toUpperCase()));
  if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

  // ALWAYS use permanent global URL so QR works on customer mobile data anywhere!
  const url = `https://asha-ro-water.onrender.com/pay/${c.id}`;

  try {
    const qrDataUrl = await QRCode.toDataURL(url, {
      width: 400, margin: 2,
      color: { dark: '#0284c7', light: '#FFFFFF' }
    });
    res.json({
      success: true,
      id: c.id,
      customer_id: c.id,
      qr_url: url,
      qr_image: qrDataUrl,
      customer_name: c.name,
      card_uid: c.card_uid || '',
      phone: c.phone || '',
      balance_rs: ((db.wallets[c.id] || 0) / 100).toFixed(2)
    });
  } catch (e) {
    res.status(500).json({ success: false, message: e.message });
  }
});

// ── GET 2-Sided QR Codes for Customer RFID Card (Direct UPI + Live Passbook) ──
app.get('/api/customer-card-qrs/:customer_id', async (req, res) => {
  try {
    const db = loadDB();
    const rawId = req.params.customer_id;
    const c = db.customers.find(x => x.id === rawId || (x.card_uid && x.card_uid.replace(/\s+/g,'').toUpperCase() === rawId.replace(/\s+/g,'').toUpperCase()));
    if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

    // 1. Create or fetch Razorpay Direct UPI QR Code (Side 1)
    if (!c.razorpay_qr_id || !c.razorpay_qr_image) {
      console.log(`⚡ Generating dedicated Razorpay UPI QR for ${c.name}...`);
      try {
        const qr = await razorpay.qrCode.create({
          type: 'upi_qr',
          name: c.name.slice(0, 30),
          usage: 'multiple_use',
          fixed_amount: false,
          description: `Card Recharge: ${c.name}`,
          notes: {
            type: 'CARD_RECHARGE',
            card_uid: c.card_uid || '',
            customer_id: c.id
          }
        });
        c.razorpay_qr_id = qr.id;
        c.razorpay_qr_image = qr.image_url;
        saveDB(db);
        console.log(`✅ Razorpay UPI QR created: ${qr.id} (${qr.image_url})`);
      } catch (qrErr) {
        console.error('❌ Razorpay QR create error:', qrErr);
      }
    }

    // Generate pure square UPI QR (prevents any rectangular banner squishing)
    const qrIdClean = (c.razorpay_qr_id || '').replace(/^qr_/, '');
    const upiIntent = qrIdClean ? `upi://pay?cu=INR&mc=5817&mode=19&pa=ashaenterprises673694.rzp@rxairtel&tn=Payment%20To%20ASHA%20ENTERPRISES&tr=${qrIdClean}qrv2` : '';
    const squareUpiQr = upiIntent ? await QRCode.toDataURL(upiIntent, {
      width: 450, margin: 2,
      color: { dark: '#15803d', light: '#FFFFFF' }
    }).catch(() => '') : '';

    // 2. Generate Web Passbook & Balance Enquiry QR (Side 2)
    const passbookUrl = `https://asha-ro-water.onrender.com/pay/${c.id}`;
    const passbookQrDataUrl = await QRCode.toDataURL(passbookUrl, {
      width: 400, margin: 2,
      color: { dark: '#0369a1', light: '#FFFFFF' }
    });

    res.json({
      success: true,
      customer: {
        id: c.id,
        name: c.name,
        card_uid: c.card_uid || '',
        phone: c.phone || '',
        balance_rs: ((db.wallets[c.id] || 0) / 100).toFixed(2),
        rate_rs: ((db.price.paise_per_litre || 100) / 100).toFixed(2)
      },
      // Side 1: Pure Square UPI QR (PhonePe / GPay)
      side1_upi_qr: squareUpiQr || c.razorpay_qr_image || '',
      side1_upi_intent: upiIntent,
      // Side 2: Passbook & Balance Enquiry QR
      side2_passbook_url: passbookUrl,
      side2_passbook_qr: passbookQrDataUrl
    });
  } catch (err) {
    console.error('❌ Error in customer-card-qrs:', err);
    res.status(500).json({ success: false, message: err.message });
  }
});

// ── Customer payment page info ──
app.get('/api/pay-info/:customer_id', async (req, res) => {
  try {
    const db = loadDB();
    const rawId = req.params.customer_id;
    const c  = db.customers.find(x => x.id === rawId || (x.card_uid && x.card_uid.replace(/\s+/g,'').toUpperCase() === rawId.replace(/\s+/g,'').toUpperCase()));
    if (!c) return res.status(404).json({ success: false, message: 'Customer nahi mila' });

    // Generate direct UPI QR if not present
    if (!c.razorpay_qr_id || !c.razorpay_qr_image) {
      try {
        const qr = await razorpay.qrCode.create({
          type: 'upi_qr',
          name: c.name.slice(0, 30),
          usage: 'multiple_use',
          fixed_amount: false,
          description: `Card Recharge: ${c.name}`,
          notes: {
            type: 'CARD_RECHARGE',
            card_uid: c.card_uid || '',
            customer_id: c.id
          }
        });
        c.razorpay_qr_id = qr.id;
        c.razorpay_qr_image = qr.image_url;
        saveDB(db);
      } catch(e) {
        console.warn('QR auto-create warning in pay-info:', e.message);
      }
    }

    const nameParts = c.name.split(' ');
    const displayName = nameParts[0] + (nameParts[1] ? ' ' + nameParts[1][0] + '.' : '');
    const cbPaise = (db.cashback_wallets && db.cashback_wallets[c.id]) || 0;

    // Generate pure square UPI QR data URL
    const qrIdClean = (c.razorpay_qr_id || '').replace(/^qr_/, '');
    const upiIntent = qrIdClean ? `upi://pay?cu=INR&mc=5817&mode=19&pa=ashaenterprises673694.rzp@rxairtel&tn=Payment%20To%20ASHA%20ENTERPRISES&tr=${qrIdClean}qrv2` : '';
    const squareUpiQr = upiIntent ? await QRCode.toDataURL(upiIntent, {
      width: 450, margin: 2,
      color: { dark: '#0f172a', light: '#FFFFFF' }
    }).catch(() => '') : '';

    res.json({
      success: true,
      id: c.id,
      display_name: displayName,
      full_name: c.name,
      card_uid: c.card_uid || '',
      phone: c.phone || '',
      status: c.status,
      balance_rs: ((db.wallets[c.id] || 0) / 100).toFixed(2),
      cashback_rs: (cbPaise / 100).toFixed(2),
      upi_qr_data: squareUpiQr || c.razorpay_qr_image || '',
      upi_intent: upiIntent,
      razorpay_qr_image: c.razorpay_qr_image || '',
      price_per_litre: ((db.price.paise_per_litre || 100) / 100).toFixed(2)
    });
  } catch(err) {
    res.status(500).json({ success: false, message: err.message });
  }
});

// ── Shortcut: /card/:uid opens payment/recharge page directly ──
app.get('/card/:card_uid', (req, res) => {
  const db = loadDB();
  const searchUid = decodeURIComponent(req.params.card_uid || '').replace(/\s+/g, '').toUpperCase();
  const c = db.customers.find(x => x.card_uid && x.card_uid.replace(/\s+/g, '').toUpperCase() === searchUid);
  if (c) {
    return res.redirect(`/pay/${c.id}`);
  }
  res.sendFile(path.join(__dirname, 'public', 'pay.html'));
});

// ── Card Scan Bridge endpoint (PowerShell bridge & ESP32 POST here) ──
app.post('/api/card-scan', (req, res) => {
  const uid = (req.body.uid || '').toUpperCase().trim();
  if (!uid) return res.status(400).json({ success: false, message: 'UID nahi mila' });
  const unreg = recordUnregisteredCard(uid);
  console.log(`📡 Card Scanned via bridge/machine: ${uid}`);
  res.json({ success: true, uid, unreg });
});

// ── GET list of recent unregistered cards (for Admin Live Alert) ──
app.get('/api/unregistered-cards', (req, res) => {
  const db = loadDB();
  const registeredSet = new Set(db.customers.filter(c => c.card_uid).map(c => c.card_uid.replace(/\s+/g, '').toUpperCase()));
  // Filter out any card that got registered
  const activeUnregistered = unregisteredCards.filter(u => !registeredSet.has(u.uid.replace(/\s+/g, '')));
  res.json({
    success: true,
    latest: activeUnregistered[0] || null,
    cards: activeUnregistered
  });
});

// ── DELETE / dismiss an unregistered card from queue ──
app.delete('/api/unregistered-cards/:uid', (req, res) => {
  const target = (req.params.uid || '').replace(/\s+/g, '').toUpperCase();
  unregisteredCards = unregisteredCards.filter(u => u.uid.replace(/\s+/g, '') !== target);
  res.json({ success: true });
});

// ── Frontend polls this to get latest scanned card ──
app.get('/api/last-scan', (req, res) => {
  // Only return if scanned within last 30 seconds
  const age = lastScannedCard.scanned_at ? (Date.now() - lastScannedCard.scanned_at) : 99999;
  if (lastScannedCard.uid && age < 30000) {
    res.json({ success: true, uid: lastScannedCard.uid, age_ms: age });
  } else {
    res.json({ success: false, uid: null });
  }
});

// ── Clear last scan ──
app.delete('/api/last-scan', (req, res) => {
  lastScannedCard = { uid: null, scanned_at: null };
  res.json({ success: true });
});

// ─── CUSTOMER QR PAYMENT PAGE & MACHINE DIRECT QR ─────────────
app.get('/water', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'water.html'));
});

app.get('/pay', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'pay.html'));
});

app.get('/pay/:customer_id', (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'pay.html'));
});

// ── Razorpay & PayU Mandatory Compliance Pages ──
app.get('/contact', (req, res) => res.sendFile(path.join(__dirname, 'public', 'contact.html')));
app.get('/terms', (req, res) => res.sendFile(path.join(__dirname, 'public', 'terms.html')));
app.get('/privacy', (req, res) => res.sendFile(path.join(__dirname, 'public', 'privacy.html')));
app.get('/refund', (req, res) => res.sendFile(path.join(__dirname, 'public', 'refund.html')));
app.get('/about', (req, res) => res.sendFile(path.join(__dirname, 'public', 'about.html')));
app.get('/delivery', (req, res) => res.sendFile(path.join(__dirname, 'public', 'delivery.html')));
app.get('/shipping', (req, res) => res.sendFile(path.join(__dirname, 'public', 'delivery.html')));

// ── GET Machine QR Code (Print karne ke liye) ──
app.get('/api/machine-qr', async (req, res) => {
  const amt = req.query.amt ? parseInt(req.query.amt) : 0;
  const auto = req.query.auto === '1' || req.query.auto === 'true';
  let url = `https://asha-ro-water.onrender.com/water`;
  let title = "ASHA RO WATER";
  if (amt > 0) {
    url += `?amt=${amt}` + (auto ? '&auto=1' : '');
    title = `ASHA RO WATER — ₹${amt} (${amt}L)`;
  }
  try {
    const qrDataUrl = await QRCode.toDataURL(url, {
      width: 360, margin: 2,
      color: { dark: '#002244', light: '#FFFFFF' }
    });
    res.json({ success: true, qr_url: url, qr_image: qrDataUrl, amt, auto, title });
  } catch(e) {
    res.status(500).json({ success: false, message: e.message });
  }
});

// ── Root → Customer Water Storefront & Dispenser Homepage ──
app.get('/', (req, res) => res.sendFile(path.join(__dirname, 'public', 'water.html')));
app.get('/water', (req, res) => res.sendFile(path.join(__dirname, 'public', 'water.html')));

// ── Admin Management Panel ──
app.get('/admin', (req, res) => res.sendFile(path.join(__dirname, 'public', 'index.html')));

// ─── START ────────────────────────────────────────────────────
initDatabase().then(() => {
  app.listen(PORT, '0.0.0.0', () => {
    const ip = getServerIP();
    console.log('\n╔══════════════════════════════════════════════════╗');
    console.log('║    🚰  ASHA RO WATER - Server Started!            ║');
    console.log('╠══════════════════════════════════════════════════╣');
    console.log(`║  Port:        ${PORT}                               ║`);
    console.log('║  Admin Portal → https://asha-ro-water.onrender.com/admin║');
    console.log('║  Customer QR  → /pay/<customer_id>               ║');
    console.log('║  Data Saved   → Supabase Cloud PostgreSQL DB     ║');
    console.log('╚══════════════════════════════════════════════════╝\n');
  });
}).catch(err => {
  console.error('Fatal startup error in initDatabase:', err);
  app.listen(PORT, '0.0.0.0', () => {
    console.log(`Server started in fallback mode on port ${PORT}`);
  });
});
