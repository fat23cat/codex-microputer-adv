#pragma once

// save_settings() only marks the settings dirty; service() commits them once
// they have stayed unchanged for the debounce window. Call flush() instead
// when persistence must land before a risky action (transport teardown,
// reboot), and check its result.
namespace store {
void init();
void save_settings();
void load_settings();
void service();
bool flush();
} // namespace store
