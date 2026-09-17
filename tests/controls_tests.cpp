#include "recorder/recorder.h"
#include "recorder/platform.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace recorder;
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static std::string contents(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
class ConstantSource : public AudioSource {
    std::uint64_t position_ = 0;

  public:
    AudioFormat open() { return AudioFormat(16000, 4); }
    unsigned max_packet_frames() const { return 160; }
    std::string name() const { return "Gain fixture"; }
    std::string device_id() const { return "constant-four"; }
    void start() {}
    bool read(AudioPacket &p, const std::atomic<bool> &stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (stop)
            return false;
        p.samples.resize(640);
        for (unsigned i = 0; i < 640; ++i)
            p.samples[i] = .01f * (i % 4 + 1);
        p.device_frame = position_;
        p.timestamp_100ns = frames_to_100ns(position_, 16000);
        p.receipt_ticks = clock_ticks();
        p.flags = 0;
        position_ += 160;
        return true;
    }
    void stop() {}
};
class ContinuousDepth : public DepthSource {
    unsigned frame_ = 0;

  public:
    void open(const std::atomic<bool> &) {}
    std::string device_id() const { return "pause-depth"; }
    std::string calibration_json() const { return "null"; }
    bool read(DepthFrame &frame, const std::atomic<bool> &stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (stop)
            return false;
        frame.index = frame_++;
        frame.relative_time_100ns = 10000000 + frame.index * 333333;
        frame.receipt_ticks = clock_ticks();
        return true;
    }
};
static void wait_frames(Recorder &r, std::uint64_t frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (r.status().active && r.status().frames < frames && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(r.status().frames >= frames, "Capture made progress");
}
int main() {
    try {
        Recorder r;
        RecordOptions o;
        o.output = "test-controls-" + std::to_string(clock_ticks());
        o.duration_seconds = .6;
        o.gain_db = 20;
        o.strict_capture = true;
        o.depth_pattern = "kinect";
        r.start(o, std::unique_ptr<AudioSource>(new ConstantSource), std::unique_ptr<DepthSource>(new ContinuousDepth));
        wait_frames(r, 800);
        require(std::abs(r.status().peak[3] - .4f) < 1e-5, "Initial gain reaches all four meters");
        r.set_paused(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
        const auto paused = r.status();
        require(paused.paused && paused.state == "Paused", "Pause reported");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        require(r.status().frames == paused.frames && r.status().depth_frames == paused.depth_frames,
                "Pause excludes both media streams after queued data drains");
        r.set_gain_db(30);
        r.set_paused(false);
        r.wait();
        const auto done = r.status();
        require(done.error.empty() && done.frames == 9600 && !done.paused,
                "Same take resumes to exact requested stored duration");
        require(done.depth_gap_intervals == 0, "Intentional depth pause is not a device fault");
        require(done.warnings == 1 && done.last_warning.find("0 dBFS") != std::string::npos,
                "Gain overload is logged without aborting strict capture");
        const auto wav = contents(path_join(o.output, "audio/000000.wav"));
        require(wav.size() == 80 + 9600 * 4 * 4, "All gained frames stored in extensible WAV");
        for (unsigned c = 0; c < 4; ++c) {
            float first, last;
            std::memcpy(&first, wav.data() + 80 + c * 4, 4);
            std::memcpy(&last, wav.data() + wav.size() - 16 + c * 4, 4);
            require(std::abs(first - (c + 1) * .1f) < 1e-6 &&
                        std::abs(last - (c + 1) * .01f * std::sqrt(1000.0f)) < 1e-5,
                    "WAV contains gain before and after change, no hard clipping");
            require(std::abs(done.peak[c] - last) < 1e-6, "Meters describe written samples");
        }
        const auto events = contents(path_join(o.output, "timing/events.jsonl"));
        require(events.find("\"type\":\"pause\"") != std::string::npos &&
                    events.find("\"type\":\"resume\"") != std::string::npos,
                "Pause boundaries journaled");
        const auto packets = contents(path_join(o.output, "timing/audio-packets.jsonl"));
        require(packets.find("\"gain_ramp_frames\":160") != std::string::npos &&
                    packets.find("\"pause_boundary\":true") != std::string::npos,
                "Gain ramp and resumed sample boundary are recoverable");
        bool rejected = false;
        try {
            r.set_gain_db(std::numeric_limits<double>::quiet_NaN());
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "Invalid live gain rejected");
        o.output = "test-controls-stop-" + std::to_string(clock_ticks());
        o.duration_seconds = 0;
        o.gain_db = 0;
        o.depth_pattern = "off";
        r.start(o, std::unique_ptr<AudioSource>(new ConstantSource));
        wait_frames(r, 320);
        r.set_paused(true);
        r.request_stop();
        r.wait();
        require(r.status().error.empty() && !r.status().active, "Stop during pause finalizes without deadlock");
        std::cout
            << "Live four-channel gain, ramp metadata, pause/resume, native depth, overload and stop-in-pause passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
