#include "recorder/recorder.h"
#include "recorder/platform.h"
#include <chrono>
#include <cmath>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
#ifdef _WIN32
std::atomic<bool> console_stop(false);
BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { console_stop = true; return TRUE; }
    return FALSE;
}
#endif
double number(const std::string& text) {
    std::size_t end = 0;
    double value = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(value)) throw std::invalid_argument("Invalid number: " + text);
    return value;
}
unsigned integer(const std::string& text) {
    double v = number(text);
    if (v < 0 || v > std::numeric_limits<unsigned>::max() || v != std::floor(v))
        throw std::invalid_argument("Invalid unsigned integer: " + text);
    return static_cast<unsigned>(v);
}
void usage() {
    std::cout << "Audio recorder (C++11)\n\n"
        "  recording_tool devices\n"
        "  recording_tool record [options]\n\n"
        "  --source simulate|wasapi  Default: simulate\n"
        "  --output DIRECTORY       New take directory (never overwritten)\n"
        "  --duration SECONDS       Default: 10; 0 records until Ctrl+C\n"
        "  --device ID              WASAPI endpoint ID; default input if omitted\n"
        "  --sample-rate HZ         Simulation: 8000..192000, default 48000\n"
        "  --channels 1|2           Simulation only; WASAPI uses native mix format\n"
        "  --signal markers|sine    Simulation, default markers\n"
        "  --frequency HZ           Simulation base tone, default 440\n"
        "  --amplitude 0..1         Simulation peak scale, default 0.25\n"
        "  --segment-seconds N      WAV rotation interval, default 60\n"
        "  --fast                   Unpaced simulation, requires finite duration\n";
}
int run(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "--help" || args[1] == "help") { usage(); return 0; }
    if (args[1] == "devices") {
        if (args.size() != 2) throw std::invalid_argument("devices takes no arguments");
        const std::vector<recorder::AudioDevice> devices = recorder::enumerate_audio_devices();
        if (devices.empty()) std::cout << "No active microphone endpoints. Simulation needs no audio hardware.\n";
        for (std::size_t i = 0; i < devices.size(); ++i) std::cout << devices[i].name << "\n  " << devices[i].id << '\n';
        return 0;
    }
    if (args[1] != "record") throw std::invalid_argument("Unknown command: " + args[1]);
    recorder::RecordOptions o; o.output = recorder::default_take_path(); o.duration_seconds = 10;
    bool simulation_options = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string key = args[i];
        if (key == "--help") { usage(); return 0; }
        if (key == "--fast") { o.fast = true; continue; }
        if (++i == args.size()) throw std::invalid_argument("Missing value for " + key);
        const std::string& v = args[i];
        if (key == "--output") o.output = v;
        else if (key == "--source") o.source = v;
        else if (key == "--duration") o.duration_seconds = number(v);
        else if (key == "--device") o.device_id = v;
        else if (key == "--segment-seconds") o.segment_seconds = integer(v);
        else if (key == "--sample-rate") { o.sample_rate = integer(v); simulation_options = true; }
        else if (key == "--channels") { o.channels = integer(v); simulation_options = true; }
        else if (key == "--signal") { o.signal = v; simulation_options = true; }
        else if (key == "--frequency") { o.frequency = number(v); simulation_options = true; }
        else if (key == "--amplitude") { o.amplitude = number(v); simulation_options = true; }
        else throw std::invalid_argument("Unknown option: " + key);
    }
    if (o.source == "wasapi" && simulation_options) throw std::invalid_argument("WASAPI records its native mix format; simulation controls do not apply");
    if (o.source == "simulate" && !o.device_id.empty()) throw std::invalid_argument("--device applies only to WASAPI");
    std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#endif
    recorder::Recorder recorder;
    recorder.start(o);
    std::cout << "Recording " << o.source << " to " << o.output << " (Ctrl+C stops and finalizes)\n";
    do {
        if (interrupted) recorder.request_stop();
#ifdef _WIN32
        if (console_stop) recorder.request_stop();
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (recorder.status().active);
    recorder.wait();
    const recorder::RecorderStatus s = recorder.status();
    std::cout << s.state << ": " << s.frames << " sample frames, " << s.format.channels << " channel(s), "
              << s.format.sample_rate << " Hz, " << std::fixed << std::setprecision(3)
              << static_cast<double>(s.frames) / s.format.sample_rate << " seconds\n";
    if (!s.error.empty()) { std::cerr << s.error << '\n'; return 1; }
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
#ifdef _WIN32
        int count = 0;
        LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide) throw std::runtime_error("Cannot read command line");
        try { for (int i = 0; i < count; ++i) args.push_back(recorder::to_utf8(wide[i])); }
        catch (...) { LocalFree(wide); throw; }
        LocalFree(wide);
        (void)argc; (void)argv;
#else
        for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
#endif
        return run(args);
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 2; }
}
