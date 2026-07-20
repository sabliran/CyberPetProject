const STAGE_NAMES  = ['Egg', 'Blob', 'Creature', 'Evolved'];
const STAGE_COLORS = ['#CFCFCF', '#6FD08C', '#4FA3E3', '#C77DFF'];
const CAT_LABELS   = { hardware: 'Hardware', firmware: 'Firmware', ui: 'UI',
                        dashboard: 'Dashboard', general: 'General' };

let activeTodoFilter = 'all';

// ---- API ------------------------------------------------------------------

async function api(path, options = {}) {
  const res = await fetch(`/api${path}`, {
    headers: { 'Content-Type': 'application/json' },
    ...options
  });
  if (!res.ok) throw new Error(`API ${path} failed: ${res.status}`);
  if (res.status === 204) return null;
  return res.json();
}

function escapeHtml(str) {
  const div = document.createElement('div');
  div.textContent = str;
  return div.innerHTML;
}

function timeAgo(iso) {
  if (!iso) return '— never synced —';
  const diffMs = Date.now() - new Date(iso).getTime();
  const mins = Math.floor(diffMs / 60000);
  if (mins < 1) return 'synced just now';
  if (mins < 60) return `synced ${mins}m ago`;
  const hrs = Math.floor(mins / 60);
  if (hrs < 24) return `synced ${hrs}h ago`;
  return `synced ${Math.floor(hrs / 24)}d ago`;
}

// ---- Tabs -----------------------------------------------------------------

document.querySelectorAll('.tabnav__btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tabnav__btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tabpanel').forEach(p => p.classList.add('tabpanel--hidden'));
    btn.classList.add('active');
    document.getElementById(`tab-${btn.dataset.tab}`).classList.remove('tabpanel--hidden');
    if (btn.dataset.tab === 'options') {
      loadSettings();
      loadStorage();
      loadBackupInfo();
      loadConfigVersion();
    }
    if (btn.dataset.tab === 'dev') {
      loadTodos();
      loadDevNotes();
      loadPetStateRaw();
    }
  });
});

// ---- Pet ------------------------------------------------------------------

async function loadPet() {
  const pet = await api('/pet');
  const stage = pet.stage ?? 0;
  document.getElementById('petStage').textContent = STAGE_NAMES[stage] ?? '—';
  document.getElementById('petXP').textContent    = pet.xp ?? 0;
  document.getElementById('petMood').textContent  = `${pet.mood ?? 0}%`;
  document.getElementById('petDays').textContent  = pet.daysAlive ?? 0;
  const hunger = pet.hunger ?? 100;
  const alive  = pet.alive  !== false;
  const hungerEl = document.getElementById('petHunger');
  if (!alive) {
    hungerEl.textContent = '💀 Dead';
    hungerEl.style.color = '#FF4040';
  } else if (hunger >= 70) {
    hungerEl.textContent = `${hunger}% (full)`;
    hungerEl.style.color = '#3EE8A0';
  } else if (hunger >= 40) {
    hungerEl.textContent = `${hunger}% (hungry)`;
    hungerEl.style.color = '#D4A030';
  } else if (hunger >= 20) {
    hungerEl.textContent = `${hunger}% (very hungry)`;
    hungerEl.style.color = '#FF8040';
  } else {
    hungerEl.textContent = `${hunger}% ⚠ STARVING`;
    hungerEl.style.color = '#FF4040';
  }

  // Health: per-habit credit at the device's daily reset — each done habit
  // heals 10, each missed habit chips 10 (damage capped 30/day; ratios tilt
  // with difficulty). 0 = dead.
  const health = pet.health ?? 100;
  const healthEl = document.getElementById('petHealth');
  if (!alive) {
    healthEl.textContent = '0%';
    healthEl.style.color = '#FF4040';
  } else {
    healthEl.textContent = `${health}%`;
    healthEl.style.color = health >= 70 ? '#3EE8A0' : health >= 40 ? '#D4A030' : '#FF4040';
  }

  const blob  = document.getElementById('petBlob');
  const color = STAGE_COLORS[stage] ?? '#6FD08C';
  blob.style.background = `radial-gradient(circle at 32% 28%, rgba(255,255,255,0.35), transparent 55%), ${color}`;
  blob.style.color = color;

  document.getElementById('syncStatus').textContent = timeAgo(pet.lastSyncedAt);
  const dot = document.getElementById('connDot');
  const recent = pet.lastSyncedAt && (Date.now() - new Date(pet.lastSyncedAt).getTime() < 5 * 60 * 1000);
  dot.classList.toggle('online', !!recent);
}

// ---- Habits ---------------------------------------------------------------

function habitRow(habit) {
  const li = document.createElement('li');
  li.className = 'itemlist__row';
  li.innerHTML = `
    <span class="itemlist__name">${escapeHtml(habit.name)}</span>
    <span class="itemlist__meta">
      <span class="itemlist__xp">+${habit.xpValue} xp</span>
      <button class="itemlist__remove" title="Remove" data-id="${habit.id}">✕</button>
    </span>`;
  li.querySelector('.itemlist__remove').addEventListener('click', async () => {
    await api(`/habits/${habit.id}`, { method: 'DELETE' });
    loadHabits();
  });
  return li;
}

// Mirrors MAX_HABITS in the firmware's habits.h — the device silently shows
// only the first N active habits, which once ate a habit without a trace.
const DEVICE_HABIT_SLOTS = 16;

async function loadHabits() {
  const habits = await api('/habits');
  const list = document.getElementById('habitList');
  list.innerHTML = '';

  let warn = document.getElementById('habitCapWarn');
  if (habits.length > DEVICE_HABIT_SLOTS) {
    if (!warn) {
      warn = document.createElement('div');
      warn.id = 'habitCapWarn';
      warn.style.cssText = 'color:#FFD060;font-size:13px;margin:0 0 8px;';
      list.parentNode.insertBefore(warn, list);
    }
    warn.textContent = `⚠ ${habits.length} active habits — the device only holds ${DEVICE_HABIT_SLOTS}; the rest won't appear on it.`;
  } else if (warn) {
    warn.remove();
  }
  if (habits.length === 0) {
    list.innerHTML = '<li class="itemlist__empty">No habits yet — add one below.</li>';
  } else {
    habits.forEach(h => list.appendChild(habitRow(h)));
  }
}

document.getElementById('habitForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const name    = document.getElementById('habitName').value.trim();
  const xpValue = parseInt(document.getElementById('habitXP').value, 10);
  if (!name) return;
  await api('/habits', { method: 'POST', body: JSON.stringify({ name, xpValue }) });
  e.target.reset();
  document.getElementById('habitXP').value = 10;
  loadHabits();
});

// ---- Danger zone: XP reset --------------------------------------------------

document.getElementById('resetXpBtn').addEventListener('click', async () => {
  if (!confirm('Reset XP, level and stage to zero — on the dashboard AND the device?')) return;
  try {
    const r = await api('/pet/reset', { method: 'POST' });
    if (r.petState) refreshPetStats(r.petState);
    const flag = document.getElementById('resetXpFlag');
    flag.classList.add('show');
    setTimeout(() => flag.classList.remove('show'), 4000);
  } catch (_) {}
});

// ---- USB bridge control -----------------------------------------------------

let bridgeRunning = false;

function renderBridge() {
  document.getElementById('bridgeDot').classList.toggle('online', bridgeRunning);
  document.getElementById('bridgeBtn').textContent =
    bridgeRunning ? 'stop USB bridge' : 'start USB bridge';
}

async function refreshBridgeStatus() {
  try {
    const s = await api('/bridge/status');
    bridgeRunning = s.running;
  } catch (_) { bridgeRunning = false; }
  renderBridge();
}

document.getElementById('bridgeBtn').addEventListener('click', async () => {
  try {
    const s = await api(bridgeRunning ? '/bridge/stop' : '/bridge/start', { method: 'POST' });
    bridgeRunning = s.running;
  } catch (_) {}
  renderBridge();
  setTimeout(refreshBridgeStatus, 1500);  // catch an immediate crash-exit
});

refreshBridgeStatus();
setInterval(refreshBridgeStatus, 5000);

// Update the pet header after an XP-awarding action (quest/goal completion).
function refreshPetStats(ps) {
  document.getElementById('petXP').textContent   = ps.xp ?? 0;
  document.getElementById('petMood').textContent = `${ps.mood ?? 0}%`;
  const blob  = document.getElementById('petBlob');
  const color = STAGE_COLORS[ps.stage] ?? '#6FD08C';
  blob.style.background = `radial-gradient(circle at 32% 28%, rgba(255,255,255,0.35), transparent 55%), ${color}`;
  blob.style.color = color;
  document.getElementById('petStage').textContent = STAGE_NAMES[ps.stage] ?? '—';
}

// ---- Goals ----------------------------------------------------------------

function goalRow(goal) {
  const li = document.createElement('li');
  // Reuses the quest checkbox styling — identical done/undone interaction.
  li.className = `itemlist__row quest-row${goal.done ? ' quest-row--done' : ''}`;
  li.innerHTML = `
    <label class="quest-check">
      <input type="checkbox" class="quest-checkbox" data-id="${goal.id}" ${goal.done ? 'checked' : ''}>
      <span class="quest-name">${escapeHtml(goal.name)}</span>
    </label>
    <span class="itemlist__meta">
      <span class="itemlist__period">${goal.period}</span>
      <span class="itemlist__xp">+${goal.xpValue} xp</span>
      <button class="itemlist__remove" title="Remove" data-id="${goal.id}">✕</button>
    </span>`;

  li.querySelector('.quest-checkbox').addEventListener('change', async (e) => {
    const result = await api(`/goals/${goal.id}`, {
      method: 'PATCH',
      body: JSON.stringify({ done: e.target.checked })
    });
    if (result.petState) refreshPetStats(result.petState);
    loadGoals();
  });

  li.querySelector('.itemlist__remove').addEventListener('click', async () => {
    await api(`/goals/${goal.id}`, { method: 'DELETE' });
    loadGoals();
  });
  return li;
}

async function loadGoals() {
  const goals = await api('/goals');
  const list = document.getElementById('goalList');
  list.innerHTML = '';
  // Show pending first, then done (matches the quest list)
  const sorted = [...goals.filter(g => !g.done), ...goals.filter(g => g.done)];
  if (sorted.length === 0) {
    list.innerHTML = '<li class="itemlist__empty">No goals yet — add one below.</li>';
  } else {
    sorted.forEach(g => list.appendChild(goalRow(g)));
  }
}

document.getElementById('goalForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const name    = document.getElementById('goalName').value.trim();
  const xpValue = parseInt(document.getElementById('goalXP').value, 10);
  const period  = document.getElementById('goalPeriod').value;
  if (!name) return;
  await api('/goals', { method: 'POST', body: JSON.stringify({ name, xpValue, period }) });
  e.target.reset();
  document.getElementById('goalXP').value = 50;
  loadGoals();
});

// ---- Quests ---------------------------------------------------------------

function questRow(quest) {
  const li = document.createElement('li');
  li.className = `itemlist__row quest-row${quest.done ? ' quest-row--done' : ''}`;
  li.innerHTML = `
    <label class="quest-check">
      <input type="checkbox" class="quest-checkbox" data-id="${quest.id}" ${quest.done ? 'checked' : ''}>
      <span class="quest-name">${escapeHtml(quest.name)}</span>
    </label>
    <span class="itemlist__meta">
      <span class="itemlist__xp">+${quest.xpValue} xp</span>
      <button class="itemlist__remove" title="Remove" data-id="${quest.id}">✕</button>
    </span>`;

  li.querySelector('.quest-checkbox').addEventListener('change', async (e) => {
    const result = await api(`/quests/${quest.id}`, {
      method: 'PATCH',
      body: JSON.stringify({ done: e.target.checked })
    });
    // Refresh pet stats since XP may have changed
    if (result.petState) refreshPetStats(result.petState);
    loadQuests();
  });

  li.querySelector('.itemlist__remove').addEventListener('click', async () => {
    await api(`/quests/${quest.id}`, { method: 'DELETE' });
    loadQuests();
  });
  return li;
}

async function loadQuests() {
  const quests = await api('/quests');
  const list = document.getElementById('questList');
  list.innerHTML = '';
  // Show pending first, then done
  const sorted = [...quests.filter(q => !q.done), ...quests.filter(q => q.done)];
  if (sorted.length === 0) {
    list.innerHTML = '<li class="itemlist__empty">No quests yet — add an objective below.</li>';
  } else {
    sorted.forEach(q => list.appendChild(questRow(q)));
  }
}

document.getElementById('questForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const name    = document.getElementById('questName').value.trim();
  const xpValue = parseInt(document.getElementById('questXP').value, 10);
  if (!name) return;
  await api('/quests', { method: 'POST', body: JSON.stringify({ name, xpValue }) });
  e.target.reset();
  document.getElementById('questXP').value = 100;
  loadQuests();
});

// ---- Settings -------------------------------------------------------------

async function loadSettings() {
  const s = await api('/settings');
  document.getElementById('setDifficulty').value   = s.difficulty ?? 1;
  document.getElementById('setPetName').value      = s.petName ?? '';
  document.getElementById('setResetHour').value    = s.dailyResetHour ?? 0;
  document.getElementById('setMoodGain').value     = s.moodGainPerHabit ?? 8;
  document.getElementById('setMoodLoss').value     = s.moodDecayPerMiss ?? 15;
  document.getElementById('setSyncInterval').value = s.syncIntervalSeconds ?? 60;
  document.getElementById('deviceId').textContent  = s.deviceId ?? 'not yet synced';
}

// Picking a difficulty suggests matching mood values (still editable before
// saving). The hunger/meal/health numbers live on the device (pet.h DIFF_*).
const DIFF_MOOD_PRESETS = { 0: { gain: 4, loss: 10 }, 1: { gain: 2, loss: 15 }, 2: { gain: 2, loss: 20 } };
document.getElementById('setDifficulty').addEventListener('change', (e) => {
  const p = DIFF_MOOD_PRESETS[e.target.value];
  if (!p) return;
  document.getElementById('setMoodGain').value = p.gain;
  document.getElementById('setMoodLoss').value = p.loss;
});

document.getElementById('settingsForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const payload = {
    difficulty:          parseInt(document.getElementById('setDifficulty').value, 10),
    petName:             document.getElementById('setPetName').value.trim(),
    dailyResetHour:      parseInt(document.getElementById('setResetHour').value, 10),
    moodGainPerHabit:    parseInt(document.getElementById('setMoodGain').value, 10),
    moodDecayPerMiss:    parseInt(document.getElementById('setMoodLoss').value, 10),
    syncIntervalSeconds: parseInt(document.getElementById('setSyncInterval').value, 10)
  };
  for (const k of Object.keys(payload)) {
    if (payload[k] === '' || (typeof payload[k] === 'number' && !Number.isFinite(payload[k])))
      delete payload[k];
  }
  await api('/settings', { method: 'POST', body: JSON.stringify(payload) });
  flashPushed();
});

// ---- Dev: Todos -----------------------------------------------------------

function todoRow(todo) {
  const li = document.createElement('li');
  li.className = `todo-row${todo.done ? ' todo-row--done' : ''}`;
  li.dataset.cat = todo.category;
  li.innerHTML = `
    <label class="todo-check">
      <input type="checkbox" class="todo-cb" data-id="${todo.id}" ${todo.done ? 'checked' : ''}>
      <span class="todo-text">${escapeHtml(todo.text)}</span>
    </label>
    <span class="todo-right">
      <span class="todo-cat todo-cat--${todo.category}">${CAT_LABELS[todo.category] || todo.category}</span>
      <button class="todo-del" data-id="${todo.id}" title="Delete">✕</button>
    </span>`;

  li.querySelector('.todo-cb').addEventListener('change', async (e) => {
    await api(`/todos/${todo.id}`, {
      method: 'PATCH',
      body: JSON.stringify({ done: e.target.checked })
    });
    loadTodos();
  });
  li.querySelector('.todo-del').addEventListener('click', async () => {
    await api(`/todos/${todo.id}`, { method: 'DELETE' });
    loadTodos();
  });
  return li;
}

async function loadTodos() {
  const todos = await api('/todos');
  const list = document.getElementById('todoList');
  list.innerHTML = '';

  const filtered = activeTodoFilter === 'all'
    ? todos
    : todos.filter(t => t.category === activeTodoFilter);

  const sorted = [...filtered.filter(t => !t.done), ...filtered.filter(t => t.done)];

  if (sorted.length === 0) {
    list.innerHTML = '<li class="itemlist__empty">Nothing here — all done or add below.</li>';
  } else {
    sorted.forEach(t => list.appendChild(todoRow(t)));
  }

  // Progress summary
  const done  = todos.filter(t => t.done).length;
  const total = todos.length;
  const pct   = total ? Math.round(done / total * 100) : 0;
  document.getElementById('todoProgress').textContent = `${done} / ${total} done (${pct}%)`;
  document.getElementById('todoProgressBar').style.width = `${pct}%`;
}

document.getElementById('todoFilters').addEventListener('click', (e) => {
  if (!e.target.matches('.filter-btn')) return;
  document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
  e.target.classList.add('active');
  activeTodoFilter = e.target.dataset.cat;
  loadTodos();
});

document.getElementById('todoForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const text     = document.getElementById('todoText').value.trim();
  const category = document.getElementById('todoCat').value;
  if (!text) return;
  await api('/todos', { method: 'POST', body: JSON.stringify({ text, category }) });
  e.target.reset();
  loadTodos();
});

// ---- Dev: Notes -----------------------------------------------------------

let notesSaveTimer = null;

async function loadDevNotes() {
  const { text } = await api('/devnotes');
  document.getElementById('devNotes').value = text;
}

document.getElementById('devNotes').addEventListener('input', () => {
  clearTimeout(notesSaveTimer);
  notesSaveTimer = setTimeout(async () => {
    const text = document.getElementById('devNotes').value;
    await api('/devnotes', { method: 'POST', body: JSON.stringify({ text }) });
    const flag = document.getElementById('notesSaved');
    flag.classList.add('show');
    setTimeout(() => flag.classList.remove('show'), 1500);
  }, 800);
});

// ---- Dev: Pet state raw ---------------------------------------------------

async function loadPetStateRaw() {
  const pet = await api('/pet');
  document.getElementById('petStateJson').textContent = JSON.stringify(pet, null, 2);
}

document.getElementById('refreshPetState').addEventListener('click', loadPetStateRaw);

// ---- Storage --------------------------------------------------------------

function fmtBytes(b) {
  if (b < 1024)        return `${b} B`;
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`;
  return `${(b / (1024 * 1024)).toFixed(2)} MB`;
}

async function loadStorage() {
  const s = await api('/storage');

  const dashPct = Math.min(100, s.dashboard.used / s.dashboard.quota * 100);
  const nvsPct  = Math.min(100, s.nvs.used  / s.nvs.total  * 100);

  document.getElementById('storeDashVal').textContent =
    `${fmtBytes(s.dashboard.used)} / ${fmtBytes(s.dashboard.quota)}`;
  document.getElementById('storeNvsVal').textContent =
    `${fmtBytes(s.nvs.used)} / ${fmtBytes(s.nvs.total)} est.`;

  const dashFill = document.getElementById('storeDashFill');
  dashFill.style.width = `${dashPct}%`;
  dashFill.className = 'storage-fill storage-fill--dashboard' +
    (dashPct > 90 ? ' storage-fill--crit' : dashPct > 70 ? ' storage-fill--warn' : '');

  const nvsFill = document.getElementById('storeNvsFill');
  nvsFill.style.width = `${nvsPct}%`;
  nvsFill.className = 'storage-fill storage-fill--nvs' +
    (nvsPct > 90 ? ' storage-fill--crit' : nvsPct > 70 ? ' storage-fill--warn' : '');

  const counts = document.getElementById('storageCounts');
  const c = s.counts;
  counts.innerHTML = [
    ['Habits', c.habits],
    ['Quests', c.quests],
    ['Todos', c.todos],
    ['Log entries', c.logEntries]
  ].map(([label, val]) =>
    `<div class="storage-count">${label} <span>${val}</span></div>`
  ).join('');
}

document.getElementById('refreshStorage').addEventListener('click', loadStorage);

// ---- Backup ---------------------------------------------------------------

async function loadBackupInfo() {
  const s = await api('/storage');
  const c = s.counts;
  document.getElementById('backupInfo').textContent =
    `${c.habits} habits · ${c.quests} quests · ${c.logEntries} log entries · ${fmtBytes(s.dashboard.used)}`;
}

// ---- History --------------------------------------------------------------

let historyDays = 28;

function pctToLevel(pct) {
  if (pct === 0) return 0;
  if (pct <= 25) return 1;
  if (pct <= 50) return 2;
  if (pct <= 75) return 3;
  return 4;
}

function renderHeatmap(heatmap) {
  const container = document.getElementById('heatmap');
  container.innerHTML = '';
  for (const cell of heatmap) {
    const div = document.createElement('div');
    div.className = `hm-cell hm-${pctToLevel(cell.pct)}`;
    const label = cell.total > 0
      ? `${cell.date}: ${cell.count}/${cell.total} (${cell.pct}%)`
      : cell.date;
    div.title = label;
    container.appendChild(div);
  }
}

function renderStreaks(streaks) {
  const list = document.getElementById('streakList');
  list.innerHTML = '';
  const todayStr = new Date().toISOString().slice(0, 10);
  for (const h of streaks) {
    const row = document.createElement('div');
    row.className = 'streak-row';
    const dots = h.recent.map((done, i) => {
      const isToday = i === h.recent.length - 1;
      return `<span class="streak-dot${done ? ' done' : ''}${isToday ? ' today' : ''}"></span>`;
    }).join('');
    row.innerHTML = `
      <span class="streak-name" title="${escapeHtml(h.name)}">${escapeHtml(h.name)}</span>
      <span class="streak-dots">${dots}</span>
      <span class="streak-nums">
        <span class="streak-cur" title="current streak">${h.streak}d</span>
        <span class="streak-best" title="best streak">best ${h.best}d</span>
      </span>`;
    list.appendChild(row);
  }
  if (streaks.length === 0) {
    list.innerHTML = '<div style="color:var(--muted);font-size:13px;padding:8px 0">No habits tracked yet.</div>';
  }
}

async function loadHistory() {
  const [{ heatmap }, streaks] = await Promise.all([
    api(`/history?days=${historyDays}`),
    api('/history/streaks')
  ]);
  renderHeatmap(heatmap);
  renderStreaks(streaks);
  document.getElementById('historyStats').innerHTML =
    nextTrophyLine(await getTrophies(), 'habits');
}

document.querySelectorAll('.history-range-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.history-range-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    historyDays = parseInt(btn.dataset.days, 10);
    loadHistory();
  });
});

// ---- Walking analytics -----------------------------------------------------

function walkDateKey(d) { return d.toISOString().slice(0, 10); }

// Shared by every panel's "next trophy" line. Trophies carry cur/target from
// the server; the closest unearned one in the category is the next to chase.
// Short cache so one refresh cycle doesn't hit /api/trophies six times.
let _trophyCache = { at: 0, data: null };
async function getTrophies() {
  if (!_trophyCache.data || Date.now() - _trophyCache.at > 5000) {
    _trophyCache = { at: Date.now(), data: await api('/trophies') };
  }
  return _trophyCache.data;
}

function nextTrophyLine(trophies, cat) {
  const cands = trophies.filter(t => t.cat === cat && !t.earned && t.target > 0);
  if (!cands.length) return `<span>all trophies earned here</span>`;
  cands.sort((a, b) => b.cur / b.target - a.cur / a.target);
  const t = cands[0];
  return `<span>next trophy: <b>${escapeHtml(t.name)}</b> (${t.cur.toLocaleString()}/${t.target.toLocaleString()})</span>`;
}

async function loadWalking() {
  const { goal, strideCm, history } = await api('/steps');
  const now = new Date();
  const todaySteps = history[walkDateKey(now)] || 0;

  document.getElementById('walkTodaySteps').textContent = todaySteps.toLocaleString();
  document.getElementById('walkTodayGoal').textContent =
    `/ ${goal.toLocaleString()} steps  ·  ${(todaySteps * strideCm / 100000).toFixed(2)} km`;
  const fill = document.getElementById('walkBarFill');
  fill.style.width = `${Math.min(100, todaySteps / goal * 100)}%`;
  fill.classList.toggle('walk-bar__fill--goal', todaySteps >= goal);

  // Last 14 days, oldest first.
  const chart = document.getElementById('walkChart');
  chart.innerHTML = '';
  let maxSteps = goal;  // scale never below the goal line so bars stay honest
  const days = [];
  for (let i = 13; i >= 0; i--) {
    const key = walkDateKey(new Date(now.getTime() - i * 864e5));
    const steps = history[key] || 0;
    days.push({ key, steps });
    if (steps > maxSteps) maxSteps = steps;
  }
  for (const day of days) {
    const bar = document.createElement('div');
    bar.className = 'walk-col';
    const pct = Math.max(2, day.steps / maxSteps * 100);
    bar.innerHTML = `<div class="walk-col__bar${day.steps >= goal ? ' walk-col__bar--goal' : ''}${day.key === walkDateKey(now) ? ' walk-col__bar--today' : ''}" style="height:${pct}%"></div>`;
    bar.title = `${day.key}: ${day.steps.toLocaleString()} steps`;
    chart.appendChild(bar);
  }

  // Stats: this week's distance, best day on record, goal days last 30.
  let weekSteps = 0;
  for (const day of days.slice(-7)) weekSteps += day.steps;
  let best = { key: null, steps: 0 };
  let goalDays30 = 0;
  const cutoff30 = walkDateKey(new Date(now.getTime() - 29 * 864e5));
  for (const [key, steps] of Object.entries(history)) {
    if (steps > best.steps) best = { key, steps };
    if (key >= cutoff30 && steps >= goal) goalDays30++;
  }
  document.getElementById('walkStats').innerHTML = `
    <span><b>${(weekSteps * strideCm / 100000).toFixed(1)} km</b> last 7 days</span>
    <span><b>${best.steps.toLocaleString()}</b> best day${best.key ? ` (${best.key})` : ''}</span>
    <span><b>${goalDays30}</b> goal day${goalDays30 === 1 ? '' : 's'} last 30</span>` +
    nextTrophyLine(await getTrophies(), 'walking');
}

// ---- Sleep history ----------------------------------------------------------

const SLEEP_LABELS = ['good', 'medium', 'bad'];

async function loadSleep() {
  const { history } = await api('/sleep');
  const now = new Date();
  const todayKey = walkDateKey(now);

  const today = document.getElementById('sleepToday');
  if (todayKey in history) {
    const q = history[todayKey];
    today.innerHTML = `last night: <b class="sleep-q sleep-q--${q}">${SLEEP_LABELS[q]}</b>`;
  } else {
    today.innerHTML = `last night: <span class="sleep-q--none">not logged yet</span>`;
  }

  // Last 14 nights, oldest first — one dot per night.
  const nights = document.getElementById('sleepNights');
  nights.innerHTML = '';
  for (let i = 13; i >= 0; i--) {
    const key = walkDateKey(new Date(now.getTime() - i * 864e5));
    const dot = document.createElement('div');
    const q = history[key];
    dot.className = `sleep-dot${q !== undefined ? ` sleep-dot--${q}` : ''}`;
    dot.title = q !== undefined ? `${key}: ${SLEEP_LABELS[q]}` : `${key}: not logged`;
    nights.appendChild(dot);
  }

  // 30-day tally.
  const cutoff30 = walkDateKey(new Date(now.getTime() - 29 * 864e5));
  const counts = [0, 0, 0];
  for (const [key, q] of Object.entries(history)) {
    if (key >= cutoff30 && q >= 0 && q <= 2) counts[q]++;
  }
  document.getElementById('sleepStats').innerHTML = `
    <span><b class="sleep-q--0">${counts[0]}</b> good</span>
    <span><b class="sleep-q--1">${counts[1]}</b> medium</span>
    <span><b class="sleep-q--2">${counts[2]}</b> bad</span>
    <span>last 30 nights</span>` +
    nextTrophyLine(await getTrophies(), 'sleep');
}

// ---- Back workouts -----------------------------------------------------------

async function loadBackWorkouts() {
  const { total, history } = await api('/backworkouts');
  const now = new Date();
  const todayKey = walkDateKey(now);

  const summary = document.getElementById('backSummary');
  const todayCount = history[todayKey] || 0;
  const dates = Object.keys(history).sort();
  summary.innerHTML = `<b class="back-num">${total}</b> session${total === 1 ? '' : 's'} total` +
    (todayCount ? `  ·  <b class="back-num">${todayCount}</b> today` : '') +
    (!todayCount && dates.length ? `  ·  last: ${dates[dates.length - 1]}` : '');

  // Last 14 days, one block per day (same strip layout as sleep).
  const days = document.getElementById('backDays');
  days.innerHTML = '';
  for (let i = 13; i >= 0; i--) {
    const key = walkDateKey(new Date(now.getTime() - i * 864e5));
    const n = history[key] || 0;
    const dot = document.createElement('div');
    dot.className = `sleep-dot${n > 0 ? ' back-dot--done' : ''}`;
    dot.title = `${key}: ${n} session${n === 1 ? '' : 's'}`;
    days.appendChild(dot);
  }

  // 30-day tally + next trophy progress (thresholds mirror computeTrophies).
  const cutoff30 = walkDateKey(new Date(now.getTime() - 29 * 864e5));
  let last30 = 0;
  for (const [key, n] of Object.entries(history)) if (key >= cutoff30) last30 += n;
  const trophies = await getTrophies();
  const backNext = trophies.filter(t => t.id.startsWith('back-') && !t.earned)
                           .sort((a, b) => a.target - b.target)[0];
  document.getElementById('backStats').innerHTML = `
    <span><b>${last30}</b> last 30 days</span>` +
    (backNext ? `<span>next trophy: <b>${escapeHtml(backNext.name)}</b> (${backNext.cur}/${backNext.target})</span>`
              : `<span>all back trophies earned</span>`);
}

// ---- Push-ups ----------------------------------------------------------------

async function loadPushUps() {
  const { total, history } = await api('/pushups');
  const now = new Date();
  const todayKey = walkDateKey(now);

  const summary = document.getElementById('pushSummary');
  const todayCount = history[todayKey] || 0;
  const dates = Object.keys(history).sort();
  summary.innerHTML = `<b class="push-num">${total}</b> session${total === 1 ? '' : 's'} total` +
    (todayCount ? `  ·  <b class="push-num">${todayCount}</b> today` : '') +
    (!todayCount && dates.length ? `  ·  last: ${dates[dates.length - 1]}` : '');

  const days = document.getElementById('pushDays');
  days.innerHTML = '';
  for (let i = 13; i >= 0; i--) {
    const key = walkDateKey(new Date(now.getTime() - i * 864e5));
    const n = history[key] || 0;
    const dot = document.createElement('div');
    dot.className = `sleep-dot${n > 0 ? ' push-dot--done' : ''}`;
    dot.title = `${key}: ${n} session${n === 1 ? '' : 's'}`;
    days.appendChild(dot);
  }

  const cutoff30 = walkDateKey(new Date(now.getTime() - 29 * 864e5));
  let last30 = 0;
  for (const [key, n] of Object.entries(history)) if (key >= cutoff30) last30 += n;
  const trophies = await getTrophies();
  const boopNext = trophies.filter(t => t.id.startsWith('boop-') && !t.earned)
                           .sort((a, b) => a.target - b.target)[0];
  document.getElementById('pushStats').innerHTML = `
    <span><b>${last30}</b> last 30 days</span>` +
    (boopNext ? `<span>next trophy: <b>${escapeHtml(boopNext.name)}</b> (${boopNext.cur}/${boopNext.target})</span>`
              : `<span>all push-up trophies earned</span>`);
}

// ---- Pull-ups ------------------------------------------------------------------

async function loadPullUps() {
  const { total, history } = await api('/pullups');
  const now = new Date();
  const todayKey = walkDateKey(now);

  const summary = document.getElementById('pullupSummary');
  const todayCount = history[todayKey] || 0;
  const dates = Object.keys(history).sort();
  summary.innerHTML = `<b class="pullup-num">${total}</b> session${total === 1 ? '' : 's'} total` +
    (todayCount ? `  ·  <b class="pullup-num">${todayCount}</b> today` : '') +
    (!todayCount && dates.length ? `  ·  last: ${dates[dates.length - 1]}` : '');

  const days = document.getElementById('pullupDays');
  days.innerHTML = '';
  for (let i = 13; i >= 0; i--) {
    const key = walkDateKey(new Date(now.getTime() - i * 864e5));
    const n = history[key] || 0;
    const dot = document.createElement('div');
    dot.className = `sleep-dot${n > 0 ? ' pullup-dot--done' : ''}`;
    dot.title = `${key}: ${n} session${n === 1 ? '' : 's'}`;
    days.appendChild(dot);
  }

  const cutoff30 = walkDateKey(new Date(now.getTime() - 29 * 864e5));
  let last30 = 0;
  for (const [key, n] of Object.entries(history)) if (key >= cutoff30) last30 += n;
  const trophies = await getTrophies();
  const pullNext = trophies.filter(t => t.id.startsWith('pull-') && !t.earned)
                           .sort((a, b) => a.target - b.target)[0];
  document.getElementById('pullupStats').innerHTML = `
    <span><b>${last30}</b> last 30 days</span>` +
    (pullNext ? `<span>next trophy: <b>${escapeHtml(pullNext.name)}</b> (${pullNext.cur}/${pullNext.target})</span>`
              : `<span>all pull-up trophies earned</span>`);
}

// ---- Motion Lab ----------------------------------------------------------------
// Plots device-recorded workout captures (|a| in g at ~66 Hz) and replays the
// firmware's rep detectors over them, so a bad count can be diagnosed from the
// couch. Detector constants mirror the .ino — keep them in sync when retuning.

const MOTION_PULL = { trigger: 1.16, rearm: 1.08, dip: 0.88, ceiling: 1.38,
                      windowMs: 1200, stillMs: 1200, stillBand: 0.12, minGapMs: 2000 };
const MOTION_BACK = { trigger: 1.35, rearm: 1.10, calmMs: 350 };

function motionDetectPull(g, rate) {
  const c = MOTION_PULL, reps = [];
  let armedSession = false, stillSince = null, armedAt = null;
  let armed = true, pending = null, burstMax = 0, lastRep = -1e9;
  for (let i = 0; i < g.length; i++) {
    const t = i / rate * 1000, m = g[i];
    if (!armedSession) {
      if (Math.abs(m - 1) < c.stillBand) {
        if (stillSince === null) stillSince = t;
        if (t - stillSince >= c.stillMs) { armedSession = true; armedAt = t; }
      } else stillSince = null;
      continue;
    }
    if (m < c.rearm) armed = true;
    if (armed && m > c.trigger && pending === null) { armed = false; pending = t; burstMax = m; }
    if (pending !== null) {
      if (m > burstMax) burstMax = m;
      if (m < c.dip) {
        if (burstMax <= c.ceiling && t - lastRep >= c.minGapMs) { reps.push({ t: pending, peak: burstMax }); lastRep = t; }
        pending = null;
      } else if (t - pending > c.windowMs) pending = null;
    }
  }
  return { reps, armedAt };
}

function motionDetectBack(g, rate) {
  const c = MOTION_BACK, reps = [];
  let quietSince = null, armedByCalm = false;
  for (let i = 0; i < g.length; i++) {
    const t = i / rate * 1000, m = g[i];
    if (m < c.rearm) {
      if (quietSince === null) quietSince = t;
      if (t - quietSince >= c.calmMs) armedByCalm = true;
    } else {
      quietSince = null;
      if (armedByCalm && m > c.trigger) { armedByCalm = false; reps.push({ t, peak: m }); }
    }
  }
  return { reps, armedAt: null };
}

function drawMotion(cap) {
  const canvas = document.getElementById('motionCanvas');
  const dpr = window.devicePixelRatio || 1;
  const W = canvas.clientWidth, H = canvas.clientHeight || 240;
  canvas.width = W * dpr; canvas.height = H * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, W, H);

  const g = cap.samples.map(v => v / 1000);
  const rate = cap.rate || 66;
  const isBack = cap.label === 'back';
  const isWorkout = isBack || cap.label === 'pullup';
  // quake captures are ||a|-1g| deviation, not raw |a| — plot as-is, no rep replay
  const det = isBack ? motionDetectBack(g, rate)
            : isWorkout ? motionDetectPull(g, rate)
            : { reps: [], armedAt: null };
  const cfg = isBack ? MOTION_BACK : MOTION_PULL;

  const lo = Math.min(0.6, Math.min(...g) - 0.05);
  const hi = Math.max(1.5, Math.max(...g) + 0.05);
  const X = t => (t / (g.length / rate * 1000)) * W;
  const Y = v => H - ((v - lo) / (hi - lo)) * H;

  // threshold guides
  const guide = (v, color, label) => {
    ctx.strokeStyle = color; ctx.setLineDash([4, 4]); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, Y(v)); ctx.lineTo(W, Y(v)); ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = color; ctx.font = '10px monospace';
    ctx.fillText(`${label} ${v.toFixed(2)}g`, 6, Y(v) - 3);
  };
  if (isWorkout) {
    guide(1.0, '#2A2A44', '');
    guide(cfg.trigger, '#60D080', 'trigger');
    if (!isBack) {
      guide(cfg.dip, '#A080FF', 'dip');
      guide(cfg.ceiling, '#E05060', 'ceiling');
    }
  }

  // stillness-gate marker
  if (det.armedAt !== null) {
    ctx.strokeStyle = '#3A6A8A'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(X(det.armedAt), 0); ctx.lineTo(X(det.armedAt), H); ctx.stroke();
    ctx.fillStyle = '#3A6A8A'; ctx.fillText('armed', X(det.armedAt) + 3, 12);
  }

  // waveform
  ctx.strokeStyle = '#50A8E8'; ctx.lineWidth = 1.2;
  ctx.beginPath();
  for (let i = 0; i < g.length; i++) {
    const x = X(i / rate * 1000), y = Y(g[i]);
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  }
  ctx.stroke();

  // detected reps
  ctx.fillStyle = '#FFD060';
  for (const r of det.reps) {
    const x = X(r.t);
    ctx.beginPath();
    ctx.moveTo(x, 6); ctx.lineTo(x - 5, 0); ctx.lineTo(x + 5, 0);
    ctx.closePath(); ctx.fill();
    ctx.fillRect(x - 0.5, 6, 1, H - 6);
  }

  const dur = g.length / rate;
  document.getElementById('motionStats').innerHTML = `
    <span><b>${escapeHtml(cap.label)}</b> · ${new Date(cap.at).toLocaleString()}</span>
    <span><b>${dur.toFixed(1)}</b> s · ${g.length} samples @ ${rate} Hz</span>
    <span>peak <b>${Math.max(...g).toFixed(2)}g</b> · min <b>${Math.min(...g).toFixed(2)}g</b></span>
    ${isWorkout ? `<span><b class="pullup-num">${det.reps.length}</b> reps detected</span>`
                : `<span>quake event capture (deviation from 1g, 3s pre-trigger)</span>`}`;
}

let motionFiles = [];

async function renderMotionFile(file) {
  const cap = await api('/motionlog/file/' + encodeURIComponent(file));
  drawMotion(cap);
}

async function loadMotionLab() {
  let list = [];
  try { list = await api('/motionlog/list'); } catch (_) {}
  const sel = document.getElementById('motionSelect');
  const panel = document.querySelector('.panel--motion');
  if (!list.length) { panel.style.display = 'none'; return; }
  panel.style.display = '';
  const prev = sel.value;
  const prevNewest = motionFiles[0];
  motionFiles = list.map(c => c.file);
  sel.innerHTML = '';
  for (const c of list) {
    const opt = document.createElement('option');
    opt.value = c.file;
    opt.textContent = `${c.label} · ${new Date(c.at).toLocaleString()} · ${(c.count / (c.rate || 66)).toFixed(0)}s`;
    sel.appendChild(opt);
  }
  // follow the newest capture unless the user is inspecting an older one
  const follow = !prev || prev === prevNewest || !motionFiles.includes(prev);
  sel.value = follow ? motionFiles[0] : prev;
  if (follow || prev !== sel.value) await renderMotionFile(sel.value);
}

document.getElementById('motionSelect').addEventListener('change', e => renderMotionFile(e.target.value));

// ---- Focus blocks --------------------------------------------------------------

async function loadFocus() {
  const { total, history } = await api('/focus');
  const now = new Date();
  const todayKey = walkDateKey(now);

  const summary = document.getElementById('focusSummary');
  const todayCount = history[todayKey] || 0;
  const dates = Object.keys(history).sort();
  summary.innerHTML = `<b class="focus-num">${total}</b> block${total === 1 ? '' : 's'} total` +
    (todayCount ? `  ·  <b class="focus-num">${todayCount}</b> today` : '') +
    (!todayCount && dates.length ? `  ·  last: ${dates[dates.length - 1]}` : '');

  const days = document.getElementById('focusDays');
  days.innerHTML = '';
  for (let i = 13; i >= 0; i--) {
    const key = walkDateKey(new Date(now.getTime() - i * 864e5));
    const n = history[key] || 0;
    const dot = document.createElement('div');
    dot.className = `sleep-dot${n > 0 ? ' focus-dot--done' : ''}`;
    dot.title = `${key}: ${n} block${n === 1 ? '' : 's'}`;
    days.appendChild(dot);
  }

  const cutoff30 = walkDateKey(new Date(now.getTime() - 29 * 864e5));
  let last30 = 0;
  for (const [key, n] of Object.entries(history)) if (key >= cutoff30) last30 += n;
  document.getElementById('focusStats').innerHTML = `
    <span><b>${last30}</b> last 30 days</span><span><b>${(last30 * 25 / 60).toFixed(1)} h</b> focused</span>` +
    nextTrophyLine(await getTrophies(), 'focus');
}

// ---- Trophies ----------------------------------------------------------------

const TROPHY_CATS = [
  ['habits',   'Habits'],
  ['strength', 'Strength'],
  ['walking',  'Walking'],
  ['focus',    'Focus'],
  ['sleep',    'Sleep'],
  ['pet',      'Pet'],
];

async function loadTrophies() {
  const trophies = await api('/trophies');
  const earned = trophies.filter(t => t.earned).length;
  document.getElementById('trophyTitle').textContent = `Trophies  ${earned} / ${trophies.length}`;

  const groups = document.getElementById('trophyGroups');
  groups.innerHTML = '';
  for (const [cat, label] of TROPHY_CATS) {
    const inCat = trophies.filter(t => t.cat === cat);
    if (inCat.length === 0) continue;
    const catEarned = inCat.filter(t => t.earned).length;

    const heading = document.createElement('div');
    heading.className = 'trophy-cat';
    heading.innerHTML = `${label} <span class="trophy-cat__count">${catEarned}/${inCat.length}</span>`;
    groups.appendChild(heading);

    const grid = document.createElement('div');
    grid.className = 'trophy-grid';
    for (const t of inCat) {
      const card = document.createElement('div');
      card.className = `trophy${t.earned ? ' trophy--earned' : ''}`;
      card.innerHTML = `
        <div class="trophy__name">${escapeHtml(t.name)}</div>
        <div class="trophy__desc">${escapeHtml(t.desc)}</div>`;
      card.title = t.earned ? 'earned!' : 'not earned yet';
      grid.appendChild(card);
    }
    groups.appendChild(grid);
  }
}

// ---- Config version + SSE push indicator ----------------------------------

function updateConfigDisplay(version, updatedAt) {
  const el = document.getElementById('configVersion');
  if (el) el.textContent = `v${version}`;
  const at = document.getElementById('configPushedAt');
  if (at) at.textContent = updatedAt ? timeAgo(updatedAt).replace('synced ', '') : '—';
}

async function loadConfigVersion() {
  const d = await api('/config-version');
  updateConfigDisplay(d.version, d.updatedAt);
}

function flashPushed() {
  const flag = document.getElementById('settingsSaved');
  if (!flag) return;
  flag.textContent = 'Pushed';
  flag.classList.add('show');
  setTimeout(() => { flag.textContent = 'Saved'; flag.classList.remove('show'); }, 2000);
}

// Open a persistent SSE connection so the dashboard reflects config bumps live
(function connectSSE() {
  const es = new EventSource('/api/events');
  es.addEventListener('config', e => {
    const { version, updatedAt } = JSON.parse(e.data);
    updateConfigDisplay(version, updatedAt);
  });
  es.onerror = () => setTimeout(connectSSE, 5000); // reconnect on drop
})();

// ---- Boot -----------------------------------------------------------------

async function refreshAll() {
  await Promise.all([loadPet(), loadHabits(), loadGoals(), loadQuests(), loadSettings(), loadHistory(), loadWalking(), loadSleep(), loadBackWorkouts(), loadPushUps(), loadPullUps(), loadFocus(), loadTrophies(), loadMotionLab()]);
}

refreshAll();
setInterval(loadPet, 15000);
// Steps and sleep arrive with every device sync (10 s on USB), so keep those
// panels fresh on the same cadence as the pet card.
setInterval(loadWalking, 15000);
setInterval(loadSleep, 15000);
setInterval(loadBackWorkouts, 15000);
setInterval(loadPushUps, 15000);
setInterval(loadPullUps, 15000);
setInterval(loadFocus, 15000);
setInterval(loadMotionLab, 15000);
