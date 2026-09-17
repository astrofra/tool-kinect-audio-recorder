#include "recorder/recorder.h"
#include "recorder/platform.h"
#include "recorder/settings.h"
#include "gui_widgets.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
std::string leaf(const std::string &path) {
    return path.substr(path.find_last_of("/\\") + 1);
}
std::string timecode(std::uint64_t frames, unsigned rate) {
    if (!rate)
        return "00:00:00:00";
    const auto seconds = frames / rate;
    char out[64];
    std::snprintf(out, sizeof(out), "%02llu:%02u:%02u:%02u", static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned>(seconds / 60 % 60), static_cast<unsigned>(seconds % 60),
                  static_cast<unsigned>(frames % rate * 30 / rate));
    return out;
}
std::string clock_text() {
    const std::time_t now = std::time(0);
    std::tm t = {};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char out[16];
    std::strftime(out, sizeof(out), "%H:%M:%S", &t);
    return out;
}
void open_external(const std::string &path) {
    if (!recorder::path_exists(path))
        throw std::runtime_error("Fichier indisponible : " + path);
#ifdef _WIN32
    const auto result = ShellExecuteW(0, L"open", recorder::from_utf8(path).c_str(), 0, 0, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
        throw std::runtime_error("Aucun lecteur associé à ce fichier. Ouvrez-le avec votre lecteur vidéo.");
#else
    throw std::runtime_error("Ouvrez ce fichier avec votre lecteur : " + path);
#endif
}
double free_gib(std::string path) {
#ifdef _WIN32
    for (;;) {
        ULARGE_INTEGER free = {};
        if (GetDiskFreeSpaceExW(recorder::from_utf8(path).c_str(), &free, 0, 0))
            return static_cast<double>(free.QuadPart) / (1024 * 1024 * 1024.0);
        const auto slash = path.find_last_of("/\\");
        if (slash == std::string::npos)
            break;
        if (slash == 2 && path[1] == ':') {
            path = path.substr(0, 3);
            if (GetDiskFreeSpaceExW(recorder::from_utf8(path).c_str(), &free, 0, 0))
                return static_cast<double>(free.QuadPart) / (1024 * 1024 * 1024.0);
            break;
        }
        path.resize(slash);
    }
#endif
    return -1;
}
void screenshot(GLFWwindow *window, const std::string &path) {
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    std::FILE *f = recorder::open_file(path, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
        std::fwrite(rgb.data() + static_cast<std::size_t>(y) * w * 3, 1, w * 3, f);
    std::fclose(f);
}
void error_callback(int, const char *message) {
    std::cerr << "GLFW: " << message << '\n';
}
void depth_image(const recorder::RecorderStatus &status, GLuint texture,
                 std::shared_ptr<const recorder::DepthFrame> &displayed, ImVec2 p, ImVec2 size, float scale) {
    auto *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(12, 17, 19, 255));
    if (status.depth_preview) {
        if (displayed != status.depth_preview) {
            std::vector<unsigned char> rgb;
            recorder::colorize_depth(status.depth_preview->millimetres, rgb);
            glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, recorder::DepthWidth, recorder::DepthHeight, 0, GL_RGB,
                         GL_UNSIGNED_BYTE, rgb.data());
            displayed = status.depth_preview;
        }
        const float factor = std::min(size.x / recorder::DepthWidth, size.y / recorder::DepthHeight);
        const ImVec2 image_size(recorder::DepthWidth * factor, recorder::DepthHeight * factor);
        const ImVec2 start(p.x + (size.x - image_size.x) * .5f, p.y + (size.y - image_size.y) * .5f);
        d->AddImage(static_cast<ImTextureID>(texture), start, ImVec2(start.x + image_size.x, start.y + image_size.y));
    } else {
        for (float x = 32 * scale; x < size.x; x += 32 * scale)
            d->AddLine(ImVec2(p.x + x, p.y), ImVec2(p.x + x, p.y + size.y), IM_COL32(25, 31, 34, 255));
        for (float y = 32 * scale; y < size.y; y += 32 * scale)
            d->AddLine(ImVec2(p.x, p.y + y), ImVec2(p.x + size.x, p.y + y), IM_COL32(25, 31, 34, 255));
        ui::centered(ImVec2(p.x, p.y + size.y * .5f - 18 * scale), size.x, "APERÇU DE PROFONDEUR", ui::Muted);
        ui::centered(ImVec2(p.x, p.y + size.y * .5f + 12 * scale), size.x, "Choisissez une entrée, puis Enregistrer",
                     ui::Muted);
    }
    d->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), ui::Border);
}
const char *state_label(const recorder::RecorderStatus &s) {
    if (s.paused)
        return "EN PAUSE";
    if (s.state == "Preparing")
        return "PRÉPARATION";
    if (s.state == "Finalizing")
        return "FINALISATION";
    if (s.active)
        return "ENREGISTREMENT";
    if (!s.error.empty())
        return "INTERROMPU";
    return s.state == "Idle" ? "PRÊT" : s.warnings ? "TERMINÉ AVEC ALERTES" : "PRISE TERMINÉE";
}
int run(int argc, char **argv) {
    bool smoke = false, kinect_smoke = false, wasapi_smoke = false, controls_smoke = false;
    std::string smoke_output, ini;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc)
            ini = argv[++i];
        else if ((arg == "--smoke-test" || arg == "--kinect-smoke-test" || arg == "--wasapi-smoke-test" ||
                  arg == "--controls-smoke-test") &&
                 i + 1 < argc) {
            smoke = true;
            kinect_smoke = arg == "--kinect-smoke-test";
            wasapi_smoke = arg == "--wasapi-smoke-test";
            smoke_output = argv[++i];
            controls_smoke = arg == "--controls-smoke-test";
        } else
            throw std::runtime_error(
                "Usage: audio_recorder [--config FILE.ini] "
                "[--smoke-test|--kinect-smoke-test|--wasapi-smoke-test|--controls-smoke-test NEW_DIRECTORY]");
    }
    if (ini.empty())
        ini = smoke ? smoke_output + ".ini" : recorder::path_join(recorder::executable_directory(), "recorder.ini");
    ini = recorder::absolute_path(ini);
    if (smoke)
        smoke_output = recorder::absolute_path(smoke_output);
    std::string load_warning;
    recorder::GuiSettings settings;
    bool settings_readable = true;
    try {
        settings = recorder::load_settings(ini, load_warning);
    } catch (const std::exception &e) {
        load_warning = e.what();
        settings_readable = false;
    }
    if (kinect_smoke)
        settings.recording.depth_pattern = "kinect";
    if (wasapi_smoke)
        settings.recording.source = "wasapi";
    auto &options = settings.recording;
    glfwSetErrorCallback(error_callback);
    if (!glfwInit())
        throw std::runtime_error("Cannot initialize GLFW");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (smoke)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    if (!smoke) {
        int work_width = 0, work_height = 0;
        if (GLFWmonitor *monitor = glfwGetPrimaryMonitor()) {
            glfwGetMonitorWorkarea(monitor, 0, 0, &work_width, &work_height);
            settings.window_width =
                std::min(settings.window_width, static_cast<unsigned>(std::max(960, work_width - 32)));
            settings.window_height =
                std::min(settings.window_height, static_cast<unsigned>(std::max(640, work_height - 64)));
        }
    }
    GLFWwindow *window = glfwCreateWindow(static_cast<int>(settings.window_width),
                                          static_cast<int>(settings.window_height), "Depth / Audio Recorder", 0, 0);
    if (!window) {
        glfwTerminate();
        throw std::runtime_error("Cannot create OpenGL 3.3 window");
    }
    glfwSetWindowSizeLimits(window, 960, 640, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (settings.maximized && !smoke)
        glfwMaximizeWindow(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(smoke ? 0 : 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = 0;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (controls_smoke)
        io.ConfigInputTrickleEventQueue = false;
    ui::theme();
    ImFont *body = 0;
    ImFont *mono = 0;
#ifdef _WIN32
    wchar_t win_dir[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(win_dir, MAX_PATH);
    if (length && length < MAX_PATH) {
        const std::string fonts = recorder::path_join(recorder::to_utf8(std::wstring(win_dir, length)), "Fonts");
        const std::string regular = recorder::path_join(fonts, "segoeui.ttf"),
                          counter = recorder::path_join(fonts, "segoeuib.ttf");
        if (recorder::path_exists(regular))
            body = io.Fonts->AddFontFromFileTTF(regular.c_str(), 19);
        if (recorder::path_exists(counter))
            mono = io.Fonts->AddFontFromFileTTF(counter.c_str(), 64);
    }
#endif
    if (!body)
        body = io.Fonts->AddFontDefaultVector();
    if (!mono)
        mono = body;
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 330"))
        throw std::runtime_error("Cannot initialize ImGui");
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    recorder::Recorder recorder;
    recorder.set_gain_db(options.gain_db);
    std::shared_ptr<const recorder::DepthFrame> displayed;
    std::vector<recorder::AudioDevice> devices;
    std::vector<std::string> journal;
    std::vector<float> held_peaks;
    std::vector<double> clip_until;
    auto log = [&](const std::string &message) {
        journal.push_back(clock_text() + "  —  " + message);
        if (journal.size() > 200)
            journal.erase(journal.begin());
    };
    std::string ui_error = load_warning;
    auto refresh = [&] {
        try {
            devices = recorder::enumerate_audio_devices();
        } catch (const std::exception &e) {
            ui_error = e.what();
            log(ui_error);
        }
    };
    refresh();
    if (!load_warning.empty())
        log(load_warning);
    bool show_settings = false, show_journal = false, show_takes = false, close_after_stop = false,
         smoke_started = false;
    bool was_active = false;
    std::string previous_state, preview_take = settings.last_take;
    std::uint64_t last_warnings = 0;
    std::size_t last_completed = 0, last_failed = 0;
    std::string saved = recorder::serialize_settings(settings), observed = saved;
    bool dirty = !recorder::path_exists(ini);
    double save_after = 0, free_after = 0, disk_free = -1;
    const auto began = std::chrono::steady_clock::now();
    int exit_code = 0;
    unsigned interaction = 0;
    double paused_at = 0;
    bool record_shot = false, pause_shot = false;
    log("Prêt à enregistrer");
    for (;;) {
        glfwPollEvents();
        const auto status = recorder.status();
        const auto encoding = recorder.encoding_status();
        if (glfwWindowShouldClose(window)) {
            recorder.request_stop();
            close_after_stop = true;
            glfwSetWindowShouldClose(window, GLFW_FALSE);
        }
        if (status.state != previous_state) {
            previous_state = status.state;
            log(state_label(status));
            if (!status.error.empty()) {
                ui_error = status.error;
                log(ui_error);
            }
        }
        if (was_active && !status.active) {
            recorder.wait();
            if (status.frames || status.depth_frames) {
                settings.last_take = status.output;
                preview_take = status.output;
                log("Prise : " + leaf(status.output));
            }
        }
        was_active = status.active;
        if (status.warnings != last_warnings) {
            last_warnings = status.warnings;
            if (status.warnings)
                log(status.last_warning);
        }
        if (encoding.completed != last_completed) {
            last_completed = encoding.completed;
            log("Encodage terminé");
        }
        if (encoding.failed != last_failed) {
            last_failed = encoding.failed;
            ui_error = encoding.last_error;
            log(ui_error);
        }
        if (close_after_stop && !status.active && !encoding.busy())
            break;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        // Hardware-free GUI regression: drive the same mouse widgets as a user.
        if (controls_smoke && ImGui::GetFrameCount() > 2) {
            int cw, ch;
            glfwGetWindowSize(window, &cw, &ch);
            const float scale = std::max(.62f, std::min(cw / 1536.0f, ch / 1024.0f));
            auto press = [&](float x, float y) {
                io.AddMousePosEvent(x, y);
                io.AddMouseButtonEvent(0, true);
            };
            const float pause_x = cw * .5f + 246 * scale, button_y = 176 * scale;
            switch (interaction) {
            case 0:
                press(cw * .5f - 82 * scale, button_y);
                ++interaction;
                break;
            case 1:
                io.AddMouseButtonEvent(0, false);
                ++interaction;
                break;
            case 2:
                if (status.frames >= status.format.sample_rate / 10) {
                    const float left = (cw - 54 * scale) * .665f, right = 36 * scale + left;
                    io.AddMousePosEvent(right + (cw - right - 18 * scale) * .5f, ch - 281 * scale);
                    ++interaction;
                }
                break;
            case 3:
                io.AddMouseWheelEvent(0, 12);
                ++interaction;
                break;
            case 4:
                if (status.frames >= status.format.sample_rate / 4) {
                    press(pause_x, button_y);
                    ++interaction;
                }
                break;
            case 5:
                io.AddMouseButtonEvent(0, false);
                paused_at = ImGui::GetTime();
                ++interaction;
                break;
            case 6:
                if (status.paused && ImGui::GetTime() - paused_at > .3) {
                    press(pause_x, button_y);
                    ++interaction;
                }
                break;
            case 7:
                io.AddMouseButtonEvent(0, false);
                ++interaction;
                break;
            case 8:
                if (!status.paused && status.frames >= status.format.sample_rate * 3 / 5) {
                    press(cw * .5f + 82 * scale, button_y);
                    ++interaction;
                }
                break;
            case 9:
                io.AddMouseButtonEvent(0, false);
                ++interaction;
                break;
            default:
                break;
            }
        }
        ImGui::NewFrame();
        const auto *view = ImGui::GetMainViewport();
        const ImVec2 origin = view->WorkPos;
        const float w = view->WorkSize.x, h = view->WorkSize.y;
        const float s = std::max(.62f, std::min(w / 1536.0f, h / 1024.0f));
        ImGui::PushFont(body, 24 * s);
        auto &style = ImGui::GetStyle();
        style.FramePadding = ImVec2(11 * s, 6 * s);
        style.ItemSpacing = ImVec2(10 * s, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize(view->WorkSize);
        ImGui::Begin("DepthAudioRecorder", 0,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();
        auto *draw = ImGui::GetWindowDrawList();
        auto at = [&](float x, float y) { return ImVec2(origin.x + x, origin.y + y); };
        auto cursor = [&](float x, float y) { ImGui::SetCursorScreenPos(at(x, y)); };
        draw->AddRectFilledMultiColor(origin, at(w, h), IM_COL32(29, 34, 37, 255), IM_COL32(24, 29, 32, 255),
                                      IM_COL32(26, 31, 34, 255), IM_COL32(30, 35, 38, 255));
        ui::text(at(24 * s, 8 * s), "DEPTH / AUDIO RECORDER");
        const std::string session = "Session : " + settings.session;
        ui::text(at(w - ImGui::CalcTextSize(session.c_str()).x - 24 * s, 8 * s), session.c_str(), ui::Muted);
        draw->AddLine(at(0, 41 * s), at(w, 41 * s), ui::Border);
        ImGui::PushFont(mono, 80 * s);
        const auto counter = timecode(status.frames, status.format.sample_rate);
        float counter_width = 8 * s * static_cast<float>(counter.size() - 1);
        for (char c : counter) {
            char glyph[] = {c, 0};
            counter_width += ImGui::CalcTextSize(glyph).x;
        }
        float counter_x = (w - counter_width) * .5f;
        for (char c : counter) {
            char glyph[] = {c, 0};
            ui::text(at(counter_x, 34 * s), glyph);
            counter_x += ImGui::CalcTextSize(glyph).x + 8 * s;
        }
        ImGui::PopFont();
        ui::centered(at(0, 111 * s), w, "30 fps · NDF", ui::Muted);
        const float indicator = w * .5f + 260 * s;
        draw->AddCircleFilled(at(indicator, 80 * s), 10 * s,
                              status.active ? (status.paused ? IM_COL32(242, 190, 74, 255) : ui::Red) : ui::Border, 32);
        ui::text(at(indicator + 24 * s, 67 * s), state_label(status),
                 status.active ? (status.paused ? IM_COL32(242, 190, 74, 255) : ui::Red) : ui::Muted);
        const float buttonw = 149 * s, gap = 15 * s, transportx = (w - 4 * buttonw - 3 * gap) * .5f;
        const auto last_take = recorder::settings_relative_path(ini, settings.last_take);
        const auto last_video = recorder::path_join(last_take, "video/preview-rgb.mkv");
        bool play_ready = false;
        try {
            play_ready = !last_take.empty() && recorder::path_exists(last_video);
        } catch (...) {
        }
        cursor(transportx, 145 * s);
        if (ui::transport("Lecture", 0, ImVec2(buttonw, 63 * s), play_ready && !status.active, false, s))
            try {
                open_external(last_video);
            } catch (const std::exception &e) {
                ui_error = e.what();
                log(ui_error);
            }
        cursor(transportx + buttonw + gap, 145 * s);
        const bool record_clicked =
            ui::transport("Enregistrer", 1, ImVec2(buttonw, 63 * s), !status.active && !close_after_stop,
                          status.active && !status.paused, s);
        cursor(transportx + 2 * (buttonw + gap), 145 * s);
        if (ui::transport("Arrêter", 2, ImVec2(buttonw, 63 * s), status.active, false, s))
            recorder.request_stop();
        cursor(transportx + 3 * (buttonw + gap), 145 * s);
        if (ui::transport(status.paused ? "Reprendre" : "Pause", 3, ImVec2(buttonw, 63 * s),
                          status.state == "Recording" || status.paused, status.paused, s))
            recorder.set_paused(!status.paused);

        const float margin = 18 * s, body_y = 217 * s, body_h = h - body_y - 192 * s, left_w = (w - 3 * margin) * .665f,
                    right_x = 2 * margin + left_w, right_w = w - right_x - margin;
        ui::panel(at(margin, body_y), ImVec2(left_w, body_h), "PROFONDEUR", s);
        ui::panel(at(right_x, body_y), ImVec2(right_w, body_h), "AUDIO", s);
        ui::text(at(margin + 21 * s, body_y + 51 * s), "Entrée vidéo", ui::Muted);
        const char *depth_names[] = {"Désactivée", "Simulation — Dégradé", "Simulation — Bruit", "Kinect v2 — Capteur"};
        const char *depth_ids[] = {"off", "gradient", "noise", "kinect"};
        int depth_index = 0;
        for (int i = 0; i < 4; ++i)
            if (options.depth_pattern == depth_ids[i])
                depth_index = i;
        cursor(margin + 144 * s, body_y + 45 * s);
        ImGui::SetNextItemWidth(std::min(345 * s, left_w - 166 * s));
        ImGui::BeginDisabled(status.active || close_after_stop);
        if (ImGui::Combo("##video", &depth_index, depth_names, 4))
            options.depth_pattern = depth_ids[depth_index];
        ImGui::EndDisabled();
        const ImVec2 imagepos = at(margin + 21 * s, body_y + 88 * s), imagesize(left_w - 42 * s, body_h - 128 * s);
        depth_image(status, texture, displayed, imagepos, imagesize, s);
        ui::text(at(margin + 21 * s, body_y + body_h - 32 * s),
                 "512 × 424  |  30 i/s  |  Profondeur 16 bits · aperçu RGB", ui::Muted);

        ui::text(at(right_x + 21 * s, body_y + 51 * s), "Entrée audio", ui::Muted);
        std::string audio_label = options.source == "simulate" ? "Simulation audio"
                                  : options.device_id.empty()  ? "Entrée Windows par défaut"
                                                               : settings.device_name;
        if (audio_label.empty())
            audio_label = "Entrée mémorisée (indisponible)";
        bool selected_found = options.device_id.empty();
        for (const auto &d : devices)
            if (options.source == "wasapi" && d.id == options.device_id) {
                audio_label = d.name;
                selected_found = true;
            }
        if (options.source == "wasapi" && !selected_found)
            audio_label += " (indisponible)";
        cursor(right_x + 139 * s, body_y + 46 * s);
        ImGui::SetNextItemWidth(right_w - 159 * s);
        ImGui::BeginDisabled(status.active || close_after_stop);
        if (ImGui::BeginCombo("##audio", audio_label.c_str())) {
            if (ImGui::Selectable("Simulation audio", options.source == "simulate"))
                options.source = "simulate";
            if (ImGui::Selectable("Entrée Windows par défaut",
                                  options.source == "wasapi" && options.device_id.empty())) {
                options.source = "wasapi";
                options.device_id.clear();
                settings.device_name.clear();
            }
            for (const auto &d : devices) {
                ImGui::PushID(d.id.c_str());
                if (ImGui::Selectable(d.name.c_str(), options.source == "wasapi" && options.device_id == d.id)) {
                    options.source = "wasapi";
                    options.device_id = d.id;
                    settings.device_name = d.name;
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("Actualiser les entrées"))
                refresh();
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        unsigned meter_channels =
            options.source == "simulate" && !status.active ? options.channels : status.format.channels;
        char format[100];
        const unsigned rate = status.source_name.empty() ? options.sample_rate : status.format.sample_rate;
        std::snprintf(format, sizeof(format), "%g kHz · %s", rate / 1000.0,
                      meter_channels == 1   ? "Mono"
                      : meter_channels == 2 ? "Stéréo"
                                            : "Multicanal");
        if (meter_channels > 2)
            std::snprintf(format, sizeof(format), "%g kHz · %u canaux", rate / 1000.0, meter_channels);
        ui::text(at(right_x + 21 * s, body_y + 86 * s),
                 status.source_name.empty() && options.source == "wasapi" ? "Format détecté au démarrage" : format,
                 ui::Muted);
        cursor(right_x + 25 * s, body_y + 117 * s);
        ImGui::BeginChild("Audio content", ImVec2(right_w - 50 * s, body_h - 132 * s), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ui::meters(status, meter_channels, body_h - 263 * s, s, held_peaks, clip_until);
        const float inner = ImGui::GetContentRegionAvail().x;
        const auto knobtop = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(knobtop.x + (inner - 94 * s) * .5f, knobtop.y));
        const auto knobpos = ImGui::GetCursorScreenPos();
        ui::text(ImVec2(knobpos.x - 51 * s, knobpos.y + 36 * s), "Gain");
        bool gain_changed = ui::gain_knob(options.gain_db, s);
        ImGui::SetCursorScreenPos(ImVec2(knobpos.x - 7 * s, knobpos.y + 94 * s));
        ImGui::SetNextItemWidth(108 * s);
        float gain = static_cast<float>(options.gain_db);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        if (ImGui::DragFloat("##gain_value", &gain, .1f, -24, 36, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp)) {
            options.gain_db = std::isfinite(gain) ? gain : 0;
            gain_changed = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (gain_changed)
            recorder.set_gain_db(options.gain_db);
        ImGui::EndChild();

        const float encoding_y = body_y + body_h + 15 * s;
        ui::panel(at(margin, encoding_y), ImVec2(w - 2 * margin, 46 * s), 0, s);
        ui::text(at(38 * s, encoding_y + 11 * s), "Encodage");
        const auto total = encoding.completed + encoding.failed + encoding.pending + (encoding.active ? 1 : 0);
        const float progress =
            total ? static_cast<float>(encoding.completed + encoding.failed) / static_cast<float>(total) : 0;
        cursor(151 * s, encoding_y + 15 * s);
        ImGui::ProgressBar(progress, ImVec2(510 * s, 17 * s), "");
        const std::string enc_label = encoding.active      ? "En cours — " + leaf(encoding.current_output)
                                      : encoding.failed    ? "Échec — ouvrir le journal"
                                      : encoding.completed ? "Terminé"
                                                           : "En attente d’une prise";
        ui::text(at(682 * s, encoding_y + 11 * s), enc_label.c_str(), encoding.failed ? ui::Red : ui::Muted);
        const std::string queue_text = std::to_string(encoding.pending) + " en attente";
        ui::text(at(w - 36 * s - ImGui::CalcTextSize(queue_text.c_str()).x, encoding_y + 11 * s), queue_text.c_str(),
                 ui::Muted);
        const float log_y = encoding_y + 59 * s;
        ui::panel(at(margin, log_y), ImVec2(w - 2 * margin, 42 * s), 0, s);
        cursor(30 * s, log_y + 4 * s);
        if (ImGui::Button("Journal", ImVec2(82 * s, 33 * s)))
            show_journal = true;
        draw->PushClipRect(at(122 * s, log_y), at(w - 30 * s, log_y + 42 * s), true);
        if (!journal.empty())
            ui::text(at(122 * s, log_y + 9 * s), journal.back().c_str(), !ui_error.empty() ? ui::Red : ui::Muted);
        draw->PopClipRect();
        const float footer = h - 55 * s;
        draw->AddLine(at(0, footer), at(w, footer), ui::Border);
        draw->AddCircleFilled(
            at(47 * s, footer + 24 * s), 9 * s,
            status.active ? status.paused ? IM_COL32(242, 190, 74, 255) : ui::Red : IM_COL32(108, 129, 133, 255), 32);
        ui::text(at(69 * s, footer + 12 * s), close_after_stop ? "Finalisation avant fermeture…"
                                              : status.active
                                                  ? status.paused ? "Capture en pause" : "Enregistrement en cours"
                                                  : state_label(status));
        cursor(w * .40f, footer + 7 * s);
        if (ImGui::Button("Paramètres"))
            show_settings = true;
        ImGui::SameLine();
        if (ImGui::Button("Prises"))
            show_takes = true;
        if (close_after_stop) {
            ImGui::SameLine();
            if (ImGui::Button("Garder ouvert"))
                close_after_stop = false;
        }
        if (ImGui::GetTime() >= free_after) {
            disk_free = free_gib(recorder::settings_relative_path(ini, options.output));
            free_after = ImGui::GetTime() + 5;
        }
        char space[100];
        if (disk_free < 0)
            std::snprintf(space, sizeof(space), "Espace disque indisponible");
        else
            std::snprintf(space, sizeof(space), "Disque : %.1f Gio libres", disk_free);
        ui::text(at(w - ImGui::CalcTextSize(space).x - 35 * s, footer + 12 * s), space, ui::Muted);

        if (record_clicked || (smoke && !controls_smoke && !smoke_started)) {
            try {
                auto capture = options;
                capture.session_name = settings.session;
                capture.output = smoke ? smoke_output : recorder::settings_relative_path(ini, options.output);
                capture.timestamped_output = !smoke;
                capture.fast = false;
                capture.encode_depth =
                    options.encode_depth && (options.depth_pattern == "gradient" || options.depth_pattern == "noise");
                capture.encode_preview = options.encode_preview && options.depth_pattern != "off";
                if (!capture.ffmpeg.empty())
                    capture.ffmpeg = recorder::settings_relative_path(ini, capture.ffmpeg);
                if (smoke)
                    capture.duration_seconds = kinect_smoke || wasapi_smoke || controls_smoke ? 3 : 0.25;
                if (capture.source == "wasapi") {
                    capture.sample_rate = 48000;
                    capture.channels = 1;
                    capture.frequency = 440;
                    capture.amplitude = .25;
                    capture.signal = "markers";
                }
                recorder.start(capture);
                ui_error.clear();
                last_warnings = 0;
                held_peaks.clear();
                log("Enregistrement : " + recorder.status().output);
            } catch (const std::exception &e) {
                ui_error = e.what();
                log(ui_error);
            }
            smoke_started = smoke;
        }
        ImGui::End();
        ImGui::PopFont();

        ImGui::PushFont(body, std::max(18.0f, 24 * s));
        if (show_settings) {
            ImGui::SetNextWindowSize(ImVec2(710, 600), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Paramètres", &show_settings)) {
                ImGui::TextWrapped("Sauvegarde automatique : %s", ini.c_str());
                ImGui::Separator();
                ImGui::BeginDisabled(status.active);
                ui::edit_text("Session", settings.session);
                ui::edit_text("Préfixe des prises", options.output);
                ImGui::TextDisabled("La date et l’heure sont ajoutées à chaque prise.");
                if (ImGui::InputDouble("Durée (s, 0 = jusqu’à Arrêter)", &options.duration_seconds, 0, 0, "%.2f"))
                    options.duration_seconds = std::isfinite(options.duration_seconds)
                                                   ? std::max(0.0, std::min(86400.0, options.duration_seconds))
                                                   : 0;
                int segment = static_cast<int>(options.segment_seconds);
                if (ImGui::InputInt("Rotation des fichiers (s)", &segment))
                    options.segment_seconds = static_cast<unsigned>(std::max(1, std::min(600, segment)));
                ImGui::Checkbox("Arrêter sur erreur de capture (strict)", &options.strict_capture);
                ImGui::Checkbox("Encoder les segments simulés en Matroska", &options.encode_depth);
                ImGui::Checkbox("Créer l’aperçu RGB après Arrêter", &options.encode_preview);
                ui::edit_text("FFmpeg (vide = fourni)", options.ffmpeg);
                if (ImGui::CollapsingHeader("Simulation audio")) {
                    int simulation_rate = static_cast<int>(options.sample_rate);
                    if (ImGui::InputInt("Fréquence d’échantillonnage", &simulation_rate))
                        options.sample_rate = static_cast<unsigned>(std::max(8000, std::min(192000, simulation_rate)));
                    int channels = static_cast<int>(options.channels);
                    if (ImGui::SliderInt("Canaux", &channels, 1, 2))
                        options.channels = static_cast<unsigned>(channels);
                    int signal = options.signal == "sine" ? 1 : 0;
                    if (ImGui::Combo("Signal", &signal, "Tonalité et repères\0Sinusoïde\0"))
                        options.signal = signal ? "sine" : "markers";
                    if (ImGui::InputDouble("Tonalité (Hz)", &options.frequency, 1, 100, "%.1f"))
                        options.frequency = std::isfinite(options.frequency)
                                                ? std::max(1.0, std::min(96000.0, options.frequency))
                                                : 440;
                    float amplitude = static_cast<float>(options.amplitude);
                    if (ImGui::SliderFloat("Amplitude", &amplitude, 0, 1))
                        options.amplitude = amplitude;
                }
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextWrapped("Gain logiciel : appliqué aux WAV sur tous les canaux. Les voyants rouges signalent "
                                   "un dépassement de 0 dBFS. Les améliorations audio Windows restent indépendantes.");
                ImGui::TextWrapped("Pause exclut l’audio et les images reçus pendant la pause. Les horodatages Kinect "
                                   "sont conservés ; l’aperçu RGB garde l’image précédente pendant cet intervalle.");
                if (ImGui::Button("Actualiser les entrées audio"))
                    refresh();
            }
            ImGui::End();
        }
        if (show_journal) {
            ImGui::SetNextWindowSize(ImVec2(1000, 430), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Journal de session", &show_journal)) {
                for (const auto &line : journal)
                    ImGui::TextWrapped("%s", line.c_str());
                if (!status.source_name.empty())
                    ImGui::TextWrapped("Entrée utilisée : %s", status.source_name.c_str());
                if (!status.output.empty())
                    ImGui::TextWrapped("Prise : %s", status.output.c_str());
            }
            ImGui::End();
        }
        if (show_takes) {
            ImGui::SetNextWindowSize(ImVec2(800, 300), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Prises et prévisualisation", &show_takes)) {
                ui::edit_text("Dossier de prise", preview_take);
                if (ImGui::Button("Utiliser pour Lecture"))
                    settings.last_take = preview_take;
                ImGui::SameLine();
                if (ImGui::Button("Copier le chemin"))
                    ImGui::SetClipboardText(preview_take.c_str());
                ImGui::BeginDisabled(status.active || encoding.busy());
                if (ImGui::Button("Exporter l’aperçu RGB"))
                    try {
                        recorder.export_preview(recorder::settings_relative_path(ini, preview_take));
                        settings.last_take = preview_take;
                        ui_error.clear();
                    } catch (const std::exception &e) {
                        ui_error = e.what();
                        log(ui_error);
                    }
                ImGui::EndDisabled();
                ImGui::TextWrapped("Lecture ouvre video/preview-rgb.mkv avec le lecteur associé. Cet aperçu de "
                                   "profondeur est sans son.");
                if (!ui_error.empty())
                    ImGui::TextWrapped("%s", ui_error.c_str());
            }
            ImGui::End();
        }
        ImGui::PopFont();
        int width, height;
        glfwGetWindowSize(window, &width, &height);
        if (!smoke) {
            settings.maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
            if (!settings.maximized && !glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                settings.window_width = std::max(960, width);
                settings.window_height = std::max(640, height);
            }
        }
        const auto current = recorder::serialize_settings(settings);
        if (current != observed) {
            observed = current;
            dirty = current != saved;
            save_after = ImGui::GetTime() + .6;
        }
        if (dirty && settings_readable && ImGui::GetTime() >= save_after)
            try {
                recorder::save_settings(ini, settings);
                saved = current;
                dirty = false;
            } catch (const std::exception &e) {
                ui_error = e.what();
                log(ui_error);
                save_after = ImGui::GetTime() + 5;
            }
        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(.1f, .12f, .13f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (controls_smoke && interaction == 4 && !record_shot) {
            screenshot(window, smoke_output + "-recording.ppm");
            record_shot = true;
        }
        if (controls_smoke && status.paused && !pause_shot) {
            screenshot(window, smoke_output + "-paused.ppm");
            pause_shot = true;
        }
        if (smoke && smoke_started && !status.active && !encoding.busy() && status.state != "Idle") {
            screenshot(window, smoke_output + ".ppm");
            exit_code =
                status.error.empty() && ui_error.empty() && !encoding.failed && (!controls_smoke || interaction == 10)
                    ? 0
                    : 1;
            break;
        }
        if (smoke && smoke_started && !status.active && !ui_error.empty() && status.state == "Idle") {
            exit_code = 1;
            break;
        }
        glfwSwapBuffers(window);
        if (smoke)
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        if (smoke && std::chrono::steady_clock::now() - began > std::chrono::seconds(kinect_smoke ? 30 : 20)) {
            std::cerr << "GUI smoke timeout: phase=" << interaction << ", state=" << status.state
                      << ", gain=" << options.gain_db << '\n';
            exit_code = 1;
            break;
        }
    }
    recorder.request_stop();
    recorder.wait();
    if (settings_readable && (dirty || recorder::serialize_settings(settings) != saved))
        try {
            recorder::save_settings(ini, settings);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            exit_code = 1;
        }
    glDeleteTextures(1, &texture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return exit_code;
}
} // namespace
int main(int argc, char **argv) {
    try {
#ifdef _WIN32
        int count = 0;
        LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide)
            throw std::runtime_error("Cannot read command line");
        std::vector<std::string> values;
        for (int i = 0; i < count; ++i)
            values.push_back(recorder::to_utf8(wide[i]));
        LocalFree(wide);
        std::vector<char *> args;
        for (auto &value : values)
            args.push_back(const_cast<char *>(value.c_str()));
        (void)argc;
        (void)argv;
        return run(count, args.data());
#else
        return run(argc, argv);
#endif
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
