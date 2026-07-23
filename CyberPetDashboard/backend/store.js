'use strict';

const fs   = require('fs');
const path = require('path');

// ── Shared constants / helpers ────────────────────────────────────────────────

const DEVICE_NAME_MAX  = 23;   // firmware truncates habit names at this length
const HISTORY_KEEP_DAYS = 365; // rolling window; oldest entries pruned on each write

// Truncation-aware name comparison — mirrors firmware's strncmp(a, b, HABIT_NAME_LEN-1).
// A plain === misses when one side was truncated to 23 chars on-device.
function namesMatch(a, b) {
  if (a === b) return true;
  return (a.length >= DEVICE_NAME_MAX || b.length >= DEVICE_NAME_MAX)
      && a.slice(0, DEVICE_NAME_MAX) === b.slice(0, DEVICE_NAME_MAX);
}
const Database = require('better-sqlite3');

const DATA_DIR  = path.join(__dirname, 'data');
const DB_FILE   = path.join(DATA_DIR, 'store.db');
const JSON_FILE = path.join(DATA_DIR, 'store.json');  // legacy — migrated on first run

// ── Default data ─────────────────────────────────────────────────────────────

const DEFAULT_TODOS = [
  // ── hardware ──────────────────────────────────────────────────────────────
  { id: 1,  text: 'Integrate onboard PCF85063 RTC for accurate standalone midnight resets (no soldering — chip is onboard on 1.75)', done: true,  category: 'firmware' },
  { id: 2,  text: 'Test firmware build on actual ESP32-S3 device', done: false, category: 'hardware' },
  { id: 3,  text: 'Test touch input with the capacitive touch driver from Waveshare\'s 1.75 example', done: false, category: 'hardware' },
  // ── firmware ──────────────────────────────────────────────────────────────
  { id: 4,  text: 'Set WiFi credentials in CyberPet.ino and test sync', done: false, category: 'firmware' },
  { id: 5,  text: 'Verify NVS storage survives deep sleep / power cycle', done: false, category: 'firmware' },
  { id: 6,  text: 'Add on-device habit list editing via touchscreen', done: false, category: 'firmware' },
  { id: 7,  text: 'Add XP notification animation on habit complete', done: true,  category: 'firmware' },
  // ── ui / sim ──────────────────────────────────────────────────────────────
  { id: 8,  text: 'Blob squash-and-stretch on roam landing', done: true,  category: 'ui' },
  { id: 9,  text: 'Eyes follow touch / cursor direction', done: false, category: 'ui' },
  { id: 10, text: 'Particle burst on level-up / evolution', done: true,  category: 'ui' },
  { id: 11, text: 'Test all 4 evolution stages on real AMOLED display', done: false, category: 'ui' },
  // ── dashboard ─────────────────────────────────────────────────────────────
  { id: 12, text: 'Add streak chart / completion history heatmap', done: true,  category: 'dashboard' },
  { id: 13, text: 'Export pet + habit history to JSON backup', done: true,  category: 'dashboard' },
  { id: 14, text: 'Add push-style config reload so device picks up changes instantly', done: true,  category: 'dashboard' },

  // ── known gaps (from README) ───────────────────────────────────────────────
  { id: 15, text: 'RTC midnight reset via onboard PCF85063 — replace the millis() standalone fallback (NTP wall-clock path already done; see #1)', done: true,  category: 'firmware' },
  { id: 16, text: 'Wire dashboard settings into firmware: moodGainPerHabit, moodDecayPerMiss, dailyResetHour', done: true,  category: 'firmware' },
  { id: 17, text: 'Add serverId to Habit struct — fix rename-resets-streak (currently name-based reconciliation)', done: true,  category: 'firmware' },
  { id: 18, text: 'Sync goals to on-device screen (API + dashboard UI exist; wifi_sync.cpp not wired)', done: false, category: 'firmware' },
  { id: 19, text: 'Upgrade ArduinoJson v6 → v7: rename StaticJsonDocument to JsonDocument (two-line change)', done: true,  category: 'firmware' },
  { id: 20, text: 'Store completion history per-date (server currently keeps only latest completion list)', done: true,  category: 'dashboard' },
  { id: 21, text: 'Add auth before exposing dashboard outside LAN — no auth currently', done: false, category: 'dashboard' },
  { id: 22, text: 'Swap single JSON file store for SQLite/DB for history & analytics (isolated in store.js)', done: true,  category: 'dashboard' },

  // ── quick wins (roadmap) ──────────────────────────────────────────────────
  { id: 23, text: 'Longest-streak display on pet screen (per-habit streaks already shown on habit list rows)', done: false, category: 'ui' },
  { id: 24, text: 'Completion celebration — confetti / particle burst on habit check-off (XP popup exists)', done: false, category: 'ui' },
  { id: 25, text: 'Pet idle animations: slow bounce, blink overlay, mood-based wobble speed', done: true,  category: 'ui' },
  { id: 26, text: 'Pet regression: low mood can demote evolution stage (check in checkEvolution)', done: true,  category: 'firmware' },

  // ── medium projects (roadmap) ─────────────────────────────────────────────
  { id: 27, text: 'Pomodoro mode: ring countdown on round screen, XP per completed block, IMU guilt-trip on pickup mid-focus', done: true,  category: 'ui' },
  { id: 28, text: 'Custom pixel-art sprites per evolution stage (LVGL image converter, PNG → C array)', done: false, category: 'ui' },
  { id: 29, text: 'QMI8658 IMU: wake screen on pickup, blob dizzy animation on shake', done: false, category: 'hardware' },
  { id: 30, text: 'AXP2101 battery status indicator — warn when pet is "getting sleepy"', done: true,  category: 'hardware' },
  { id: 31, text: 'Movement / sedentary nudge: droopy blob + mood tick after ~1 h of no movement', done: true,  category: 'ui' },

  // ── bigger ideas (roadmap) ────────────────────────────────────────────────
  { id: 32, text: 'n8n / self-hosted webhook on completion events: Discord alerts, DB logging, evening reminders', done: false, category: 'dashboard' },
  { id: 33, text: 'Habitica bridge — sync completions to a real Habitica account via their open API', done: false, category: 'dashboard' },
  { id: 34, text: 'Step counting: QMI8658 pedometer in carry mode, or phone-as-sensor posting to /api/sync', done: false, category: 'hardware' },
  { id: 35, text: 'Port to Waveshare 1.75" AMOLED — only display/touch init block in CyberPet.ino changes', done: true,  category: 'hardware' },
];

const DEFAULT_DATA = {
  habits: [
    { id: 1, name: 'Drink water',      xpValue: 5,  active: true },
    { id: 2, name: 'Move body',        xpValue: 10, active: true },
    { id: 3, name: 'Practice guitar',  xpValue: 10, active: true },
    { id: 4, name: 'Read / study',     xpValue: 15, active: true }
  ],
  goals: [],
  quests: [],
  todos: DEFAULT_TODOS,
  devNotes: '',
  completionLog: [],
  nextLogId: 1,
  configVersion: 1,
  configUpdatedAt: null,
  settings: {
    petName: 'Blob',
    dailyResetHour: 0,
    moodDecayPerMiss: 15,
    moodGainPerHabit: 8,
    syncIntervalSeconds: 60,
    deviceId: null
  },
  petState: {
    xp: 0,
    stage: 0,
    mood: 80,
    daysAlive: 0,
    hunger: 100,
    alive: true,
    lastSyncedAt: null
  },
  nextHabitId: 5,
  nextGoalId: 1,
  nextQuestId: 1,
  nextTodoId: DEFAULT_TODOS.length + 1,
  dashXpTotal: 0
};

// ── DB init ───────────────────────────────────────────────────────────────────

let _db   = null;
let cache = null;

function getDb() {
  if (_db) return _db;
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  _db = new Database(DB_FILE);
  _db.pragma('journal_mode = WAL');
  _db.exec(`
    CREATE TABLE IF NOT EXISTS habits (
      id       INTEGER PRIMARY KEY,
      name     TEXT    NOT NULL,
      xp_value INTEGER NOT NULL DEFAULT 0,
      active   INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS goals (
      id       INTEGER PRIMARY KEY,
      name     TEXT    NOT NULL,
      xp_value INTEGER NOT NULL DEFAULT 0,
      period   TEXT    NOT NULL DEFAULT 'weekly',
      active   INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS quests (
      id          INTEGER PRIMARY KEY,
      name        TEXT    NOT NULL,
      xp_value    INTEGER NOT NULL DEFAULT 0,
      description TEXT    NOT NULL DEFAULT '',
      done        INTEGER NOT NULL DEFAULT 0,
      done_at     TEXT,
      active      INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS todos (
      id       INTEGER PRIMARY KEY,
      text     TEXT    NOT NULL,
      done     INTEGER NOT NULL DEFAULT 0,
      category TEXT    NOT NULL DEFAULT 'general'
    );
    CREATE TABLE IF NOT EXISTS completion_log (
      id           INTEGER PRIMARY KEY,
      habit_id     INTEGER,
      habit_name   TEXT    NOT NULL,
      xp_value     INTEGER NOT NULL DEFAULT 0,
      date         TEXT    NOT NULL,
      completed_at TEXT    NOT NULL,
      device_id    TEXT
    );
    CREATE INDEX IF NOT EXISTS idx_log_date ON completion_log(date);
    CREATE TABLE IF NOT EXISTS kv (
      key   TEXT PRIMARY KEY,
      value TEXT NOT NULL
    );
  `);
  // Migration: add device_id column to DBs created before this field existed.
  // SQLite throws on duplicate column names so we swallow that error.
  try { _db.exec('ALTER TABLE completion_log ADD COLUMN device_id TEXT'); } catch (_) {}
  return _db;
}

// ── Read / write helpers ──────────────────────────────────────────────────────

function kvGet(db, key, fallback) {
  const row = db.prepare('SELECT value FROM kv WHERE key = ?').get(key);
  return row !== undefined ? JSON.parse(row.value) : fallback;
}

function readFromDb(db) {
  const habits = db.prepare('SELECT id, name, xp_value, active FROM habits').all()
    .map(r => ({ id: r.id, name: r.name, xpValue: r.xp_value, active: !!r.active }));

  const goals = db.prepare('SELECT id, name, xp_value, period, active FROM goals').all()
    .map(r => ({ id: r.id, name: r.name, xpValue: r.xp_value, period: r.period, active: !!r.active }));

  const quests = db.prepare('SELECT id, name, xp_value, description, done, done_at, active FROM quests').all()
    .map(r => ({ id: r.id, name: r.name, xpValue: r.xp_value, description: r.description,
                 done: !!r.done, doneAt: r.done_at, active: !!r.active }));

  const todos = db.prepare('SELECT id, text, done, category FROM todos').all()
    .map(r => ({ id: r.id, text: r.text, done: !!r.done, category: r.category }));

  const completionLog = db.prepare(
    'SELECT id, habit_id, habit_name, xp_value, date, completed_at, device_id FROM completion_log'
  ).all().map(r => ({
    id: r.id, habitId: r.habit_id, habitName: r.habit_name,
    xpValue: r.xp_value, date: r.date, completedAt: r.completed_at,
    deviceId: r.device_id
  }));

  return {
    habits, goals, quests, todos, completionLog,
    devNotes:        kvGet(db, 'devNotes',        ''),
    nextHabitId:     kvGet(db, 'nextHabitId',     DEFAULT_DATA.nextHabitId),
    nextGoalId:      kvGet(db, 'nextGoalId',      DEFAULT_DATA.nextGoalId),
    nextQuestId:     kvGet(db, 'nextQuestId',     DEFAULT_DATA.nextQuestId),
    nextTodoId:      kvGet(db, 'nextTodoId',      DEFAULT_DATA.nextTodoId),
    nextLogId:       kvGet(db, 'nextLogId',       DEFAULT_DATA.nextLogId),
    dashXpTotal:     kvGet(db, 'dashXpTotal',     0),
    configVersion:   kvGet(db, 'configVersion',   1),
    configUpdatedAt: kvGet(db, 'configUpdatedAt', null),
    settings:        kvGet(db, 'settings',        DEFAULT_DATA.settings),
    petState:        kvGet(db, 'petState',        DEFAULT_DATA.petState),
    // Device-analytics fields. These were FORGOTTEN in the July 2026 SQLite
    // migration: server.js kept writing them onto the in-memory object, but
    // they never reached the kv table, so every container restart wiped
    // step/sleep/workout history (the session counters silently re-credited
    // the device's lifetime totals to "today" after each restart).
    stepHistory:     kvGet(db, 'stepHistory',     {}),
    sleepHistory:    kvGet(db, 'sleepHistory',    {}),
    backHistory:     kvGet(db, 'backHistory',     {}),
    pushHistory:     kvGet(db, 'pushHistory',     {}),
    pullupHistory:   kvGet(db, 'pullupHistory',   {}),
    focusHistory:    kvGet(db, 'focusHistory',    {}),
    backSessions:    kvGet(db, 'backSessions',    0),
    pushSessions:    kvGet(db, 'pushSessions',    0),
    pullupSessions:  kvGet(db, 'pullupSessions',  0),
    focusSessions:   kvGet(db, 'focusSessions',   0),
    // One-shot device-command tokens. petResetToken had the same
    // forgotten-field bug as the history keys above: a container restart
    // reset it to 0 — below the device's NVS-stored last-applied value —
    // so dashboard XP resets silently stopped firing until the counter
    // caught back up.
    petResetToken:   kvGet(db, 'petResetToken',   0),
    dictPushToken:   kvGet(db, 'dictPushToken',   0),
  };
}

const KV_KEYS = [
  'devNotes', 'nextHabitId', 'nextGoalId', 'nextQuestId', 'nextTodoId',
  'nextLogId', 'dashXpTotal', 'configVersion', 'configUpdatedAt', 'settings', 'petState',
  'stepHistory', 'sleepHistory', 'backHistory', 'pushHistory', 'pullupHistory',
  'focusHistory', 'backSessions', 'pushSessions', 'pullupSessions', 'focusSessions',
  'petResetToken', 'dictPushToken'
];

function writeToDb(db, data) {
  db.transaction(() => {
    db.prepare('DELETE FROM habits').run();
    const ih = db.prepare('INSERT INTO habits (id, name, xp_value, active) VALUES (?,?,?,?)');
    for (const h of data.habits) ih.run(h.id, h.name, h.xpValue, h.active ? 1 : 0);

    db.prepare('DELETE FROM goals').run();
    const ig = db.prepare('INSERT INTO goals (id, name, xp_value, period, active) VALUES (?,?,?,?,?)');
    for (const g of data.goals) ig.run(g.id, g.name, g.xpValue, g.period, g.active ? 1 : 0);

    db.prepare('DELETE FROM quests').run();
    const iq = db.prepare(
      'INSERT INTO quests (id, name, xp_value, description, done, done_at, active) VALUES (?,?,?,?,?,?,?)'
    );
    for (const q of data.quests)
      iq.run(q.id, q.name, q.xpValue, q.description || '', q.done ? 1 : 0, q.doneAt ?? null, q.active ? 1 : 0);

    db.prepare('DELETE FROM todos').run();
    const it = db.prepare('INSERT INTO todos (id, text, done, category) VALUES (?,?,?,?)');
    for (const t of data.todos) it.run(t.id, t.text, t.done ? 1 : 0, t.category);

    db.prepare('DELETE FROM completion_log').run();
    const il = db.prepare(
      'INSERT INTO completion_log (id, habit_id, habit_name, xp_value, date, completed_at, device_id) VALUES (?,?,?,?,?,?,?)'
    );
    for (const e of data.completionLog)
      il.run(e.id, e.habitId ?? null, e.habitName, e.xpValue, e.date, e.completedAt, e.deviceId ?? null);

    const uk = db.prepare('INSERT OR REPLACE INTO kv (key, value) VALUES (?,?)');
    for (const k of KV_KEYS) {
      if (data[k] !== undefined) uk.run(k, JSON.stringify(data[k]));
    }
  })();
}

// ── Migration from store.json ─────────────────────────────────────────────────

function migrateFromJson(db) {
  if (!fs.existsSync(JSON_FILE)) return;

  const hasData =
    db.prepare('SELECT COUNT(*) as n FROM habits').get().n > 0 ||
    db.prepare('SELECT COUNT(*) as n FROM kv').get().n > 0;

  if (hasData) {
    // SQLite already populated; retire the JSON file
    try { fs.renameSync(JSON_FILE, JSON_FILE + '.migrated'); } catch (_) {}
    return;
  }

  try {
    const raw = JSON.parse(fs.readFileSync(JSON_FILE, 'utf-8'));

    // Same field migrations as the old load()
    if (!raw.quests)                          raw.quests = [];
    if (!raw.todos)                           { raw.todos = DEFAULT_TODOS; raw.nextTodoId = DEFAULT_TODOS.length + 1; }
    if (raw.devNotes === undefined)           raw.devNotes = '';
    if (!raw.nextQuestId)                     raw.nextQuestId = 1;
    if (!raw.completionLog)                   raw.completionLog = [];
    if (!raw.nextLogId)                       raw.nextLogId = 1;
    if (!raw.configVersion)                   raw.configVersion = 1;
    if (raw.configUpdatedAt === undefined)    raw.configUpdatedAt = null;
    if (raw.petState.hunger  === undefined)   raw.petState.hunger = 100;
    if (raw.petState.alive   === undefined)   raw.petState.alive  = true;
    if (raw.dashXpTotal      === undefined)   raw.dashXpTotal = 0;

    writeToDb(db, raw);
    fs.renameSync(JSON_FILE, JSON_FILE + '.migrated');
    console.log('[store] Migrated store.json → store.db');
  } catch (e) {
    console.error('[store] JSON migration failed:', e.message);
  }
}

// ── Seed defaults + merge new default todos ───────────────────────────────────

function seedOrMerge(db) {
  const empty =
    db.prepare('SELECT COUNT(*) as n FROM habits').get().n === 0 &&
    db.prepare('SELECT COUNT(*) as n FROM kv').get().n === 0;

  if (empty) {
    writeToDb(db, DEFAULT_DATA);
    return;
  }

  // Inject any DEFAULT_TODOS whose id isn't in the todos table yet
  const existing = new Set(db.prepare('SELECT id FROM todos').all().map(r => r.id));
  const ins = db.prepare('INSERT INTO todos (id, text, done, category) VALUES (?,?,?,?)');
  let maxNew = 0;
  db.transaction(() => {
    for (const t of DEFAULT_TODOS) {
      if (!existing.has(t.id)) {
        ins.run(t.id, t.text, t.done ? 1 : 0, t.category);
        if (t.id > maxNew) maxNew = t.id;
      }
    }
  })();

  if (maxNew > 0) {
    const cur = kvGet(db, 'nextTodoId', 1);
    if (maxNew + 1 > cur) {
      db.prepare('INSERT OR REPLACE INTO kv (key, value) VALUES (?,?)')
        .run('nextTodoId', JSON.stringify(maxNew + 1));
    }
  }
}

// ── History recording ─────────────────────────────────────────────────────────

// Call this inside a store.update() callback to append completion records.
// Keeps the mutation atomic with whatever else the update does (e.g. petState).
//
// date      : server's local "YYYY-MM-DD" string — use new Date().toISOString().slice(0,10).
//             The device clock and server clock may disagree slightly around midnight
//             (device uses NTP when available; server always uses its own clock).
//             The server date is canonical.
// deviceId  : MAC/ID string from the syncing device, or null for dashboard-side completions.
//
// Dedupes per habit per day (truncation-aware name match).
// Prunes the log to the most recent HISTORY_KEEP_DAYS days on each call.
function appendCompletions(d, names, date, deviceId) {
  for (const name of names) {
    const already = d.completionLog.some(
      e => namesMatch(e.habitName, name) && e.date === date
    );
    if (already) continue;
    const habit = d.habits.find(h => namesMatch(h.name, name) && h.active);
    d.completionLog.push({
      id:          d.nextLogId++,
      habitId:     habit?.id      ?? null,
      habitName:   name,
      xpValue:     habit?.xpValue ?? 0,
      date,
      completedAt: new Date().toISOString(),
      deviceId:    deviceId || null
    });
  }
  // Roll: drop entries older than HISTORY_KEEP_DAYS days
  const cutoff = new Date();
  cutoff.setDate(cutoff.getDate() - HISTORY_KEEP_DAYS);
  const cutoffStr = cutoff.toISOString().slice(0, 10);
  d.completionLog = d.completionLog.filter(e => e.date >= cutoffStr);
}

// ── Public API ────────────────────────────────────────────────────────────────

function load() {
  if (cache) return cache;
  const db = getDb();
  migrateFromJson(db);
  seedOrMerge(db);
  cache = readFromDb(db);
  return cache;
}

function save() {
  writeToDb(getDb(), cache);
}

function get()             { return load(); }
function update(mutatorFn) { const d = load(); mutatorFn(d); save(); return d; }

module.exports = { get, update, appendCompletions };
