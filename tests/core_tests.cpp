#include "recorder/audio.h"
#include "recorder/platform.h"
#include "recorder/recorder.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

class FaultSource : public recorder::AudioSource {
public:
    explicit FaultSource(int mode) : mode_(mode), index_(0), origin_(0) {}
    recorder::AudioFormat open() { return recorder::AudioFormat(48000, 1); }
    unsigned max_packet_frames() const { return 480; }
    std::string name() const { return "fault-test"; }
    std::string device_id() const { return "fault-test"; }
    void start() { origin_ = recorder::clock_100ns(); }
    bool read(recorder::AudioPacket& p, const std::atomic<bool>& stop) {
        if (stop) return false;
        if (mode_ == 2 && index_ == 3) throw std::runtime_error("Injected source failure");
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
};

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        using namespace recorder;
        require(frames_to_100ns(44100, 44100) == 10000000, "44.1 kHz conversion");
        require(frames_to_100ns(48000ULL * 86400, 48000) == 864000000000ULL, "Long clock conversion");
        require(json_string("a\n\"\\\t") == "\"a\\u000a\\\"\\\\\\u0009\"", "JSON escaping");
        RecordOptions o; o.output = "unused"; o.signal = "sine"; o.frequency = 1000; o.amplitude = 0.5;
        require(std::abs(simulated_sample(o, 12, 0) - 0.5f) < 1e-6f, "Sine quarter cycle");
        require(std::abs(simulated_sample(o, 36, 0) + 0.5f) < 1e-6f, "Sine negative quarter cycle");
        o.signal = "markers";
        for (unsigned i = 0; i < 100000; ++i) require(std::abs(simulated_sample(o, i, 0)) <= 0.5f, "Signal amplitude bound");
        bool rejected = false; o.amplitude = 2;
        try { validate_options(o); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Amplitude validation");
        o.amplitude = 0.25; o.output = "test-stop-" + std::to_string(clock_ticks());
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
        // A new capture on the same Recorder must reset its state and reject overwriting a take.
        recorder.start(o); recorder.wait();
        require(recorder.status().state == "Interrupted", "Existing take cannot be overwritten");
        for (int mode = 0; mode < 3; ++mode) {
            o.output = "test-fault-" + std::to_string(clock_ticks()); o.fast = true; o.duration_seconds = 0.04;
            recorder.start(o, std::unique_ptr<AudioSource>(new FaultSource(mode))); recorder.wait();
            const RecorderStatus s = recorder.status();
            if (mode == 0) require(s.state == "Interrupted" && s.frames == 960, "Gap stops capture and preserves queued prefix");
            if (mode == 1) require(s.state == "Complete" && s.frames == 1920 && s.timestamp_errors == 1, "Invalid timestamps retain samples and quality flags");
            if (mode == 2) require(s.state == "Interrupted" && s.frames == 1440, "Source exception drains queued audio");
        }
        std::cout << "Core, simulation, stop/finalization, overwrite, discontinuity and source-failure tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
