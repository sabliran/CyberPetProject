#include "dict.h"
#include <string.h>

// Dictionary lookup (see dict.h). Pure logic over the dictOpen/dictReadAt/
// dictClose seam — no Arduino APIs, compiles in the web_sim build unchanged.

// defs.dat text buffer: sized by the generator-measured largest record
// (DICT_MAX_RECORD_LEN, currently the merged atomic_number_* entry) + NUL.
// Static, single-threaded use only — dictReadEntry results alias it.
static char textBuf[DICT_MAX_RECORD_LEN + 1];
static bool ready = false;

static uint32_t rdU32(const uint8_t* p) {  // uint32 LE, alignment-safe
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool readRecord(uint32_t index, uint8_t rec[DICT_RECORD_SIZE]) {
  return dictReadAt(0, index * (uint32_t)DICT_RECORD_SIZE, rec, DICT_RECORD_SIZE);
}

// Same rule as the generator's make_key(): lowercase, keep a-z, drop the
// rest (spaces, hyphens, apostrophes, digits). No transliteration — device
// input is ASCII. Returns the letter count; out is NUL-padded to key width.
static int normalizeKey(const char* in, char out[DICT_KEY_LEN]) {
  int n = 0;
  for (const char* p = in; *p && n < DICT_KEY_LEN; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c >= 'a' && c <= 'z') out[n++] = c;
  }
  for (int i = n; i < DICT_KEY_LEN; i++) out[i] = '\0';
  return n;
}

// Key must be a-z up to the first NUL, all-NUL padding after, >= 2 letters.
static bool keyWellFormed(const uint8_t* key) {
  int n = 0;
  while (n < DICT_KEY_LEN && key[n] >= 'a' && key[n] <= 'z') n++;
  if (n < 2) return false;
  for (int i = n; i < DICT_KEY_LEN; i++)
    if (key[i] != '\0') return false;
  return true;
}

bool dictInit() {
  if (ready) return true;
  if (!dictOpen()) return false;
  // Sanity-check the two ends of the index: well-formed keys in ascending
  // order, first defs offset 0, and both defs.dat records parseable.
  uint8_t first[DICT_RECORD_SIZE], last[DICT_RECORD_SIZE];
  bool ok = readRecord(0, first) && readRecord(DICT_WORD_COUNT - 1, last) &&
            keyWellFormed(first) && keyWellFormed(last) &&
            memcmp(first, last, DICT_KEY_LEN) < 0 &&
            rdU32(first + DICT_KEY_LEN) == 0;
  if (ok) {
    ready = true;  // dictReadEntry refuses while !ready
    DictEntry probe;
    ok = dictReadEntry(0, &probe) && dictReadEntry(DICT_WORD_COUNT - 1, &probe);
  }
  if (!ok) {
    ready = false;
    dictClose();
  }
  return ready;
}

int dictExact(const char* key) {
  if (!ready || !key) return -1;
  char want[DICT_KEY_LEN];
  if (normalizeKey(key, want) < 2) return -1;  // sub-2-letter keys were dropped
  uint32_t lo = 0, hi = DICT_WORD_COUNT;       // [lo, hi)
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint8_t rec[DICT_RECORD_SIZE];
    if (!readRecord(mid, rec)) return -1;
    int cmp = memcmp(rec, want, DICT_KEY_LEN);
    if (cmp == 0) return (int)mid;
    if (cmp < 0) lo = mid + 1;
    else         hi = mid;
  }
  return -1;
}

// First index whose key compares >= (lower) or > (upper) the prefix on its
// first plen bytes. NUL padding sorts before 'a', so shorter keys land left.
static uint32_t prefixBound(const char* prefix, int plen, bool upper) {
  uint32_t lo = 0, hi = DICT_WORD_COUNT;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint8_t rec[DICT_RECORD_SIZE];
    if (!readRecord(mid, rec)) return 0;  // I/O failure -> empty range
    int cmp = memcmp(rec, prefix, plen);
    if (upper ? cmp <= 0 : cmp < 0) lo = mid + 1;
    else                            hi = mid;
  }
  return lo;
}

void dictPrefixRange(const char* prefix, uint32_t* first, uint32_t* count) {
  *first = 0;
  *count = 0;
  if (!ready || !prefix) return;
  char norm[DICT_KEY_LEN];
  int plen = normalizeKey(prefix, norm);
  if (plen == 0) return;
  uint32_t lo = prefixBound(norm, plen, false);
  uint32_t hi = prefixBound(norm, plen, true);
  *first = lo;
  if (hi > lo) *count = hi - lo;
}

bool dictReadEntry(uint32_t index, DictEntry* out) {
  if (!ready || !out || index >= DICT_WORD_COUNT) return false;
  uint8_t rec[DICT_RECORD_SIZE];
  if (!readRecord(index, rec)) return false;
  uint32_t off = rdU32(rec + DICT_KEY_LEN);
  uint32_t len = rdU32(rec + DICT_KEY_LEN + 4);
  if (len == 0 || len > DICT_MAX_RECORD_LEN) return false;
  if (!dictReadAt(1, off, textBuf, len)) return false;
  textBuf[len] = '\0';

  out->display[0] = '\0';
  out->senseCount = 0;
  bool haveDisplay = false;
  char* line = textBuf;
  while (line && *line) {
    char* nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (!haveDisplay) {
      strncpy(out->display, line, DICT_DISPLAY_LEN - 1);
      out->display[DICT_DISPLAY_LEN - 1] = '\0';
      haveDisplay = true;
    } else {
      if (out->senseCount >= DICT_MAX_SENSES) break;  // giant merged entries
      char* bar = strchr(line, '|');
      if (bar) {  // "pos|definition"
        *bar = '\0';
        DictSense* s = &out->senses[out->senseCount++];
        strncpy(s->pos, line, sizeof(s->pos) - 1);
        s->pos[sizeof(s->pos) - 1] = '\0';
        s->def = bar + 1;
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  return haveDisplay;
}
