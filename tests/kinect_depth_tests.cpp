#include "recorder/recorder.h"
#include "recorder/platform.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <iterator>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace recorder;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::string contents(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    require(!!f, "Expected recording file");
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
class TestDepth : public DepthSource {
public:
    explicit TestDepth(int mode) : mode_(mode), index_(0) {}
    void open(const std::atomic<bool>& stop) {
        if (mode_ == 3) throw std::runtime_error("Injected Kinect open failure");
        if (mode_ == 4) while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::string device_id() const { return "test-kinect"; }
    std::string calibration_json() const { return "{\"test\":true}"; }
    bool read(DepthFrame& frame, const std::atomic<bool>& stop) {
        if (mode_ == 2 && index_ == 3) throw std::runtime_error("Injected Kinect disconnect");
        while ((index_ == 4 || mode_ == 5) && !stop) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop) return false;
        const std::int64_t times[] = { 10000000, 10333333, 11333333, 21333333 };
        frame.index = index_;
        frame.relative_time_100ns = mode_ == 1 && index_ == 2 ? times[0] : times[index_];
        frame.receipt_ticks = clock_ticks();
        frame.min_reliable_mm = 500; frame.max_reliable_mm = 4500;
        for (std::size_t i = 0; i < frame.millimetres.size(); ++i)
            frame.millimetres[i] = static_cast<std::uint16_t>(i + index_);
        ++index_;
        return true;
    }
private:
    int mode_;
    unsigned index_;
};
int main() {
    try {
        Recorder r;
        for (int strict = 0; strict < 2; ++strict) for (int mode = 0; mode < 6; ++mode) {
            RecordOptions o;
            o.output = "test-kinect-" + std::to_string(clock_ticks());
            o.depth_pattern = "kinect"; o.duration_seconds = 0.15; o.segment_seconds = 1;
            o.strict_capture = strict != 0;
            r.start(o, std::unique_ptr<AudioSource>(), std::unique_ptr<DepthSource>(new TestDepth(mode)));
            if (mode == 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); r.request_stop();
            }
            r.wait();
            const RecorderStatus s = r.status();
            if (!strict && mode != 3 && mode != 4 && s.state != "Complete with warnings")
                std::cerr << "Mode " << mode << ": " << s.state << ", audio=" << s.frames << ", error=" << s.error << '\n';
            require(!s.active, "Depth shutdown must join");
            if (mode == 0) {
                require(s.state == "Complete with warnings" && s.depth_frames == 4 && s.frames == 7200, "Native streams drain independently");
                require(s.depth_preview && s.depth_preview->index == 3, "Native preview available");
                require(s.depth_gap_intervals == 2, "Native gaps exposed to controller");
                const std::string manifest = contents(path_join(o.output, "manifest.json"));
                require(manifest.find("kinect-depth-audio-prototype/2") != std::string::npos, "Native schema");
                require(manifest.find("\"gap_intervals\":2") != std::string::npos, "Native gaps preserved");
                require(manifest.find("test-kinect") != std::string::npos && manifest.find("\"test\":true") != std::string::npos, "Device and calibration persisted");
                const std::string journal = contents(path_join(o.output, "timing/depth-frames.jsonl"));
                require(journal.find("\"relative_time_100ns\":\"11333333\"") != std::string::npos, "Native timestamp persisted");
                require(journal.find("\"gap_before\":true") != std::string::npos, "Gap journal flag");
                require(journal.find("pts_100ns") == std::string::npos && journal.find("audio_sample_floor") == std::string::npos, "No fabricated audio mapping");
                const std::string first = contents(path_join(o.output, "depth/000000.kd16"));
                const std::string second = contents(path_join(o.output, "depth/000001.kd16"));
                const std::size_t bytes = DepthWidth * DepthHeight * 2;
                require(first.size() == 64 + bytes * 3 && second.size() == 64 + bytes, "Rotate by native time, not frame count");
                require(first.compare(0, 8, std::string("KD16RAW\0", 8)) == 0 && first[52] == 1 && second[52] == 1, "Native segments finalized");
                for (unsigned f = 0; f < 3; ++f) for (unsigned i = 0; i < DepthWidth * DepthHeight; ++i) {
                    const std::uint16_t value = static_cast<unsigned char>(first[64 + f * bytes + i * 2]) |
                        (static_cast<unsigned char>(first[65 + f * bytes + i * 2]) << 8);
                    require(value == static_cast<std::uint16_t>(i + f), "All native uint16 values preserved");
                }
                bool rejected = false;
                try { export_depth_video(path_join(o.output, "depth/000000.kd16"), path_join(o.output, "invalid.mkv"), "", "unused"); }
                catch (const std::runtime_error&) { rejected = true; }
                require(rejected && !path_exists(path_join(o.output, "invalid.mkv")), "Native frames cannot be silently retimed by exporter");
            } else if (strict || mode == 3 || mode == 4) {
                require(s.state == "Interrupted" && !s.error.empty(), "Depth faults cannot produce Complete");
                if (mode == 1) require(s.depth_frames == 2, "Reject sensor clock reset");
                if (mode == 2) require(s.depth_frames == 3, "Drain captured prefix after disconnect");
                if (mode == 3 || mode == 4) require(!path_exists(o.output), "No take created before depth readiness");
                if (mode == 5) require(s.depth_frames == 0, "No generated substitute for missing depth");
            } else {
                require(s.state == "Complete with warnings" && s.error.empty() && s.frames == 7200, "Depth incidents must not stop audio");
                require(s.warnings > 0, "Depth warnings visible in status");
                if (mode == 1) {
                    require(s.depth_frames == 4, "Clock reset retains both sensor epochs");
                    const std::string journal = contents(path_join(o.output, "timing/depth-frames.jsonl"));
                    require(journal.find("\"timestamp_epoch\":\"1\"") != std::string::npos, "New clock epoch recorded");
                }
                if (mode == 2) require(s.depth_frames == 3, "Disconnected depth does not generate substitute images");
                if (mode == 5) require(s.depth_frames == 0, "Missing depth does not cancel audio");
            }
        }
        std::cout << "Native depth archive, gaps, rotation, faults, cancellation and export protection passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
