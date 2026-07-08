const express = require('express');
const cors = require('cors');
const fs = require('fs');
const path = require('path');
const store = require('./store');

const app = express();
const PORT = process.env.PORT || 8080;

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '..', 'public')));

// ---- SSE clients + config push --------------------------------------------

const sseClients = new Set();

function bumpConfig(d) {
  d.configVersion  = (d.configVersion || 1) + 1;
  d.configUpdatedAt = new Date().toISOString();
}

function broadcastConfigChange(version, updatedAt) {
  const line = `event: config\ndata: ${JSON.stringify({ version, updatedAt })}\n\n`;
  for (const res of sseClients) { try { res.write(line); } catch (_) {} }
}

// ---- shared helpers -------------------------------------------------------

// Replicate firmware level/stage formula so the dashboard can update stage
// instantly when quest XP is awarded (without waiting for a device sync).
function stageFromXP(xp) {
  const STAGE_LEVEL_THRESHOLDS = [0, 2, 5, 8];
  function xpForLevel(l) { return 500 * l * (l + 1) / 2; }
  let lv = 0;
  while (xpForLevel(lv + 1) <= xp) lv++;
  for (let s = STAGE_LEVEL_THRESHOLDS.length - 1; s >= 0; s--) {
    if (lv >= STAGE_LEVEL_THRESHOLDS[s]) return s;
  }
  return 0;
}

// ---------- shared helpers (cont.) ----------------------------------------

// FIX 2: device habit names are truncated to DEVICE_NAME_MAX chars; a plain
// === against a longer dashboard name never matches.  Use namesMatch() for
// any comparison involving a name that may have originated on the device.
const DEVICE_NAME_MAX = 23;
function namesMatch(a, b) {
  if (a === b) return true;
  return (a.length >= DEVICE_NAME_MAX || b.length >= DEVICE_NAME_MAX)
      && a.slice(0, DEVICE_NAME_MAX) === b.slice(0, DEVICE_NAME_MAX);
}

// ---------- Habits ---------------------------------------------------------

app.get('/api/habits', (req, res) => {
  res.json(store.get().habits.filter(h => h.active));
});

app.post('/api/habits', (req, res) => {
  const { name, xpValue } = req.body;
  if (!name || typeof xpValue !== 'number')
    return res.status(400).json({ error: 'name (string) and xpValue (number) required' });
  const data = store.update(d => {
    d.habits.push({ id: d.nextHabitId, name, xpValue, active: true });
    d.nextHabitId += 1;
    bumpConfig(d);
  });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.status(201).json(data.habits[data.habits.length - 1]);
});

app.delete('/api/habits/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  const data = store.update(d => {
    const h = d.habits.find(x => x.id === id);
    if (h) { h.active = false; bumpConfig(d); }
  });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.status(204).end();
});

app.post('/api/habits/:id/complete', (req, res) => {
  const id    = parseInt(req.params.id, 10);
  const today = new Date().toISOString().slice(0, 10);
  let recorded = false;
  const data = store.update(d => {
    const habit = d.habits.find(h => h.id === id && h.active);
    if (!habit) return;
    const already = d.completionLog.some(e => e.habitId === id && e.date === today);
    if (!already) {
      d.completionLog.push({
        id: d.nextLogId++, habitId: id, habitName: habit.name,
        xpValue: habit.xpValue, date: today, completedAt: new Date().toISOString()
      });
      d.petState.xp    += habit.xpValue;
      d.petState.stage  = stageFromXP(d.petState.xp);
      d.petState.mood   = Math.min(100, d.petState.mood + 5);
      // FIX 1: accumulate so device can delta-apply this XP on next sync
      d.dashXpTotal = (d.dashXpTotal || 0) + habit.xpValue;
      bumpConfig(d);
      recorded = true;
    }
  });
  const habit = data.habits.find(h => h.id === id);
  if (!habit) return res.status(404).json({ error: 'not found' });
  if (recorded) broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.json({ recorded, petState: data.petState });
});

app.patch('/api/habits/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  const { name, xpValue } = req.body;
  const data = store.update(d => {
    const h = d.habits.find(x => x.id === id);
    if (h) {
      if (name !== undefined) h.name = name;
      if (xpValue !== undefined) h.xpValue = xpValue;
      bumpConfig(d);
    }
  });
  const updated = data.habits.find(x => x.id === id);
  if (!updated) return res.status(404).json({ error: 'not found' });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.json(updated);
});

// ---------- Goals ----------------------------------------------------------

app.get('/api/goals', (req, res) => {
  res.json(store.get().goals.filter(g => g.active));
});

app.post('/api/goals', (req, res) => {
  const { name, xpValue, period } = req.body;
  if (!name || typeof xpValue !== 'number' || !['weekly', 'monthly'].includes(period))
    return res.status(400).json({ error: 'name, xpValue, and period ("weekly"|"monthly") required' });
  const data = store.update(d => {
    d.goals.push({ id: d.nextGoalId, name, xpValue, period, active: true });
    d.nextGoalId += 1;
  });
  res.status(201).json(data.goals[data.goals.length - 1]);
});

app.delete('/api/goals/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  store.update(d => { const g = d.goals.find(x => x.id === id); if (g) g.active = false; });
  res.status(204).end();
});

// ---------- Quests ---------------------------------------------------------
// One-time objectives. Marking done awards XP to petState immediately.

app.get('/api/quests', (req, res) => {
  res.json(store.get().quests.filter(q => q.active));
});

app.post('/api/quests', (req, res) => {
  const { name, xpValue, description } = req.body;
  if (!name || typeof xpValue !== 'number')
    return res.status(400).json({ error: 'name (string) and xpValue (number) required' });
  const data = store.update(d => {
    d.quests.push({
      id: d.nextQuestId,
      name,
      xpValue,
      description: description || '',
      done: false,
      doneAt: null,
      active: true
    });
    d.nextQuestId += 1;
  });
  res.status(201).json(data.quests[data.quests.length - 1]);
});

app.delete('/api/quests/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  store.update(d => { const q = d.quests.find(x => x.id === id); if (q) q.active = false; });
  res.status(204).end();
});

app.patch('/api/quests/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  const { done } = req.body;
  let updated;
  const data = store.update(d => {
    const q = d.quests.find(x => x.id === id);
    if (!q) return;
    const wasDone = q.done;
    q.done   = !!done;
    q.doneAt = q.done ? (q.doneAt || new Date().toISOString()) : null;
    // Award XP when transitioning undone → done.
    // Un-checking does NOT reverse dashXpTotal — the counter is monotonic by design.
    if (!wasDone && q.done) {
      d.petState.xp   += q.xpValue;
      d.petState.stage = stageFromXP(d.petState.xp);
      d.petState.mood  = Math.min(100, d.petState.mood + 5);
      // FIX 1: accumulate so device can delta-apply this XP on next sync
      d.dashXpTotal = (d.dashXpTotal || 0) + q.xpValue;
      bumpConfig(d);
    }
    updated = q;
  });
  if (!updated) return res.status(404).json({ error: 'not found' });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.json({ quest: updated, petState: data.petState });
});

// ---------- Developer todos ------------------------------------------------

app.get('/api/todos', (req, res) => {
  res.json(store.get().todos);
});

app.post('/api/todos', (req, res) => {
  const { text, category } = req.body;
  if (!text) return res.status(400).json({ error: 'text required' });
  const data = store.update(d => {
    d.todos.push({ id: d.nextTodoId, text, category: category || 'general', done: false });
    d.nextTodoId += 1;
  });
  res.status(201).json(data.todos[data.todos.length - 1]);
});

app.patch('/api/todos/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  const { done, text, category } = req.body;
  const data = store.update(d => {
    const t = d.todos.find(x => x.id === id);
    if (!t) return;
    if (done    !== undefined) t.done     = !!done;
    if (text    !== undefined) t.text     = text;
    if (category !== undefined) t.category = category;
  });
  const updated = data.todos.find(x => x.id === id);
  if (!updated) return res.status(404).json({ error: 'not found' });
  res.json(updated);
});

app.delete('/api/todos/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  store.update(d => { d.todos = d.todos.filter(x => x.id !== id); });
  res.status(204).end();
});

// ---------- Dev notes ------------------------------------------------------

app.get('/api/devnotes', (req, res) => {
  res.json({ text: store.get().devNotes || '' });
});

app.post('/api/devnotes', (req, res) => {
  const { text } = req.body;
  if (typeof text !== 'string') return res.status(400).json({ error: 'text required' });
  const data = store.update(d => { d.devNotes = text; });
  res.json({ text: data.devNotes });
});

// ---------- Completion history ---------------------------------------------

app.get('/api/history', (req, res) => {
  const days = Math.min(parseInt(req.query.days || '112', 10), 365);
  const data = store.get();
  const habitCount = data.habits.filter(h => h.active).length;

  // Build date range
  const today = new Date(); today.setHours(0, 0, 0, 0);
  const dates = [];
  for (let i = days - 1; i >= 0; i--) {
    const d = new Date(today);
    d.setDate(d.getDate() - i);
    dates.push(d.toISOString().slice(0, 10));
  }

  // Group log by date
  const byDate = {};
  for (const e of data.completionLog) {
    if (!byDate[e.date]) byDate[e.date] = [];
    byDate[e.date].push(e);
  }

  const heatmap = dates.map(date => ({
    date,
    count: byDate[date]?.length ?? 0,
    total: habitCount,
    pct:   habitCount > 0 ? Math.round((byDate[date]?.length ?? 0) / habitCount * 100) : 0
  }));

  // Recent log (last 30 entries, newest first)
  const recent = [...data.completionLog]
    .sort((a, b) => b.completedAt.localeCompare(a.completedAt))
    .slice(0, 30);

  res.json({ heatmap, recent });
});

app.get('/api/history/streaks', (req, res) => {
  const data  = store.get();
  const today = new Date().toISOString().slice(0, 10);

  function streakFor(habitId, habitName) {
    const dates = new Set(
      data.completionLog
        .filter(e => e.habitId === habitId || namesMatch(e.habitName, habitName))
        .map(e => e.date)
    );

    // Current streak (consecutive days ending today or yesterday)
    let streak = 0;
    const d = new Date(today);
    if (!dates.has(today)) d.setDate(d.getDate() - 1); // start from yesterday if not done today
    while (dates.has(d.toISOString().slice(0, 10))) {
      streak++;
      d.setDate(d.getDate() - 1);
    }

    // Best streak
    const sorted = [...dates].sort();
    let best = sorted.length ? 1 : 0, cur = 1;
    for (let i = 1; i < sorted.length; i++) {
      const prev = new Date(sorted[i - 1]);
      prev.setDate(prev.getDate() + 1);
      cur = prev.toISOString().slice(0, 10) === sorted[i] ? cur + 1 : 1;
      if (cur > best) best = cur;
    }

    // Last 14 days boolean array (oldest → newest)
    const recent = [];
    for (let i = 13; i >= 0; i--) {
      const d2 = new Date(today);
      d2.setDate(d2.getDate() - i);
      recent.push(dates.has(d2.toISOString().slice(0, 10)));
    }

    return { streak, best, recent };
  }

  const result = data.habits
    .filter(h => h.active)
    .map(h => ({ id: h.id, name: h.name, xpValue: h.xpValue, ...streakFor(h.id, h.name) }));

  res.json(result);
});

// ---------- Settings -------------------------------------------------------

app.get('/api/settings', (req, res) => {
  res.json(store.get().settings);
});

app.post('/api/settings', (req, res) => {
  const ALLOWED = ['petName', 'dailyResetHour', 'moodDecayPerMiss',
                   'moodGainPerHabit', 'syncIntervalSeconds'];
  const clean = {};
  for (const key of ALLOWED) {
    const v = req.body[key];
    if (v === null || v === undefined || v === '') continue;
    if (typeof v === 'number' && !Number.isFinite(v)) continue;
    clean[key] = v;
  }
  const data = store.update(d => { Object.assign(d.settings, clean); bumpConfig(d); });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.json(data.settings);
});

// ---------- Pet state ------------------------------------------------------

app.get('/api/pet', (req, res) => {
  res.json(store.get().petState);
});

// ---------- Device sync ----------------------------------------------------

app.post('/api/sync', (req, res) => {
  const { deviceId, petState, completedHabits } = req.body;
  // Use the server's local date as the canonical completion date.
  // Device and server clocks may disagree slightly around midnight — the server
  // date is always preferred so history records are consistent.
  const todayDate = new Date().toISOString().slice(0, 10);
  const data = store.update(d => {
    if (deviceId) d.settings.deviceId = deviceId;
    if (petState) {
      d.petState = { ...d.petState, ...petState, lastSyncedAt: new Date().toISOString() };
    }
    if (Array.isArray(completedHabits) && completedHabits.length > 0) {
      // Accept legacy string-name arrays (old firmware) or {id, name} object
      // arrays (new firmware).  When an id is known, use the server's current
      // habit name so completion log entries stay accurate through renames.
      const names = completedHabits.map(entry => {
        if (typeof entry === 'string') return entry;
        if (typeof entry === 'object' && entry !== null) {
          const sid = typeof entry.id === 'number' ? entry.id : -1;
          if (sid >= 0) {
            const h = d.habits.find(h => h.id === sid && h.active);
            if (h) return h.name; // use current server name (rename-aware)
          }
          return typeof entry.name === 'string' ? entry.name : null;
        }
        return null;
      }).filter(Boolean);
      d.lastCompletedHabits = names;
      // History recording lives in store.js — called here inside update() so
      // the pet-state write and log append are a single atomic DB transaction.
      store.appendCompletions(d, names, todayDate, deviceId || null);
    }
  });
  res.json({
    habits:        data.habits.filter(h => h.active),
    goals:         data.goals.filter(g => g.active),
    quests:        data.quests.filter(q => q.active && !q.done),
    settings:      data.settings,
    // FIX 1: device uses these to delta-apply dashboard XP and stay in config sync
    dashXpTotal:   data.dashXpTotal   || 0,
    configVersion: data.configVersion || 1
  });
});

// ---------- Config version (lightweight polling for device) ----------------

app.get('/api/config-version', (req, res) => {
  const d = store.get();
  res.json({ version: d.configVersion || 1, updatedAt: d.configUpdatedAt || null });
});

// ---------- SSE event stream (dashboard live push indicator) ---------------

app.get('/api/events', (req, res) => {
  res.setHeader('Content-Type',  'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection',    'keep-alive');
  res.flushHeaders();

  sseClients.add(res);
  req.on('close', () => sseClients.delete(res));

  const d = store.get();
  res.write(`event: connected\ndata: ${JSON.stringify({ version: d.configVersion || 1 })}\n\n`);

  // Keep-alive ping every 25 s so proxies/browsers don't time out
  const ping = setInterval(() => { try { res.write(': ping\n\n'); } catch (_) {} }, 25000);
  req.on('close', () => clearInterval(ping));
});

// ---------- Export / backup ------------------------------------------------

app.get('/api/export', (req, res) => {
  const data  = store.get();
  const today = new Date().toISOString().slice(0, 10);
  const payload = {
    exportedAt:    new Date().toISOString(),
    petState:      data.petState,
    settings:      data.settings,
    habits:        data.habits.filter(h => h.active),
    goals:         data.goals.filter(g => g.active),
    quests:        data.quests.filter(q => q.active),
    completionLog: data.completionLog
  };
  res.setHeader('Content-Disposition', `attachment; filename="cyberpet-backup-${today}.json"`);
  res.setHeader('Content-Type', 'application/json');
  res.json(payload);
});

// ---------- Storage info ---------------------------------------------------

const DATA_FILE_PATH = path.join(__dirname, 'data', 'store.db');
const DASHBOARD_QUOTA = 5 * 1024 * 1024; // 5 MB soft cap for display purposes

// ESP32 NVS partition — typical CyberPet build uses a 20 KB NVS partition
const NVS_SIZE = 20 * 1024;

app.get('/api/storage', (req, res) => {
  let storeSize = 0;
  try { storeSize = fs.statSync(DATA_FILE_PATH).size; } catch (_) {}

  const data = store.get();

  // Rough NVS estimate: fixed pet state fields + per-habit entry
  const nvsFixed    = 128;                       // xp, mood, stage, daysAlive, settings
  const nvsPerHabit = 32;                        // id + name + xpValue per habit
  const nvsEstimate = nvsFixed + data.habits.filter(h => h.active).length * nvsPerHabit;

  const logEntries  = data.completionLog.length;
  const habitCount  = data.habits.filter(h => h.active).length;
  const questCount  = data.quests.filter(q => q.active).length;
  const todoCount   = data.todos.length;

  res.json({
    dashboard: { used: storeSize, quota: DASHBOARD_QUOTA },
    nvs:       { used: nvsEstimate, total: NVS_SIZE },
    counts:    { habits: habitCount, quests: questCount, todos: todoCount, logEntries }
  });
});

app.listen(PORT, () => {
  console.log(`CyberPet dashboard running on port ${PORT}`);
});
