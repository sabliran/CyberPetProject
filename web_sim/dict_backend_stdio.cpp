// Sim implementation of dict.h's file-access seam: plain stdio against the
// generated files in tools/dict_out. The default path assumes the documented
// run location (`cd web_sim && ./build_native/cyberpet_sim`); override with
// the DICT_DIR env var when running from anywhere else.
#include "dict.h"
#include <stdio.h>
#include <stdlib.h>

static FILE* g_dictFiles[2] = {NULL, NULL};  // 0 = words.idx, 1 = defs.dat

bool dictOpen() {
  const char* dir = getenv("DICT_DIR");
  if (!dir || !*dir) dir = "../tools/dict_out";
  const char* names[2] = {"words.idx", "defs.dat"};
  for (int i = 0; i < 2; i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    g_dictFiles[i] = fopen(path, "rb");
    if (!g_dictFiles[i]) {
      fprintf(stderr, "dict: cannot open %s (set DICT_DIR?)\n", path);
      dictClose();
      return false;
    }
  }
  return true;
}

bool dictReadAt(int file, uint32_t off, void* buf, uint32_t len) {
  if (file < 0 || file > 1 || !g_dictFiles[file]) return false;
  if (fseek(g_dictFiles[file], (long)off, SEEK_SET) != 0) return false;
  return fread(buf, 1, len, g_dictFiles[file]) == len;
}

void dictClose() {
  for (int i = 0; i < 2; i++) {
    if (g_dictFiles[i]) fclose(g_dictFiles[i]);
    g_dictFiles[i] = NULL;
  }
}
