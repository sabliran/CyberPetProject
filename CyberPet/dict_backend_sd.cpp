// Firmware implementation of dict.h's file-access seam: reads
// /dict/words.idx + /dict/defs.dat from the TF card. The card itself is
// mounted by the board sketch's SD_MMC init (phase 1) — this file only
// opens files on the already-mounted filesystem, so it stays pin-free.
//
// Firmware-only: excluded from the web_sim build exactly like storage.cpp /
// wifi_sync.cpp (never listed in web_sim/CMakeLists.txt; the sim links
// dict_backend_stdio.cpp instead).
// ⚠ Compile-verified only — untested on hardware until a later phase wires
// the UI up to it (SKILL.md: flag, don't guess).
#include "dict.h"
#include <FS.h>
#include <SD_MMC.h>

static File g_dictFiles[2];  // 0 = words.idx, 1 = defs.dat

bool dictOpen() {
  g_dictFiles[0] = SD_MMC.open("/dict/words.idx", FILE_READ);
  g_dictFiles[1] = SD_MMC.open("/dict/defs.dat", FILE_READ);
  if (!g_dictFiles[0] || !g_dictFiles[1]) {
    dictClose();
    return false;
  }
  return true;
}

bool dictReadAt(int file, uint32_t off, void* buf, uint32_t len) {
  if (file < 0 || file > 1 || !g_dictFiles[file]) return false;
  if (!g_dictFiles[file].seek(off)) return false;
  return g_dictFiles[file].read((uint8_t*)buf, len) == len;
}

void dictClose() {
  for (int i = 0; i < 2; i++) {
    if (g_dictFiles[i]) g_dictFiles[i].close();
  }
}
