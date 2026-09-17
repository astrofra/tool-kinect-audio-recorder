#include "gui_widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace ui {
void theme() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowRounding = 5;
    s.FrameRounding = 4;
    s.ChildRounding = 3;
    s.PopupRounding = 5;
    s.FrameBorderSize = 1;
    s.WindowBorderSize = 1;
    s.ScrollbarSize = 10;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(.105f, .12f, .132f, 1);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.12f, .14f, .155f, 1);
    s.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(Text);
    s.Colors[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(Muted);
    s.Colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(Border);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(.13f, .15f, .164f, 1);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.18f, .21f, .23f, 1);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(.21f, .25f, .28f, 1);
    s.Colors[ImGuiCol_Button] = s.Colors[ImGuiCol_FrameBg];
    s.Colors[ImGuiCol_ButtonHovered] = s.Colors[ImGuiCol_FrameBgHovered];
    s.Colors[ImGuiCol_ButtonActive] = s.Colors[ImGuiCol_FrameBgActive];
    s.Colors[ImGuiCol_Header] = ImVec4(.19f, .25f, .29f, 1);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(.23f, .31f, .36f, 1);
    s.Colors[ImGuiCol_CheckMark] = ImVec4(.27f, .64f, .96f, 1);
    s.Colors[ImGuiCol_PlotHistogram] = ImVec4(.22f, .6f, .95f, 1);
}
void text(ImVec2 p, const char *value, ImU32 color) {
    ImGui::GetWindowDrawList()->AddText(p, color, value);
}
void centered(ImVec2 p, float width, const char *value, ImU32 color) {
    p.x += (width - ImGui::CalcTextSize(value).x) * .5f;
    text(p, value, color);
}
void panel(ImVec2 p, ImVec2 size, const char *title, float scale) {
    auto *d = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    d->AddRectFilledMultiColor(p, q, IM_COL32(31, 36, 39, 255), IM_COL32(29, 34, 37, 255), IM_COL32(25, 30, 33, 255),
                               IM_COL32(27, 32, 35, 255));
    d->AddRect(p, q, Border, 3 * scale);
    if (title) {
        d->AddRectFilled(p, ImVec2(q.x, p.y + 37 * scale), IM_COL32(35, 40, 43, 180));
        d->AddLine(ImVec2(p.x, p.y + 37 * scale), ImVec2(q.x, p.y + 37 * scale), Border);
        text(ImVec2(p.x + 21 * scale, p.y + 8 * scale), title);
    }
}
bool transport(const char *label, int icon, ImVec2 size, bool enabled, bool selected, float scale) {
    const auto p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    const bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    auto *d = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    const bool record = icon == 1;
    const ImU32 top = record ? IM_COL32(102, 43, 44, 255) : IM_COL32(39, 45, 49, 255),
                bottom = record ? IM_COL32(77, 31, 33, 255) : IM_COL32(31, 37, 40, 255);
    d->AddRectFilledMultiColor(p, q, top, top, bottom, bottom);
    if (hover || held || selected)
        d->AddRectFilled(p, q, IM_COL32(255, 255, 255, held ? 20 : 10), 4 * scale);
    d->AddRect(p, q, record || selected ? Red : hover ? Muted : Border, 4 * scale, 0, (record ? 1.5f : 1) * scale);
    const ImU32 color = !enabled && !selected ? IM_COL32(104, 113, 121, 255) : record ? Red : Text;
    const ImVec2 c(p.x + size.x * .5f, p.y + 21 * scale);
    const float r = 10 * scale;
    if (icon == 0)
        d->AddTriangleFilled(ImVec2(c.x - r * .7f, c.y - r), ImVec2(c.x - r * .7f, c.y + r), ImVec2(c.x + r, c.y),
                             color);
    else if (icon == 1)
        d->AddCircleFilled(c, r, color, 32);
    else if (icon == 2)
        d->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color);
    else {
        d->AddRectFilled(ImVec2(c.x - r * .7f, c.y - r), ImVec2(c.x - r * .2f, c.y + r), color);
        d->AddRectFilled(ImVec2(c.x + r * .2f, c.y - r), ImVec2(c.x + r * .7f, c.y + r), color);
    }
    centered(ImVec2(p.x, p.y + 39 * scale), size.x, label, enabled || selected ? Text : Muted);
    if (ImGui::IsItemFocused())
        d->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(q.x + 2, q.y + 2), Muted, 5 * scale);
    ImGui::EndDisabled();
    return clicked;
}
bool gain_knob(double &db, float scale) {
    const auto p = ImGui::GetCursorScreenPos();
    const float size = 94 * scale;
    ImGui::InvisibleButton("Gain", ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const double before = db;
    auto &io = ImGui::GetIO();
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        db += (io.MouseDelta.x - io.MouseDelta.y) * .15 / scale;
    if (hovered) {
        db += io.MouseWheel;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            db = 0;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(
                "Gain du WAV et des vu-mètres\nGlisser ou molette ; double-clic : 0 dB\nRéglage fin avec la "
                "valeur sous le bouton");
    }
    if (ImGui::IsItemFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            db += .5;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            db -= .5;
    }
    db = std::max(-24.0, std::min(36.0, db));
    auto *d = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * .5f, p.y + size * .5f);
    d->AddCircleFilled(c, 45 * scale, IM_COL32(16, 21, 23, 255), 64);
    d->AddCircle(c, 45 * scale, active || hovered ? Muted : Border, 64, 2 * scale);
    d->AddCircleFilled(c, 38 * scale, IM_COL32(49, 55, 60, 255), 64);
    d->AddCircle(c, 37 * scale, IM_COL32(58, 64, 69, 255), 64, 2 * scale);
    const float angle = static_cast<float>(-1.570796327 + (db < 0 ? db / 24 : db / 36) * 2.35619449);
    d->AddLine(ImVec2(c.x + 25 * scale * std::cos(angle), c.y + 25 * scale * std::sin(angle)),
               ImVec2(c.x + 39 * scale * std::cos(angle), c.y + 39 * scale * std::sin(angle)), Text, 4 * scale);
    return db != before;
}
void meters(const recorder::RecorderStatus &status, unsigned channels, float height, float scale,
            std::vector<float> &held_peaks, std::vector<double> &clip_until) {
    if (held_peaks.size() != channels) {
        held_peaks.assign(channels, -60);
        clip_until.assign(channels, 0);
    }
    ImGui::BeginChild("Audio meters", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const auto p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, column = std::max(91 * scale, width / std::max(1U, channels));
    const float top = p.y + 36 * scale, bottom = p.y + height - 44 * scale, barh = bottom - top;
    auto *d = ImGui::GetWindowDrawList();
    const double now = ImGui::GetTime();
    for (unsigned c = 0; c < channels; ++c) {
        const float peak = c < status.peak.size() && !status.paused ? status.peak[c] : 0,
                    db = peak > 0 ? 20 * std::log10(peak) : -90;
        held_peaks[c] = std::max(db, held_peaks[c] - static_cast<float>(ImGui::GetIO().DeltaTime) * 22);
        if (peak >= 1)
            clip_until[c] = now + 2;
        const float x = p.x + column * c + column * .56f, barw = 30 * scale;
        d->AddRectFilled(ImVec2(x - 10 * scale, p.y), ImVec2(x + barw + 9 * scale, bottom + 8 * scale),
                         IM_COL32(16, 21, 23, 255), 3 * scale);
        d->AddRect(ImVec2(x - 10 * scale, p.y), ImVec2(x + barw + 9 * scale, bottom + 8 * scale), Border, 3 * scale);
        d->AddRectFilled(ImVec2(x, p.y + 7 * scale), ImVec2(x + barw, p.y + 23 * scale),
                         clip_until[c] > now ? Red : IM_COL32(60, 33, 35, 255), 3 * scale);
        for (int segment = 0; segment < 30; ++segment) {
            const float threshold = -60 + segment * 2.0f, y = bottom - (segment + 1) * barh / 30;
            const ImU32 color = threshold >= -6    ? IM_COL32(248, 82, 68, 255)
                                : threshold >= -18 ? IM_COL32(243, 219, 44, 255)
                                                   : IM_COL32(57, 216, 65, 255);
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + barw, y + barh / 30 - 1.5f * scale),
                             held_peaks[c] >= threshold ? color : IM_COL32(27, 34, 37, 255));
        }
        const int ticks[] = {0, -6, -12, -18, -24, -36, -48, -60};
        for (int tick : ticks) {
            const float y = top - tick / 60.0f * barh;
            char label[16];
            std::snprintf(label, sizeof(label), "%d", tick);
            text(ImVec2(x - 20 * scale - ImGui::CalcTextSize(label).x, y - ImGui::GetFontSize() * .5f), label, Muted);
            d->AddLine(ImVec2(x - 17 * scale, y), ImVec2(x - 8 * scale, y), Muted);
        }
        char label[20];
        std::snprintf(label, sizeof(label), "%u", c + 1);
        centered(ImVec2(x - 10 * scale, bottom + 15 * scale), barw + 20 * scale,
                 channels == 2 ? (c == 0 ? "L" : "R") : label);
    }
    ImGui::Dummy(ImVec2(column * channels, height - 5 * scale));
    ImGui::EndChild();
}
bool edit_text(const char *label, std::string &value) {
    char b[2048];
    std::snprintf(b, sizeof(b), "%s", value.c_str());
    if (!ImGui::InputText(label, b, sizeof(b)))
        return false;
    value = b;
    return true;
}
} // namespace ui
