#include "recorder/audio.h"
#include "recorder/platform.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace recorder {
RecordOptions::RecordOptions() : source("simulate"), signal("markers"), depth_pattern("off"), sample_rate(48000),
    channels(1), frequency(440.0), amplitude(0.25), duration_seconds(0), segment_seconds(60), fast(false), encode_depth(false) {}

void validate_options(const RecordOptions& o) {
    if (o.output.empty()) throw std::invalid_argument("Choose an output directory");
    if (o.source != "simulate" && o.source != "wasapi") throw std::invalid_argument("Source must be simulate or wasapi");
    if (o.sample_rate < 8000 || o.sample_rate > 192000) throw std::invalid_argument("Sample rate must be 8000..192000");
    if (o.channels < 1 || o.channels > 2) throw std::invalid_argument("Simulation supports one or two channels");
    if (o.signal != "sine" && o.signal != "markers") throw std::invalid_argument("Signal must be sine or markers");
    if (o.depth_pattern != "off" && o.depth_pattern != "gradient" && o.depth_pattern != "noise")
        throw std::invalid_argument("Depth must be off, gradient or noise");
    if (o.encode_depth && o.depth_pattern == "off") throw std::invalid_argument("Automatic encoding requires depth capture");
    if (!std::isfinite(o.frequency) || o.frequency <= 0 || o.frequency * (o.channels == 2 ? 1.5 : 1.0) >= o.sample_rate * 0.5)
        throw std::invalid_argument("Tone frequency must be positive and below Nyquist on every channel");
    if (!std::isfinite(o.amplitude) || o.amplitude < 0 || o.amplitude > 1)
        throw std::invalid_argument("Amplitude must be finite and in 0..1");
    if (!std::isfinite(o.duration_seconds) || o.duration_seconds < 0 || o.duration_seconds > 86400)
        throw std::invalid_argument("Duration must be 0..86400 seconds (0 means until Stop)");
    if (o.duration_seconds > 0 && o.duration_seconds * o.sample_rate < 0.5)
        throw std::invalid_argument("Duration is shorter than one sample");
    if (o.segment_seconds < 1 || o.segment_seconds > 600) throw std::invalid_argument("Segment duration must be 1..600 seconds");
    if (o.fast && (o.source != "simulate" || o.duration_seconds == 0))
        throw std::invalid_argument("--fast requires simulation and a finite duration");
}

float simulated_sample(const RecordOptions& o, std::uint64_t frame, unsigned channel) {
    const double pi = 3.14159265358979323846;
    const double t = static_cast<double>(frame) / o.sample_rate;
    const double tone = o.frequency * (channel == 0 ? 1.0 : 1.5);
    double value = std::sin(2 * pi * std::fmod(t * tone, 1.0));
    if (o.signal == "markers") {
        // 80 ms, 1 kHz burst at every whole sample-clock second, with 5 ms ramps.
        const double phase = static_cast<double>(frame % o.sample_rate) / o.sample_rate;
        const double envelope = phase < 0.08 ? std::min(1.0, std::min(phase / 0.005, (0.08 - phase) / 0.005)) : 0.0;
        value = 0.4 * value + 0.6 * envelope * std::sin(2 * pi * std::fmod(t * 1000, 1.0));
    }
    return static_cast<float>(o.amplitude * value);
}

class SimulatedSource : public AudioSource {
public:
    explicit SimulatedSource(const RecordOptions& o) : options_(o), position_(0), origin_(0) {}
    AudioFormat open() { return AudioFormat(options_.sample_rate, options_.channels, options_.channels == 1 ? 4 : 3); }
    unsigned max_packet_frames() const { return options_.sample_rate / 100; }
    std::string name() const { return "Simulated " + options_.signal; }
    std::string device_id() const { return "simulation"; }
    void start() { position_ = 0; origin_ = clock_100ns(); }
    bool read(AudioPacket& p, const std::atomic<bool>& stopped) {
        const unsigned count = max_packet_frames();
        if (!options_.fast) {
            const std::uint64_t deadline = origin_ + frames_to_100ns(position_ + count, options_.sample_rate);
            while (!stopped && clock_100ns() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stopped) return false;
        p.device_frame = position_;
        p.timestamp_100ns = origin_ + frames_to_100ns(position_, options_.sample_rate);
        p.receipt_ticks = clock_ticks();
        p.flags = options_.amplitude == 0 ? Silent : 0;
        p.samples.resize(static_cast<std::size_t>(count) * options_.channels);
        for (unsigned f = 0; f < count; ++f)
            for (unsigned c = 0; c < options_.channels; ++c)
                p.samples[f * options_.channels + c] = simulated_sample(options_, position_ + f, c);
        position_ += count;
        return true;
    }
    void stop() {}
private:
    RecordOptions options_;
    std::uint64_t position_, origin_;
};
std::unique_ptr<AudioSource> make_simulated_source(const RecordOptions& o) {
    return std::unique_ptr<AudioSource>(new SimulatedSource(o));
}
std::unique_ptr<AudioSource> make_audio_source(const RecordOptions& o) {
    return o.source == "simulate" ? make_simulated_source(o) : make_wasapi_source(o);
}
}
