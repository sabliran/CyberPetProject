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
  const STAGE_LEVEL_THRESHOLDS = [0, 2, 6, 12];
  function xpForLevel(l) { return 25 * l * l + 75 * l; }
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

// Goals are periodic: a done goal becomes available again once its period
// elapses (measured from doneAt). Call inside a store.update() before
// reading/serving goals so expiry is applied lazily.
const GOAL_PERIOD_MS = { weekly: 7 * 864e5, monthly: 30 * 864e5 };
function resetExpiredGoals(d) {
  const now = Date.now();
  for (const g of d.goals) {
    if (g.done && g.doneAt) {
      const ms = GOAL_PERIOD_MS[g.period] || GOAL_PERIOD_MS.weekly;
      if (now - new Date(g.doneAt).getTime() >= ms) {
        g.done = false;
        g.doneAt = null;
      }
    }
  }
}

app.get('/api/goals', (req, res) => {
  const data = store.update(d => resetExpiredGoals(d));
  res.json(data.goals.filter(g => g.active));
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

app.patch('/api/goals/:id', (req, res) => {
  const id = parseInt(req.params.id, 10);
  const { done } = req.body;
  let updated;
  const data = store.update(d => {
    resetExpiredGoals(d);
    const g = d.goals.find(x => x.id === id);
    if (!g) return;
    const wasDone = g.done;
    g.done   = !!done;
    g.doneAt = g.done ? (g.doneAt || new Date().toISOString()) : null;
    // Award XP when transitioning undone → done (same rules as quests).
    // Un-checking does NOT reverse dashXpTotal — the counter is monotonic by design.
    if (!wasDone && g.done) {
      d.petState.xp   += g.xpValue;
      d.petState.stage = stageFromXP(d.petState.xp);
      d.petState.mood  = Math.min(100, d.petState.mood + 5);
      d.dashXpTotal = (d.dashXpTotal || 0) + g.xpValue;
      bumpConfig(d);
    }
    updated = g;
  });
  if (!updated) return res.status(404).json({ error: 'not found' });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.json({ goal: updated, petState: data.petState });
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
                   'moodGainPerHabit', 'syncIntervalSeconds', 'difficulty'];
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

// ---------- Reminders ------------------------------------------------------
// Daily speech bubbles Koko holds up on the watch. Dashboard-owned: the
// device receives the list in every sync response (quests pattern) and
// caches it in NVS. Max 6; each has a start time, a window length in
// minutes (how long the bubble stays available before giving up — may
// cross midnight), a message (device truncates at 39 chars), and an
// enable flag.

const DEFAULT_REMINDERS = [
  { enabled: true, hour: 14, minute: 0,  durationMin: 600, message: 'brush your teeth!' },
  { enabled: true, hour: 0,  minute: 30, durationMin: 210, message: 'brush your teeth!' },
  { enabled: true, hour: 9,  minute: 0,  durationMin: 180, message: 'time to study!' },
  { enabled: true, hour: 15, minute: 30, durationMin: 150, message: 'read something!' },
];

function cleanReminders(list) {
  if (!Array.isArray(list)) return null;
  const clampNum = (v, lo, hi, dflt) => {
    v = Number(v);
    return Number.isFinite(v) ? Math.min(hi, Math.max(lo, Math.round(v))) : dflt;
  };
  const out = [];
  for (const r of list.slice(0, 6)) {
    const message = String(r.message || '').trim().slice(0, 39);
    if (!message) continue;
    out.push({
      enabled:     !!r.enabled,
      hour:        clampNum(r.hour, 0, 23, 0),
      minute:      clampNum(r.minute, 0, 59, 0),
      durationMin: clampNum(r.durationMin, 1, 1439, 120),
      message,
    });
  }
  return out;
}

app.get('/api/reminders', (req, res) => {
  res.json(store.get().reminders || DEFAULT_REMINDERS);
});

app.put('/api/reminders', (req, res) => {
  const clean = cleanReminders(req.body);
  if (!clean) return res.status(400).json({ error: 'expected an array of reminders' });
  const data = store.update(d => { d.reminders = clean; bumpConfig(d); });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.json(data.reminders);
});

// ---------- Pet state ------------------------------------------------------

app.get('/api/pet', (req, res) => {
  res.json(store.get().petState);
});

// Reset XP/level/stage everywhere. The device can't be written directly, so
// this bumps a monotonic petResetToken carried in every sync response; the
// firmware applies it exactly once (persisted last-applied token in NVS).
app.post('/api/pet/reset', (req, res) => {
  const data = store.update(d => {
    d.petResetToken  = (d.petResetToken || 0) + 1;
    d.petState.xp    = 0;
    d.petState.stage = 0;
    d.dashXpTotal    = 0;  // device's dashXpApplied is zeroed by resetProgress()
    bumpConfig(d);
  });
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  res.json({ petState: data.petState, petResetToken: data.petResetToken });
});

// ---------- Device sync ----------------------------------------------------

// Walk app: mirrors WALK_DAILY_GOAL / WALK_STRIDE_CM in the firmware's ui.h.
const WALK_DAILY_GOAL = 6000;
const WALK_STRIDE_CM  = 70;
const STEP_HISTORY_DAYS = 90;

app.post('/api/sync', (req, res) => {
  const { deviceId, petState, completedHabits, steps, sleep, backSessions, pushSessions, pullupSessions, focusSessions, plank, storage: devStorage } = req.body;
  // Use the server's local date as the canonical completion date.
  // Device and server clocks may disagree slightly around midnight — the server
  // date is always preferred so history records are consistent.
  const todayDate = new Date().toISOString().slice(0, 10);
  const data = store.update(d => {
    resetExpiredGoals(d);  // lazy period rollover so the device gets fresh goals
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
    // Walking analytics: per-day step history. Keyed by the device's own
    // calendar date when its clock is valid (the device rolls its counter at
    // its own midnight); server date otherwise. Steps only grow within a
    // day, so max() absorbs reboots where the device restores a slightly
    // stale NVS count.
    if (steps && typeof steps.today === 'number' && steps.today >= 0) {
      let key = todayDate;
      if (steps.year >= 2020 && steps.dayOfYear >= 1 && steps.dayOfYear <= 366) {
        key = new Date(Date.UTC(steps.year, 0, steps.dayOfYear)).toISOString().slice(0, 10);
      }
      d.stepHistory = d.stepHistory || {};
      if (steps.today > (d.stepHistory[key] || 0)) d.stepHistory[key] = steps.today;
      const cutoff = new Date(Date.now() - STEP_HISTORY_DAYS * 864e5).toISOString().slice(0, 10);
      for (const k of Object.keys(d.stepHistory)) if (k < cutoff) delete d.stepHistory[k];
    }
    // Sleep history: quality (0 good / 1 medium / 2 bad) keyed by the date
    // the device logged it. No server-date fallback — a rating stamped with
    // an unset clock is dropped rather than recorded on the wrong day.
    if (sleep && typeof sleep.quality === 'number' && sleep.quality >= 0 && sleep.quality <= 2 &&
        sleep.year >= 2020 && sleep.dayOfYear >= 1 && sleep.dayOfYear <= 366) {
      const key = new Date(Date.UTC(sleep.year, 0, sleep.dayOfYear)).toISOString().slice(0, 10);
      d.sleepHistory = d.sleepHistory || {};
      d.sleepHistory[key] = sleep.quality;
      const cutoff = new Date(Date.now() - STEP_HISTORY_DAYS * 864e5).toISOString().slice(0, 10);
      for (const k of Object.keys(d.sleepHistory)) if (k < cutoff) delete d.sleepHistory[k];
    }
    // Back-workout lifetime sessions: monotonic max (a device NVS wipe can
    // never claw back earned trophies). The increment since last sync is
    // credited to the device's calendar date (fall back to server date) so
    // the dashboard can show per-day workout history.
    if (typeof backSessions === 'number' && backSessions > (d.backSessions || 0)) {
      const delta = backSessions - (d.backSessions || 0);
      d.backSessions = backSessions;
      let key = todayDate;
      if (steps && steps.year >= 2020 && steps.dayOfYear >= 1 && steps.dayOfYear <= 366) {
        key = new Date(Date.UTC(steps.year, 0, steps.dayOfYear)).toISOString().slice(0, 10);
      }
      d.backHistory = d.backHistory || {};
      d.backHistory[key] = (d.backHistory[key] || 0) + delta;
      const cutoff = new Date(Date.now() - STEP_HISTORY_DAYS * 864e5).toISOString().slice(0, 10);
      for (const k of Object.keys(d.backHistory)) if (k < cutoff) delete d.backHistory[k];
    }
    // Push-up sessions: identical bookkeeping.
    if (typeof pushSessions === 'number' && pushSessions > (d.pushSessions || 0)) {
      const delta = pushSessions - (d.pushSessions || 0);
      d.pushSessions = pushSessions;
      let key = todayDate;
      if (steps && steps.year >= 2020 && steps.dayOfYear >= 1 && steps.dayOfYear <= 366) {
        key = new Date(Date.UTC(steps.year, 0, steps.dayOfYear)).toISOString().slice(0, 10);
      }
      d.pushHistory = d.pushHistory || {};
      d.pushHistory[key] = (d.pushHistory[key] || 0) + delta;
      const cutoff = new Date(Date.now() - STEP_HISTORY_DAYS * 864e5).toISOString().slice(0, 10);
      for (const k of Object.keys(d.pushHistory)) if (k < cutoff) delete d.pushHistory[k];
    }
    // Pull-up sessions: identical bookkeeping.
    if (typeof pullupSessions === 'number' && pullupSessions > (d.pullupSessions || 0)) {
      const delta = pullupSessions - (d.pullupSessions || 0);
      d.pullupSessions = pullupSessions;
      let key = todayDate;
      if (steps && steps.year >= 2020 && steps.dayOfYear >= 1 && steps.dayOfYear <= 366) {
        key = new Date(Date.UTC(steps.year, 0, steps.dayOfYear)).toISOString().slice(0, 10);
      }
      d.pullupHistory = d.pullupHistory || {};
      d.pullupHistory[key] = (d.pullupHistory[key] || 0) + delta;
      const cutoff = new Date(Date.now() - STEP_HISTORY_DAYS * 864e5).toISOString().slice(0, 10);
      for (const k of Object.keys(d.pullupHistory)) if (k < cutoff) delete d.pullupHistory[k];
    }
    // Focus blocks (25-min pomodoros): identical bookkeeping.
    if (typeof focusSessions === 'number' && focusSessions > (d.focusSessions || 0)) {
      const delta = focusSessions - (d.focusSessions || 0);
      d.focusSessions = focusSessions;
      let key = todayDate;
      if (steps && steps.year >= 2020 && steps.dayOfYear >= 1 && steps.dayOfYear <= 366) {
        key = new Date(Date.UTC(steps.year, 0, steps.dayOfYear)).toISOString().slice(0, 10);
      }
      d.focusHistory = d.focusHistory || {};
      d.focusHistory[key] = (d.focusHistory[key] || 0) + delta;
      const cutoff = new Date(Date.now() - STEP_HISTORY_DAYS * 864e5).toISOString().slice(0, 10);
      for (const k of Object.keys(d.focusHistory)) if (k < cutoff) delete d.focusHistory[k];
    }
    // Plank: the device reports lifetime seconds held; the growth since the
    // last sync is credited to today (device date preferred, same as steps).
    // Session count and best hold are monotonic maxes like the counters above.
    if (plank && typeof plank.totalSec === 'number' && plank.totalSec > (d.plankTotalSec || 0)) {
      const delta = plank.totalSec - (d.plankTotalSec || 0);
      d.plankTotalSec = plank.totalSec;
      let key = todayDate;
      if (steps && steps.year >= 2020 && steps.dayOfYear >= 1 && steps.dayOfYear <= 366) {
        key = new Date(Date.UTC(steps.year, 0, steps.dayOfYear)).toISOString().slice(0, 10);
      }
      d.plankHistory = d.plankHistory || {};
      d.plankHistory[key] = (d.plankHistory[key] || 0) + delta;
      const cutoff = new Date(Date.now() - STEP_HISTORY_DAYS * 864e5).toISOString().slice(0, 10);
      for (const k of Object.keys(d.plankHistory)) if (k < cutoff) delete d.plankHistory[k];
    }
    if (plank && typeof plank.sessions === 'number' && plank.sessions > (d.plankSessions || 0)) {
      d.plankSessions = plank.sessions;
    }
    if (plank && typeof plank.bestMs === 'number' && plank.bestMs > (d.plankBestMs || 0)) {
      d.plankBestMs = plank.bestMs;
    }
    // Real device storage numbers (sketch/app partition + NVS) — replaces
    // the /api/storage estimates once the device has reported.
    if (devStorage && typeof devStorage.appTotal === 'number' && devStorage.appTotal > 0) {
      d.deviceStorage = {
        sketch:     devStorage.sketch   | 0,
        appTotal:   devStorage.appTotal | 0,
        nvsUsed:    devStorage.nvsUsed  | 0,
        nvsTotal:   devStorage.nvsTotal | 0,
        reportedAt: new Date().toISOString(),
      };
    }
  });
  res.json({
    habits:        data.habits.filter(h => h.active),
    goals:         data.goals.filter(g => g.active && !g.done),
    quests:        data.quests.filter(q => q.active && !q.done),
    settings:      data.settings,
    // FIX 1: device uses these to delta-apply dashboard XP and stay in config sync
    dashXpTotal:   data.dashXpTotal   || 0,
    petResetToken: data.petResetToken || 0,
    // Bumped by POST /api/dict/publish; the device downloads the dictionary
    // files over WiFi when this advances past its NVS-stored last-applied.
    dictPushToken: data.dictPushToken || 0,
    configVersion: data.configVersion || 1,
    // Daily speech-bubble reminders (dashboard-owned; device caches in NVS).
    reminders:     data.reminders || DEFAULT_REMINDERS,
    // Earned trophy names for the device's trophy screen (read-only there).
    trophies:      computeTrophies(data).filter(t => t.earned).map(t => t.name)
  });
});

// ---------- Trophies ---------------------------------------------------------
// Computed on the fly from history — deterministic, so nothing to persist.
// Device gets earned names in the sync response (quests/goals pattern);
// the dashboard panel shows the full list with locked/unlocked state.

function computeTrophies(d) {
  const stepVals  = Object.values(d.stepHistory || {});
  const sleepVals = Object.values(d.sleepHistory || {});
  const log       = d.completionLog || [];

  // Best habit streak ever: longest consecutive-day chain per habit.
  const byHabit = new Map();
  for (const e of log) {
    const key = (typeof e.habitId === 'number' && e.habitId >= 0) ? `#${e.habitId}` : `n:${e.habitName}`;
    if (!byHabit.has(key)) byHabit.set(key, new Set());
    byHabit.get(key).add(e.date);
  }
  let bestStreak = 0;
  for (const dates of byHabit.values()) {
    const sorted = [...dates].sort();
    let cur = sorted.length ? 1 : 0;
    if (cur > bestStreak) bestStreak = cur;
    for (let i = 1; i < sorted.length; i++) {
      const prev = new Date(sorted[i - 1]);
      prev.setDate(prev.getDate() + 1);
      cur = prev.toISOString().slice(0, 10) === sorted[i] ? cur + 1 : 1;
      if (cur > bestStreak) bestStreak = cur;
    }
  }

  // Level from the firmware's curve: xpForLevel(L) = 25L² + 75L.
  const xp = (d.petState && d.petState.xp) || 0;
  let level = 0;
  while (25 * (level + 1) ** 2 + 75 * (level + 1) <= xp) level++;
  const stage = (d.petState && d.petState.stage) || 0;

  // Most completions on a single day (for the busy-bee trophy).
  const perDay = new Map();
  for (const e of log) perDay.set(e.date, (perDay.get(e.date) || 0) + 1);
  const maxPerDay = Math.max(0, ...perDay.values());

  const goalWalks  = stepVals.filter(s => s >= WALK_DAILY_GOAL).length;
  const totalKm    = stepVals.reduce((a, s) => a + s, 0) * WALK_STRIDE_CM / 100000;
  const goodNights = sleepVals.filter(q => q === 0).length;

  // Longest run of consecutive calendar dates with a "good" rating.
  const goodDates = Object.entries(d.sleepHistory || {})
    .filter(([, q]) => q === 0).map(([k]) => k).sort();
  let goodRun = goodDates.length ? 1 : 0, run = 1;
  for (let i = 1; i < goodDates.length; i++) {
    const prev = new Date(goodDates[i - 1]);
    prev.setDate(prev.getDate() + 1);
    run = prev.toISOString().slice(0, 10) === goodDates[i] ? run + 1 : 1;
    if (run > goodRun) goodRun = run;
  }

  const questDone = (d.quests || []).some(q => q.done);
  const daysAlive = (d.petState && d.petState.daysAlive) || 0;
  const maxSteps  = stepVals.length ? Math.max(...stepVals) : 0;
  const kmTotal   = Math.floor(totalKm);

  // Strength: completion-days of habits whose name mentions the exercise
  // (matches "10 pullups", "push ups", "Pull-Ups", ...). Distinct dates so
  // renames/duplicates can't double-count a day.
  const daysMatching = (re) => new Set(
    log.filter(e => re.test(e.habitName || '')).map(e => e.date)
  ).size;
  // Pull-up days: union of habit-log matches and the pull-up app's tracked
  // sessions (per-date history), so days earned either way both count and
  // pre-app trophies survive.
  const pullDaySet = new Set(
    log.filter(e => /pull[\s-]?ups?/i.test(e.habitName || '')).map(e => e.date)
  );
  for (const k of Object.keys(d.pullupHistory || {})) pullDaySet.add(k);
  const pullDays = pullDaySet.size;
  const pushDays = daysMatching(/push[\s-]?ups?/i);

  // Each entry carries cur/target so the dashboard panels can show
  // "next trophy: X (cur/target)" without re-deriving any metric.
  const T = (id, cat, name, desc, cur, target) =>
    ({ id, cat, name, desc, cur, target, earned: cur >= target });
  const backS  = d.backSessions  || 0;
  const pushS  = d.pushSessions  || 0;
  const focusS = d.focusSessions || 0;

  return [
    // habits
    T('first-habit', 'habits', 'First step',      'complete your first habit', log.length, 1),
    T('habits-10',   'habits', 'Regular',         '10 habit completions',      log.length, 10),
    T('habits-100',  'habits', 'Centurion',       '100 habit completions',     log.length, 100),
    T('habits-500',  'habits', 'Legend',          '500 habit completions',     log.length, 500),
    T('streak-7',    'habits', 'Week warrior',    'a 7-day habit streak',      bestStreak, 7),
    T('streak-30',   'habits', 'Iron month',      'a 30-day habit streak',     bestStreak, 30),
    T('streak-100',  'habits', 'Unstoppable',     'a 100-day habit streak',    bestStreak, 100),
    T('busy-bee',    'habits', 'Busy bee',        '5 habits done in one day',  maxPerDay, 5),
    // strength
    T('back-1',      'strength', 'First row',     'first back workout',        backS, 1),
    T('back-10',     'strength', 'Row machine',   '10 back workouts',          backS, 10),
    T('back-30',     'strength', 'Iron back',     '30 back workouts',          backS, 30),
    T('boop-1',      'strength', 'First boop',    'first push-up session',     pushS, 1),
    T('boop-10',     'strength', 'Boop machine',  '10 push-up sessions',       pushS, 10),
    T('boop-30',     'strength', 'Iron chest',    '30 push-up sessions',       pushS, 30),
    T('pull-1',      'strength', 'First hang',    'first pull-up day',         pullDays, 1),
    T('pull-10',     'strength', 'Bar starter',   '10 pull-up days',           pullDays, 10),
    T('pull-30',     'strength', 'Bar veteran',   '30 pull-up days',           pullDays, 30),
    T('push-1',      'strength', 'First rep',     'first push-up day',         pushDays, 1),
    T('push-10',     'strength', 'Floor regular', '10 push-up days',           pushDays, 10),
    T('push-30',     'strength', 'Floor general', '30 push-up days',           pushDays, 30),
    // walking
    T('steps-2k',    'walking', 'Warm-up',        '2,000 steps in a day',      maxSteps, 2000),
    T('steps-10k',   'walking', 'Long hauler',    '10,000 steps in one day',   maxSteps, 10000),
    T('steps-20k',   'walking', 'Ultra',          '20,000 steps in one day',   maxSteps, 20000),
    T('walk-5',      'walking', 'Regular walker', '5 days at the step goal',   goalWalks, 5),
    T('walk-25',     'walking', 'Trail veteran',  '25 days at the step goal',  goalWalks, 25),
    T('km-100',      'walking', 'Globetrotter',   '100 km walked in total',    kmTotal, 100),
    // focus
    T('focus-1',     'focus', 'First block',      'complete a focus block',    focusS, 1),
    T('focus-10',    'focus', 'Deep worker',      '10 focus blocks',           focusS, 10),
    T('focus-50',    'focus', 'Flow state',       '50 focus blocks',           focusS, 50),
    // sleep
    T('sleep-7',     'sleep', 'Well rested',      '7 nights of good sleep',    goodNights, 7),
    T('dream-week',  'sleep', 'Dream week',       '7 good nights in a row',    goodRun, 7),
    T('sleep-30',    'sleep', 'Sleep scientist',  '30 nights logged',          sleepVals.length, 30),
    T('bad-night',   'sleep', 'It happens',       'honestly log a bad night',  sleepVals.some(q => q === 2) ? 1 : 0, 1),
    // pet
    T('hatched',     'pet', 'Hatched',            'reach the blob stage',      stage, 1),
    T('level-5',     'pet', 'Level 5',            'reach level 5',             level, 5),
    T('level-10',    'pet', 'Level 10',           'reach level 10',            level, 10),
    T('level-20',    'pet', 'Overachiever',       'reach level 20',            level, 20),
    T('evolved',     'pet', 'Final form',         'reach the evolved stage',   stage, 3),
    T('quest-1',     'pet', 'Adventurer',         'complete your first quest', questDone ? 1 : 0, 1),
    T('days-30',     'pet', 'Survivor',           '30 days together',          daysAlive, 30),
  ];
}

app.get('/api/trophies', (req, res) => {
  res.json(computeTrophies(store.get()));
});

// Back-workout history for the dashboard panel.
app.get('/api/backworkouts', (req, res) => {
  const d = store.get();
  res.json({ total: d.backSessions || 0, history: d.backHistory || {} });
});

// Push-up history for the dashboard panel.
app.get('/api/pushups', (req, res) => {
  const d = store.get();
  res.json({ total: d.pushSessions || 0, history: d.pushHistory || {} });
});

// Pull-up history for the dashboard panel.
app.get('/api/pullups', (req, res) => {
  const d = store.get();
  res.json({ total: d.pullupSessions || 0, history: d.pullupHistory || {} });
});

// Plank history + records for the dashboard panel.
app.get('/api/plank', (req, res) => {
  const d = store.get();
  res.json({ totalSec: d.plankTotalSec || 0, sessions: d.plankSessions || 0,
             bestMs: d.plankBestMs || 0, history: d.plankHistory || {} });
});

// Motion captures (detector tuning): raw uint16 little-endian milli-g
// samples from the device, one file per capture. Deliberately outside
// store.js — these are throwaway debug recordings, not app state.
const MOTION_DIR = path.join(__dirname, 'data', 'motionlogs');

app.post('/api/motionlog', express.raw({ type: 'application/octet-stream', limit: '1mb' }), (req, res) => {
  const label = String(req.query.label || 'capture').replace(/[^a-zA-Z0-9_-]/g, '');
  const rate = parseInt(req.query.rate, 10) || 0;
  const buf = req.body;
  if (!Buffer.isBuffer(buf) || buf.length < 4) return res.status(400).json({ error: 'empty capture' });
  const samples = [];
  for (let i = 0; i + 1 < buf.length; i += 2) samples.push(buf.readUInt16LE(i));
  fs.mkdirSync(MOTION_DIR, { recursive: true });
  const file = `${new Date().toISOString().replace(/[:.]/g, '-')}_${label}.json`;
  fs.writeFileSync(path.join(MOTION_DIR, file),
    JSON.stringify({ label, rate, at: new Date().toISOString(), samples }));
  console.log(`motionlog: ${label}, ${samples.length} samples @ ${rate} Hz -> ${file}`);
  res.json({ ok: true, samples: samples.length, file });
});

app.get('/api/motionlog/latest', (req, res) => {
  let files = [];
  try { files = fs.readdirSync(MOTION_DIR).filter(f => f.endsWith('.json')).sort(); } catch (_) {}
  if (!files.length) return res.status(404).json({ error: 'no captures yet' });
  res.type('json').send(fs.readFileSync(path.join(MOTION_DIR, files[files.length - 1])));
});

// Capture index for the Motion Lab panel (newest first, metadata only).
app.get('/api/motionlog/list', (req, res) => {
  let files = [];
  try { files = fs.readdirSync(MOTION_DIR).filter(f => f.endsWith('.json')).sort().reverse(); } catch (_) {}
  const list = [];
  for (const f of files.slice(0, 30)) {
    try {
      const d = JSON.parse(fs.readFileSync(path.join(MOTION_DIR, f)));
      list.push({ file: f, label: d.label, at: d.at, rate: d.rate, count: (d.samples || []).length });
    } catch (_) {}
  }
  res.json(list);
});

// One capture by file name (as returned by /list; name is sanitized).
app.get('/api/motionlog/file/:name', (req, res) => {
  const name = String(req.params.name).replace(/[^a-zA-Z0-9_.:-]/g, '');
  const p = path.join(MOTION_DIR, name);
  if (!name.endsWith('.json') || !fs.existsSync(p)) return res.status(404).json({ error: 'not found' });
  res.type('json').send(fs.readFileSync(p));
});

// ---------- Dictionary files (device SD card, pulled over WiFi) ------------
// tools/make_dict.py output pushed here by tools/push_dict.py, then pulled
// by the firmware (dict_update.cpp) when the sync response's dictPushToken
// advances. Same deliberately-outside-store.js idiom as motionlogs: big
// binary blobs, not app state — only the token lives in the store.
const crypto = require('crypto');
const DICT_DIR = path.join(__dirname, 'data', 'dict');
const DICT_FILES = ['words.idx', 'defs.dat'];

app.post('/api/dict/files/:name', express.raw({ type: 'application/octet-stream', limit: '64mb' }), (req, res) => {
  const name = String(req.params.name);
  if (!DICT_FILES.includes(name)) return res.status(400).json({ error: 'unknown dict file' });
  if (!Buffer.isBuffer(req.body) || !req.body.length) return res.status(400).json({ error: 'empty body' });
  fs.mkdirSync(DICT_DIR, { recursive: true });
  fs.writeFileSync(path.join(DICT_DIR, name), req.body);
  console.log(`dict: received ${name} (${req.body.length} bytes)`);
  res.json({ ok: true, bytes: req.body.length });
});

// Stamp the manifest (sizes + md5s the firmware verifies against) and bump
// the token. Called once by push_dict.py after both uploads; the config bump
// makes a live device sync within seconds instead of the 60 s cadence.
app.post('/api/dict/publish', (req, res) => {
  const files = [];
  for (const name of DICT_FILES) {
    const p = path.join(DICT_DIR, name);
    if (!fs.existsSync(p)) return res.status(400).json({ error: `${name} not uploaded yet` });
    const buf = fs.readFileSync(p);
    files.push({ name, size: buf.length, md5: crypto.createHash('md5').update(buf).digest('hex') });
  }
  const data = store.update(d => {
    d.dictPushToken = (d.dictPushToken || 0) + 1;
    bumpConfig(d);
  });
  fs.writeFileSync(path.join(DICT_DIR, 'manifest.json'),
    JSON.stringify({ token: data.dictPushToken, publishedAt: new Date().toISOString(), files }));
  broadcastConfigChange(data.configVersion, data.configUpdatedAt);
  console.log(`dict: published token ${data.dictPushToken} (${files.map(f => `${f.name} ${f.size}B`).join(', ')})`);
  res.json({ ok: true, token: data.dictPushToken, files });
});

// File-manager listing for the Options tab's "SD card files" panel: what's
// uploaded, whether each file made it into the last publish, current token.
app.get('/api/dict/files', (req, res) => {
  let manifest = null;
  try { manifest = JSON.parse(fs.readFileSync(path.join(DICT_DIR, 'manifest.json'))); } catch (_) {}
  const publishedMs = manifest && manifest.publishedAt ? new Date(manifest.publishedAt).getTime() : 0;
  const files = DICT_FILES.map(name => {
    const p = path.join(DICT_DIR, name);
    if (!fs.existsSync(p)) return { name, missing: true };
    const st = fs.statSync(p);
    return {
      name, size: st.size, mtime: st.mtime.toISOString(),
      // +1s slack: publish stamps the manifest moments after reading the files
      published: publishedMs > 0 && st.mtimeMs <= publishedMs + 1000,
    };
  });
  res.json({
    files,
    token:       manifest ? manifest.token : 0,
    publishedAt: manifest ? manifest.publishedAt || null : null,
  });
});

app.get('/api/dict/manifest', (req, res) => {
  const p = path.join(DICT_DIR, 'manifest.json');
  if (!fs.existsSync(p)) return res.status(404).json({ error: 'nothing published' });
  res.type('json').send(fs.readFileSync(p));
});

app.get('/api/dict/files/:name', (req, res) => {
  const name = String(req.params.name);
  const p = path.join(DICT_DIR, name);
  if (!DICT_FILES.includes(name) || !fs.existsSync(p)) return res.status(404).json({ error: 'not found' });
  res.setHeader('Content-Type', 'application/octet-stream');
  res.sendFile(p);
});

// Focus-block history for the dashboard panel.
app.get('/api/focus', (req, res) => {
  const d = store.get();
  res.json({ total: d.focusSessions || 0, history: d.focusHistory || {} });
});

// Sleep history for the dashboard panel.
app.get('/api/sleep', (req, res) => {
  const d = store.get();
  res.json({ history: d.sleepHistory || {} });
});

// Walking analytics for the dashboard panel.
app.get('/api/steps', (req, res) => {
  const d = store.get();
  res.json({
    goal:     WALK_DAILY_GOAL,
    strideCm: WALK_STRIDE_CM,
    history:  d.stepHistory || {},
  });
});

// ---------- USB sync bridge (start/stop from the dashboard) ----------------
// Spawns usb-bridge.py as a child process so the device can sync over USB
// serial without the user touching a terminal. The bridge has its own 2 s
// reconnect loop, so it survives unplug/replug once started.

const { spawn } = require('child_process');

// In the Docker image the bridge sits next to server.js (/app/usb-bridge.py);
// running from the repo it's one directory up.
const BRIDGE_SCRIPT = [
  path.join(__dirname, 'usb-bridge.py'),
  path.join(__dirname, '..', 'usb-bridge.py'),
].find(p => fs.existsSync(p));

let bridgeProc = null;
let bridgeLog  = [];
// Routine sync chatter (2+ lines every 10 s) is squelched into a periodic
// summary so the ring keeps real events — crash forensics got pushed out of
// the old 100-line ring twice before anyone could read it.
let syncSquelch = { count: 0, lastEmit: 0 };
function bridgeLogPush(line) {
  if (!line) return;
  if (/usb-bridge: synced|Synced over USB bridge/.test(line)) {
    syncSquelch.count++;
    const now = Date.now();
    if (now - syncSquelch.lastEmit < 300000) return;
    line = `sync ok (x${syncSquelch.count} since last note)`;
    syncSquelch = { count: 0, lastEmit: now };
  }
  bridgeLog.push(`[${new Date().toISOString().slice(11, 19)}] ${line}`);
  if (bridgeLog.length > 500) bridgeLog.shift();
}

function startBridge(port = '/dev/ttyACM0') {
  if (bridgeProc || !BRIDGE_SCRIPT) return !!bridgeProc;
  // Talk to ourselves on the in-container port, not the published one.
  bridgeProc = spawn('python3', ['-u', BRIDGE_SCRIPT, port, `http://localhost:${PORT}`]);
  bridgeLogPush(`bridge started (pid ${bridgeProc.pid}, port ${port})`);

  let buf = '';
  const onData = chunk => {
    buf += chunk.toString();
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      bridgeLogPush(buf.slice(0, nl).trimEnd());
      buf = buf.slice(nl + 1);
    }
  };
  bridgeProc.stdout.on('data', onData);
  bridgeProc.stderr.on('data', onData);
  bridgeProc.on('exit', (code, signal) => {
    bridgeLogPush(`bridge exited (${signal || code})`);
    bridgeProc = null;
  });
  return true;
}

app.get('/api/bridge/status', (req, res) => {
  res.json({ running: !!bridgeProc, available: !!BRIDGE_SCRIPT, log: bridgeLog.slice(-15) });
});

app.post('/api/bridge/start', (req, res) => {
  if (!BRIDGE_SCRIPT) return res.status(500).json({ error: 'usb-bridge.py not found' });
  startBridge((req.body && req.body.port) || '/dev/ttyACM0');
  res.json({ running: !!bridgeProc });
});

app.post('/api/bridge/stop', (req, res) => {
  if (bridgeProc) bridgeProc.kill('SIGTERM');
  res.json({ running: false });
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

  // Prefer the device's own report (rides every sync) over the estimates.
  const dev = data.deviceStorage || null;
  res.json({
    dashboard: { used: storeSize, quota: DASHBOARD_QUOTA },
    nvs:       (dev && dev.nvsTotal > 0)
                 ? { used: dev.nvsUsed, total: dev.nvsTotal, real: true }
                 : { used: nvsEstimate, total: NVS_SIZE },
    flash:     (dev && dev.appTotal > 0)
                 ? { used: dev.sketch, total: dev.appTotal }
                 : null,
    counts:    { habits: habitCount, quests: questCount, todos: todoCount, logEntries }
  });
});

app.listen(PORT, () => {
  console.log(`CyberPet dashboard running on port ${PORT}`);
  // Auto-start the USB bridge: its own 2 s retry loop handles the device
  // being unplugged, so it's safe to start even with nothing docked.
  // The dashboard button remains a manual override (stop/restart).
  if (startBridge()) console.log('USB bridge auto-started');
});
