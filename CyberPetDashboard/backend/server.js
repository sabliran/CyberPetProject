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
  const { deviceId, petState, completedHabits, steps, sleep, backSessions } = req.body;
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
    // never claw back earned trophies).
    if (typeof backSessions === 'number' && backSessions > (d.backSessions || 0)) {
      d.backSessions = backSessions;
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
    configVersion: data.configVersion || 1,
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

  // Strength: completion-days of habits whose name mentions the exercise
  // (matches "10 pullups", "push ups", "Pull-Ups", ...). Distinct dates so
  // renames/duplicates can't double-count a day.
  const daysMatching = (re) => new Set(
    log.filter(e => re.test(e.habitName || '')).map(e => e.date)
  ).size;
  const pullDays = daysMatching(/pull[\s-]?ups?/i);
  const pushDays = daysMatching(/push[\s-]?ups?/i);

  return [
    // habits
    { id: 'first-habit', cat: 'habits',  name: 'First step',      desc: 'complete your first habit',   earned: log.length >= 1 },
    { id: 'habits-10',   cat: 'habits',  name: 'Regular',         desc: '10 habit completions',        earned: log.length >= 10 },
    { id: 'habits-100',  cat: 'habits',  name: 'Centurion',       desc: '100 habit completions',       earned: log.length >= 100 },
    { id: 'habits-500',  cat: 'habits',  name: 'Legend',          desc: '500 habit completions',       earned: log.length >= 500 },
    { id: 'streak-7',    cat: 'habits',  name: 'Week warrior',    desc: 'a 7-day habit streak',        earned: bestStreak >= 7 },
    { id: 'streak-30',   cat: 'habits',  name: 'Iron month',      desc: 'a 30-day habit streak',       earned: bestStreak >= 30 },
    { id: 'streak-100',  cat: 'habits',  name: 'Unstoppable',     desc: 'a 100-day habit streak',      earned: bestStreak >= 100 },
    { id: 'busy-bee',    cat: 'habits',  name: 'Busy bee',        desc: '5 habits done in one day',    earned: maxPerDay >= 5 },
    // walking
    { id: 'steps-2k',    cat: 'walking', name: 'Warm-up',         desc: '2,000 steps in a day',        earned: stepVals.some(s => s >= 2000) },
    { id: 'steps-10k',   cat: 'walking', name: 'Long hauler',     desc: '10,000 steps in one day',     earned: stepVals.some(s => s >= 10000) },
    { id: 'steps-20k',   cat: 'walking', name: 'Ultra',           desc: '20,000 steps in one day',     earned: stepVals.some(s => s >= 20000) },
    { id: 'walk-5',      cat: 'walking', name: 'Regular walker',  desc: '5 days at the step goal',     earned: goalWalks >= 5 },
    { id: 'walk-25',     cat: 'walking', name: 'Trail veteran',   desc: '25 days at the step goal',    earned: goalWalks >= 25 },
    { id: 'km-100',      cat: 'walking', name: 'Globetrotter',    desc: '100 km walked in total',      earned: totalKm >= 100 },
    // strength
    { id: 'back-1',      cat: 'strength', name: 'First row',      desc: 'first back workout',          earned: (d.backSessions || 0) >= 1 },
    { id: 'back-10',     cat: 'strength', name: 'Row machine',    desc: '10 back workouts',            earned: (d.backSessions || 0) >= 10 },
    { id: 'back-30',     cat: 'strength', name: 'Iron back',      desc: '30 back workouts',            earned: (d.backSessions || 0) >= 30 },
    { id: 'pull-1',      cat: 'strength', name: 'First hang',     desc: 'first pull-up day',           earned: pullDays >= 1 },
    { id: 'pull-10',     cat: 'strength', name: 'Bar starter',    desc: '10 pull-up days',             earned: pullDays >= 10 },
    { id: 'pull-30',     cat: 'strength', name: 'Bar veteran',    desc: '30 pull-up days',             earned: pullDays >= 30 },
    { id: 'push-1',      cat: 'strength', name: 'First rep',      desc: 'first push-up day',           earned: pushDays >= 1 },
    { id: 'push-10',     cat: 'strength', name: 'Floor regular',  desc: '10 push-up days',             earned: pushDays >= 10 },
    { id: 'push-30',     cat: 'strength', name: 'Floor general',  desc: '30 push-up days',             earned: pushDays >= 30 },
    // sleep
    { id: 'sleep-7',     cat: 'sleep',   name: 'Well rested',     desc: '7 nights of good sleep',      earned: goodNights >= 7 },
    { id: 'dream-week',  cat: 'sleep',   name: 'Dream week',      desc: '7 good nights in a row',      earned: goodRun >= 7 },
    { id: 'sleep-30',    cat: 'sleep',   name: 'Sleep scientist', desc: '30 nights logged',            earned: sleepVals.length >= 30 },
    { id: 'bad-night',   cat: 'sleep',   name: 'It happens',      desc: 'honestly log a bad night',    earned: sleepVals.some(q => q === 2) },
    // pet
    { id: 'hatched',     cat: 'pet',     name: 'Hatched',         desc: 'reach the blob stage',        earned: stage >= 1 },
    { id: 'level-5',     cat: 'pet',     name: 'Level 5',         desc: 'reach level 5',               earned: level >= 5 },
    { id: 'level-10',    cat: 'pet',     name: 'Level 10',        desc: 'reach level 10',              earned: level >= 10 },
    { id: 'level-20',    cat: 'pet',     name: 'Overachiever',    desc: 'reach level 20',              earned: level >= 20 },
    { id: 'evolved',     cat: 'pet',     name: 'Final form',      desc: 'reach the evolved stage',     earned: stage >= 3 },
    { id: 'quest-1',     cat: 'pet',     name: 'Adventurer',      desc: 'complete your first quest',   earned: questDone },
    { id: 'days-30',     cat: 'pet',     name: 'Survivor',        desc: '30 days together',            earned: daysAlive >= 30 },
  ];
}

app.get('/api/trophies', (req, res) => {
  res.json(computeTrophies(store.get()));
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
function bridgeLogPush(line) {
  if (!line) return;
  bridgeLog.push(`[${new Date().toISOString().slice(11, 19)}] ${line}`);
  if (bridgeLog.length > 100) bridgeLog.shift();
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

  res.json({
    dashboard: { used: storeSize, quota: DASHBOARD_QUOTA },
    nvs:       { used: nvsEstimate, total: NVS_SIZE },
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
