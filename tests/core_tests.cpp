#include "recorder/audio.h"
#include "recorder/platform.h"
#include "recorder/recorder.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

class FaultSource : public recorder::AudioSource {
public:
    explicit FaultSource(int mode) : mode_(mode), index_(0), origin_(0), failed_(false) {}
    recorder::AudioFormat open() { return recorder::AudioFormat(48000, 1); }
    unsigned max_packet_frames() const { return 480; }
    std::string name() const { return "fault-test"; }
    std::string device_id() const { return "fault-test"; }
    void start() { origin_ = recorder::clock_100ns(); }
    bool read(recorder::AudioPacket& p, const std::atomic<bool>& stop) {
        if (stop) return false;
        if (mode_ == 2 && index_ == 3) throw std::runtime_error("Injected source failure");
        if (mode_ == 3 && index_ == 1 && !failed_) { failed_ = true; throw std::runtime_error("Injected temporary audio failure"); }
        p.samples.assign(480, 0.1f);
        p.device_frame = index_ * 480 + (mode_ == 0 && index_ > 0 ? 480 : 0);
        p.timestamp_100ns = origin_ + recorder::frames_to_100ns(p.device_frame, 48000);
        p.receipt_ticks = recorder::clock_ticks();
        p.flags = index_ == 0 ? recorder::Discontinuity : 0; // Startup flag is not a mid-take gap.
        if (mode_ == 1 && index_ == 1) p.flags |= recorder::TimestampError;
        ++index_;
        return true;
    }
    void stop() {}
private:
    int mode_;
    unsigned index_;
    std::uint64_t origin_;
    bool failed_;
};

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        using namespace recorder;
        require(frames_to_100ns(44100, 44100) == 10000000, "44.1 kHz conversion");
        require(frames_to_100ns(48000ULL * 86400, 48000) == 864000000000ULL, "Long clock conversion");
        require(depth_frames_for_audio(0, 48000) == 0, "No depth without audio");
        require(depth_frames_for_audio(1, 48000) == 1, "Depth frame at timeline zero");
        require(depth_frames_for_audio(1600, 48000) == 1, "Half-open exact frame boundary");
        require(depth_frames_for_audio(1601, 48000) == 2, "Depth frame after boundary");
        require(depth_frames_for_audio(8000, 8000) == 30, "Non-divisible sample/frame cadence");
        require(depth_frames_for_audio(48000ULL * 86400, 48000) == 2592000, "Day-long depth clock");
        require(json_string("a\n\"\\\t") == "\"a\\u000a\\\"\\\\\\u0009\"", "JSON escaping");
        RecordOptions o; o.output = "unused"; o.signal = "sine"; o.frequency = 1000; o.amplitude = 0.5;
        require(std::abs(simulated_sample(o, 12, 0) - 0.5f) < 1e-6f, "Sine quarter cycle");
        require(std::abs(simulated_sample(o, 36, 0) + 0.5f) < 1e-6f, "Sine negative quarter cycle");
        o.signal = "markers";
        for (unsigned i = 0; i < 100000; ++i) require(std::abs(simulated_sample(o, i, 0)) <= 0.5f, "Signal amplitude bound");
        bool rejected = false; o.amplitude = 2;
        try { validate_options(o); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Amplitude validation");
        o.amplitude = 0.25; o.depth_pattern = "gradient"; o.output = "test-stop-" + std::to_string(clock_ticks());
        Recorder recorder; recorder.start(o);
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (recorder.status().frames < 480 && recorder.status().active && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(recorder.status().frames >= 480, "Realtime simulation produced samples");
        rejected = false;
        try { recorder.start(o); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Cannot start twice");
        recorder.request_stop(); recorder.wait();
        require(!recorder.status().active && recorder.status().state == "Complete", "Stop drains and finalizes");
        require(recorder.status().depth_frames == depth_frames_for_audio(recorder.status().frames, 48000), "Stop keeps depth and audio aligned");
        require(recorder.status().depth_preview.get() != 0, "Stored frame available to preview");
        // A new capture on the same Recorder must reset its state and reject overwriting a take.
        recorder.start(o); recorder.wait();
        require(recorder.status().state == "Interrupted", "Existing take cannot be overwritten");
        const std::string prefix = "test-stop-repeat-" + std::to_string(clock_ticks());
        o.output = prefix; o.timestamped_output = true; o.duration_seconds = 0.02; o.fast = true;
        recorder.start(o); recorder.wait(); const std::string first_take = recorder.status().output;
        recorder.start(o); recorder.wait(); const std::string second_take = recorder.status().output;
        require(recorder.status().error.empty() && first_take != second_take, "Record can restart with the same prefix");
        require(path_exists(path_join(first_take, "manifest.json")) && path_exists(path_join(second_take, "manifest.json")), "Both takes preserved");
        require(std::regex_match(first_take.substr(prefix.size()), std::regex("-[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{3}(-[0-9]+)?")), "Take name contains a readable launch timestamp");
        require(default_take_path() != default_take_path(), "Sub-millisecond default paths are unique");
#ifdef _WIN32
        const std::string metadata = path_join(first_take, "lock-test.json");
        write_atomic(metadata, "old");
        HANDLE held = CreateFileW(from_utf8(metadata).c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        require(held != INVALID_HANDLE_VALUE, "Hold metadata without delete sharing");
        std::thread release_lock([held] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); CloseHandle(held); });
        try { write_atomic(metadata, "new"); } catch (...) { release_lock.join(); throw; }
        release_lock.join();
        std::ifstream updated(metadata.c_str()); std::string value; updated >> value;
        require(value == "new", "Brief Windows metadata locks are retried");
#endif
        o.timestamped_output = false;
        for (int strict = 0; strict < 2; ++strict) for (int mode = 0; mode < 4; ++mode) {
            o.strict_capture = strict != 0;
            o.output = "test-fault-" + std::to_string(clock_ticks()); o.fast = true; o.duration_seconds = 0.04;
            if (mode == 3) o.duration_seconds = 0.5;
            recorder.start(o, std::unique_ptr<AudioSource>(new FaultSource(mode))); recorder.wait();
            const RecorderStatus s = recorder.status();
            require(s.depth_frames == depth_frames_for_audio(s.frames, s.format.sample_rate), "Fault drains both streams");
            if (mode == 0) require(strict ? (s.state == "Interrupted" && s.frames == 960) :
                (s.state == "Complete with warnings" && s.frames == 1920 && s.warnings == 1), "Audio gap obeys selected policy");
            if (mode == 1) require(s.state == "Complete with warnings" && s.frames == 1920 && s.timestamp_errors == 1, "Invalid timestamps retain samples and quality flags");
            if (mode == 2) require(s.frames == 1440 && (strict ? s.state == "Interrupted" : s.state == "Complete with warnings"), "Source exceptions retain data and finite takes finish");
            if (mode == 3) require(strict ? s.state == "Interrupted" :
                (s.frames == 24000 && s.state == "Complete with warnings"), "Temporary audio failure resumes capture");
            if (!strict) {
                std::ifstream events(path_join(o.output, "timing/events.jsonl").c_str());
                const std::string text((std::istreambuf_iterator<char>(events)), std::istreambuf_iterator<char>());
                require(text.find("\"type\":\"warning\"") != std::string::npos, "Warnings persisted in the event journal");
            }
        }
        std::cout << "Core, simulation, stop/finalization, overwrite, discontinuity and source-failure tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
