#pragma once
// Minimal Arduino.h shim for the CyberPet web simulator build.
// The hardware-agnostic logic files (pet, habits, ui) only use uint32_t,
// strncpy/snprintf, and min/max from the Arduino environment, so they
// compile unchanged against this shim under Emscripten or native gcc.
//
// Firmware-only files (storage.cpp, wifi_sync.cpp, CyberPet.ino) are NOT
// part of the web build - don't try to shim Preferences/WiFi/HTTPClient.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>

using std::min;
using std::max;
