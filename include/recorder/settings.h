#pragma once
#include "recorder/audio.h"
#include <map>

namespace recorder {
struct GuiSettings {
    RecordOptions recording;
    std::string session, device_name, last_take;
    unsigned window_width, window_height;
    bool maximized;
    // Retain unknown keys when a newer/hand-edited INI is saved.
    std::map<std::string, std::string> extra;
    GuiSettings();
};
GuiSettings load_settings(const std::string &path, std::string &warning);
std::string serialize_settings(const GuiSettings &settings);
void save_settings(const std::string &path, const GuiSettings &settings);
// Relative recording/encoder paths are relative to the INI, not the launch directory.
std::string settings_relative_path(const std::string &ini, const std::string &path);
std::string executable_directory();
} // namespace recorder
