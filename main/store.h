// Persistent device state in the shared `apps_nvs` partition.
//
// The companion owns exactly one namespace there and never erases the
// partition, because M5Apps and its other apps share it.
#pragma once

#include <cstdint>

namespace store {

void init();

// Settings mirrored from model::state. Saved lazily: callers just mark dirty
// and service() commits at most once every few seconds.
void save_settings();
void load_settings();
void service();
void flush();

}  // namespace store
