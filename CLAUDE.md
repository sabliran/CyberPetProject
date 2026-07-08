Read SKILL.md at the repo root before making any changes and follow all its conventions.

Key points it enforces (read SKILL.md for full detail — it is the canonical reference):
- Hardware-agnostic logic files (pet, habits, storage, ui, wifi_sync) contain zero pin references; board-specific code lives only at the two marked integration points in CyberPet.ino.
- Habit sync reconciliation rules and truncation-aware name comparison (strncmp at HABIT_NAME_LEN - 1, never plain strcmp).
- NVS writes are change-guarded; call HabitTracker::recount() after restoring habits.
- store.js is the only storage seam on the dashboard; server routes never touch the JSON file directly.
- Logic files must keep compiling against web_sim/shim/Arduino.h.
- Flag anything board-specific or that can't be compile-verified off-device instead of guessing.
