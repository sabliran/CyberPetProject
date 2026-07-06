const fs = require('fs');
const path = require('path');

const DATA_FILE = path.join(__dirname, 'data', 'store.json');

const DEFAULT_TODOS = [
  // hardware
  { id: 1,  text: 'Wire PCF85063 RTC for accurate midnight habit resets', done: false, category: 'hardware' },
  { id: 2,  text: 'Test firmware build on actual ESP32-S3 device', done: false, category: 'hardware' },
  { id: 3,  text: 'Test touch input with Waveshare CST9217 driver', done: false, category: 'hardware' },
  // firmware
  { id: 4,  text: 'Set WiFi credentials in CyberPet.ino and test sync', done: false, category: 'firmware' },
  { id: 5,  text: 'Verify NVS storage survives deep sleep / power cycle', done: false, category: 'firmware' },
  { id: 6,  text: 'Add on-device habit list editing via touchscreen', done: false, category: 'firmware' },
  { id: 7,  text: 'Add XP notification animation on habit complete', done: false, category: 'firmware' },
  // ui / sim
  { id: 8,  text: 'Add squash-and-stretch on blob landing', done: false, category: 'ui' },
  { id: 9,  text: 'Eyes follow touch / cursor direction', done: false, category: 'ui' },
  { id: 10, text: 'Particle burst on level-up evolution', done: false, category: 'ui' },
  { id: 11, text: 'Test all 4 evolution stages on real AMOLED display', done: false, category: 'ui' },
  // dashboard
  { id: 12, text: 'Add streak chart / completion history', done: false, category: 'dashboard' },
  { id: 13, text: 'Export pet + habit history to JSON backup', done: false, category: 'dashboard' },
  { id: 14, text: 'Add push-style config reload so device picks up changes instantly', done: false, category: 'dashboard' },
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
  completionLog: [],   // { id, habitId, habitName, xpValue, date (YYYY-MM-DD), completedAt }
  nextLogId: 1,
  configVersion: 1,    // bumps whenever settings/habits change; device polls this to detect config drift
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
  // Monotonic lifetime XP counter for dashboard-awarded XP (quests + manual
  // habit completions). Device reads this on every sync and applies only the
  // delta above what it has already received — idempotent, no ack needed.
  dashXpTotal: 0
};

let cache = null;

function ensureFile() {
  const dir = path.dirname(DATA_FILE);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  if (!fs.existsSync(DATA_FILE)) {
    fs.writeFileSync(DATA_FILE, JSON.stringify(DEFAULT_DATA, null, 2));
  }
}

function load() {
  if (cache) return cache;
  ensureFile();
  const raw = fs.readFileSync(DATA_FILE, 'utf-8');
  cache = JSON.parse(raw);
  // Migrate: add missing top-level fields if store predates this version
  if (!cache.quests)        { cache.quests = []; }
  if (!cache.todos)         { cache.todos = DEFAULT_TODOS; cache.nextTodoId = DEFAULT_TODOS.length + 1; }
  if (cache.devNotes === undefined) { cache.devNotes = ''; }
  if (!cache.nextQuestId)   { cache.nextQuestId = 1; }
  if (!cache.completionLog)  { cache.completionLog = []; }
  if (!cache.nextLogId)      { cache.nextLogId = 1; }
  if (!cache.configVersion)  { cache.configVersion = 1; }
  if (cache.configUpdatedAt === undefined) { cache.configUpdatedAt = null; }
  if (cache.petState.hunger  === undefined) { cache.petState.hunger = 100; }
  if (cache.petState.alive   === undefined) { cache.petState.alive  = true; }
  if (cache.dashXpTotal      === undefined) { cache.dashXpTotal = 0; }
  return cache;
}

function save() {
  fs.writeFileSync(DATA_FILE, JSON.stringify(cache, null, 2));
}

function get() { return load(); }

function update(mutatorFn) {
  const data = load();
  mutatorFn(data);
  save();
  return data;
}

module.exports = { get, update };
