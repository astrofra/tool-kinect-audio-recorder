#pragma once
#include "recorder/recorder.h"
#include <imgui.h>
namespace ui {
const ImU32 Text = IM_COL32(222, 227, 234, 255), Muted = IM_COL32(163, 173, 185, 255);
const ImU32 Border = IM_COL32(62, 70, 76, 255), Red = IM_COL32(255, 81, 80, 255);
void theme();
void text(ImVec2 at, const char *value, ImU32 color = Text);
void centered(ImVec2 at, float width, const char *value, ImU32 color = Text);
void panel(ImVec2 at, ImVec2 size, const char *title, float scale);
bool transport(const char *label, int icon, ImVec2 size, bool enabled, bool selected, float scale);
bool gain_knob(double &db, float scale);
void meters(const recorder::RecorderStatus &status, unsigned channels, float height, float scale,
            std::vector<float> &held_peaks, std::vector<double> &clip_until);
bool edit_text(const char *label, std::string &value);
} // namespace ui
