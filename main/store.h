#pragma once
namespace store {
void init();
void save_settings();
void load_settings();
void service();
bool flush();
} // namespace store
