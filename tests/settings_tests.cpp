#include "recorder/settings.h"
#include "recorder/platform.h"
#include <iostream>
#include <stdexcept>
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        using namespace recorder;
        const std::string root = "test-settings-" + std::to_string(clock_ticks());
        create_new_directory(root);
        const auto path = path_join(root, "recorder.ini");
        std::string warning;
        auto s = load_settings(path, warning);
        require(s.recording.gain_db == 0 && warning.empty(), "Missing INI uses defaults");
        s.session = "Interview \"A\"; #1";
        s.recording.output = "C:\\takes\\prise-\xc3\xa9";
        s.recording.source = "wasapi";
        s.recording.device_id = "{0.0.1.0}.{opaque-endpoint}";
        s.device_name = "Microphone \xc3\xa9";
        s.recording.depth_pattern = "kinect";
        s.recording.gain_db = 30.5;
        s.recording.frequency = 333.5;
        s.recording.amplitude = .125;
        s.recording.channels = 2;
        s.recording.sample_rate = 44100;
        s.recording.duration_seconds = 123.25;
        s.recording.segment_seconds = 7;
        s.recording.strict_capture = true;
        s.recording.encode_depth = false;
        s.recording.encode_preview = true;
        s.recording.ffmpeg = "encoder folder/ffmpeg.exe";
        s.last_take = "D:\\a & b\\take";
        s.window_width = 1200;
        s.window_height = 800;
        s.maximized = true;
        s.extra["future.setting"] = "\"keep this\"";
        save_settings(path, s);
        auto loaded = load_settings(path, warning);
        require(warning.empty() && serialize_settings(s) == serialize_settings(loaded),
                "All settings and unknown keys round-trip without loss");
        require(loaded.recording.device_id == s.recording.device_id,
                "Endpoint ID retained rather than enumeration index");
        write_atomic(path,
                     "\xef\xbb\xbf[audio]\ngain_db=nan\nsource=typo\ndevice_id=\"missing but "
                     "retained\"\n[ui]\nwindow_width=-1\n[video]\nsource=kinect\n[encoding]\nrgb_preview=false\n");
        loaded = load_settings(path, warning);
        require(!warning.empty() && loaded.recording.gain_db == 0 && loaded.recording.source == "simulate" &&
                    loaded.window_width == 1536,
                "Malformed individual values do not corrupt defaults");
        require(loaded.recording.device_id == "missing but retained" && loaded.recording.depth_pattern == "kinect" &&
                    !loaded.recording.encode_preview,
                "Valid selections survive other malformed values and BOM");
        require(settings_relative_path("C:/app/recorder.ini", "recordings/take") == "C:/app/recordings/take",
                "Relative takes use config directory");
        require(settings_relative_path("C:/app/recorder.ini", "D:/takes") == "D:/takes", "Absolute path retained");
        std::cout
            << "INI settings, Unicode, endpoint identity, malformed values, unknown keys and path anchoring passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
