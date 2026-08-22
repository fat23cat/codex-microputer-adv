// The running firmware version.
//
// The root CMakeLists.txt PROJECT_VER is the single source of truth; the OTA
// app descriptor carries it as `version`, so the diagnostic protocol, the boot
// probe, the splash stamp and the flashed image can never disagree. Every
// reader goes through here so no second copy can appear.
#pragma once

#include <esp_app_desc.h>

namespace firmware {

inline const char* version() { return esp_app_get_description()->version; }

}  // namespace firmware
