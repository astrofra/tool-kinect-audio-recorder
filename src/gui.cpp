#include "recorder/recorder.h"
#include "recorder/platform.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void set_text(char* buffer, std::size_t size, const std::string& text) {
    std::snprintf(buffer, size, "%s", text.c_str());
}
void screenshot(GLFWwindow* window, const std::string& path) {
    int w, h; glfwGetFramebufferSize(window, &w, &h);
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    std::FILE* f = recorder::open_file(path, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) std::fwrite(rgb.data() + static_cast<std::size_t>(y) * w * 3, 1, w * 3, f);
    std::fclose(f);
}
void error_callback(int, const char* message) { std::cerr << "GLFW: " << message << '\n'; }
int run(int argc, char** argv) {
    bool smoke = false;
    std::string smoke_output;
    if (argc == 3 && std::string(argv[1]) == "--smoke-test") { smoke = true; smoke_output = argv[2]; }
    else if (argc != 1) throw std::runtime_error("Usage: audio_recorder [--smoke-test NEW_DIRECTORY]");
    glfwSetErrorCallback(error_callback);
    if (!glfwInit()) throw std::runtime_error("Cannot initialize GLFW");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (smoke) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(920, 690, "Kinect Audio Recorder", 0, 0);
    if (!window) { glfwTerminate(); throw std::runtime_error("Cannot create OpenGL 3.3 window"); }
    glfwMakeContextCurrent(window); glfwSwapInterval(smoke ? 0 : 1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = 0;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(24, 22); style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 12); style.FrameRounding = 4;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.065f, 0.08f, 0.105f, 1);
    float scale_x = 1, scale_y = 1; glfwGetWindowContentScale(window, &scale_x, &scale_y);
    style.ScaleAllSizes(scale_x); io.FontGlobalScale = scale_x;
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 330"))
        throw std::runtime_error("Cannot initialize ImGui backends");

    recorder::Recorder recorder;
    recorder::RecordOptions options;
    char output[2048]; set_text(output, sizeof(output), smoke ? smoke_output : recorder::default_take_path());
    int source_choice = 0, device_choice = 0, signal_choice = 0, channels = 1;
    int sample_rate = 48000;
    float frequency = 440, amplitude = 0.25f;
    double duration = 0;
    std::vector<recorder::AudioDevice> devices;
    std::vector<std::string> takes;
    std::string ui_error;
    bool was_active = false, smoke_started = false, close_after_stop = false;
    const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    int exit_code = 0;
    while (true) {
        glfwPollEvents();
        recorder::RecorderStatus status = recorder.status();
        if (glfwWindowShouldClose(window)) {
            if (status.active) { recorder.request_stop(); close_after_stop = true; glfwSetWindowShouldClose(window, GLFW_FALSE); }
            else break;
        }
        if (close_after_stop && !status.active) break;
        if (was_active && !status.active) { recorder.wait(); takes.push_back(status.output); }
        was_active = status.active;
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("Recorder", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("KINECT AUDIO RECORDER");
        ImGui::TextDisabled("Record a microphone or generate a test signal without audio hardware");
        ImGui::Separator();
        ImGui::BeginDisabled(status.active);
        ImGui::SetNextItemWidth(240);
        ImGui::Combo("Source", &source_choice, "Simulated audio\0Windows microphone (WASAPI)\0");
        if (source_choice == 0) {
            ImGui::SetNextItemWidth(240); ImGui::Combo("Signal", &signal_choice, "Tone + one-second markers\0Continuous sine wave\0");
            ImGui::SetNextItemWidth(150); ImGui::InputInt("Sample rate", &sample_rate, 1000);
            ImGui::SameLine(); ImGui::SetNextItemWidth(130); ImGui::SliderInt("Channels", &channels, 1, 2);
            ImGui::SetNextItemWidth(260); ImGui::SliderFloat("Tone (Hz)", &frequency, 20, 2000, "%.0f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(200); ImGui::SliderFloat("Amplitude", &amplitude, 0, 1, "%.2f");
            ImGui::TextDisabled("No input or output audio device is needed. Stereo uses different tones per channel.");
        } else {
            if (ImGui::Button("Refresh inputs")) {
                try { devices = recorder::enumerate_audio_devices(); device_choice = 0; ui_error.clear(); }
                catch (const std::exception& e) { ui_error = e.what(); }
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(440);
            const char* selected = device_choice == 0 ? "Default Windows input" : devices[device_choice - 1].name.c_str();
            if (ImGui::BeginCombo("Device", selected)) {
                if (ImGui::Selectable("Default Windows input", device_choice == 0)) device_choice = 0;
                for (std::size_t i = 0; i < devices.size(); ++i)
                    if (ImGui::Selectable(devices[i].name.c_str(), device_choice == static_cast<int>(i + 1))) device_choice = static_cast<int>(i + 1);
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Uses the device's native shared format (mono/stereo); no automatic source switching.");
        }
        ImGui::SetNextItemWidth(-120); ImGui::InputText("Take folder", output, sizeof(output));
        if (ImGui::Button("New take path")) set_text(output, sizeof(output), recorder::default_take_path());
        ImGui::SameLine(); ImGui::SetNextItemWidth(130); ImGui::InputDouble("Seconds (0 = until Stop)", &duration, 0, 0, "%.2f");
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(status.active);
        const bool record_pressed = ImGui::Button("Record", ImVec2(140, 42));
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(!status.active);
        if (ImGui::Button("Stop", ImVec2(140, 42))) recorder.request_stop();
        ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::Text("%s   %.2f s", status.state.c_str(), static_cast<double>(status.frames) / status.format.sample_rate);
        if (record_pressed || (smoke && !smoke_started)) {
            options.output = output; options.source = source_choice == 0 ? "simulate" : "wasapi";
            options.device_id = device_choice > 0 ? devices[device_choice - 1].id : "";
            options.signal = signal_choice == 0 ? "markers" : "sine";
            options.sample_rate = static_cast<unsigned>(sample_rate); options.channels = static_cast<unsigned>(channels);
            options.frequency = frequency; options.amplitude = amplitude; options.duration_seconds = smoke ? 0.25 : duration;
            if (source_choice != 0) {
                // Simulation controls must not constrain the microphone's native format.
                options.sample_rate = 48000; options.channels = 1;
                options.frequency = 440; options.amplitude = 0.25; options.signal = "markers";
            }
            try { recorder.start(options); ui_error.clear(); } catch (const std::exception& e) { ui_error = e.what(); }
            smoke_started = smoke;
        }
        for (unsigned c = 0; c < status.format.channels; ++c) {
            const float peak = status.peak[c];
            const float db = peak > 0 ? 20.0f * std::log10(peak) : -90.0f;
            char label[120]; std::snprintf(label, sizeof(label), "Channel %u   peak %.1f dBFS   RMS %.3f%s", c + 1, db, status.rms[c], peak >= 1 ? "   CLIPPING" : "");
            ImGui::ProgressBar(std::max(0.0f, std::min(1.0f, (db + 60) / 60)), ImVec2(-1, 28), label);
        }
        ImGui::Text("%u Hz / %u channel(s) / %llu packets / %llu invalid timestamps", status.format.sample_rate,
            status.format.channels, static_cast<unsigned long long>(status.packets), static_cast<unsigned long long>(status.timestamp_errors));
        if (!status.error.empty()) ImGui::TextWrapped("Recording error: %s", status.error.c_str());
        if (!ui_error.empty()) ImGui::TextWrapped("%s", ui_error.c_str());
        ImGui::Separator(); ImGui::TextUnformatted("Takes created in this session");
        for (std::size_t i = 0; i < takes.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Copy path")) ImGui::SetClipboardText(takes[i].c_str());
            ImGui::SameLine(); ImGui::TextUnformatted(takes[i].c_str()); ImGui::PopID();
        }
        ImGui::End(); ImGui::Render();
        int w, h; glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h); glClearColor(0.065f, 0.08f, 0.105f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (smoke && smoke_started && !status.active && status.state != "Idle") {
            screenshot(window, smoke_output + ".ppm");
            exit_code = status.error.empty() && ui_error.empty() ? 0 : 1;
            break;
        }
        glfwSwapBuffers(window);
        if (smoke && std::chrono::steady_clock::now() - began > std::chrono::seconds(10)) { exit_code = 1; break; }
    }
    recorder.request_stop(); recorder.wait();
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return exit_code;
}
}
int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
