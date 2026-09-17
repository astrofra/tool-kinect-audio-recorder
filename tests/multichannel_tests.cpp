#include "recorder/recorder.h"
#include "recorder/platform.h"
#include "recorder/wav_writer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static float sample(std::uint64_t frame, unsigned channel) {
    const float value = (channel + 1) / 64.0f + (frame % 17) / 4096.0f;
    return channel % 2 ? -value : value;
}
class ArraySource : public recorder::AudioSource {
public:
    explicit ArraySource(recorder::AudioFormat format) : format_(format), frame_(0) {}
    recorder::AudioFormat open() { return format_; }
    // The 32-channel case exercises the queue memory bound at maximum capacity/rate.
    unsigned max_packet_frames() const { return format_.channels == 32 ? format_.sample_rate : 257; }
    std::string name() const { return "Injected microphone array"; }
    std::string device_id() const { return "array-test"; }
    void start() {}
    bool read(recorder::AudioPacket& p, const std::atomic<bool>& stop) {
        if (stop) return false;
        p.samples.resize(257 * format_.channels);
        for (unsigned f = 0; f < 257; ++f) for (unsigned c = 0; c < format_.channels; ++c)
            p.samples[f * format_.channels + c] = sample(frame_ + f, c);
        p.device_frame = frame_;
        p.timestamp_100ns = recorder::frames_to_100ns(frame_, format_.sample_rate);
        p.receipt_ticks = recorder::clock_ticks(); p.flags = 0; frame_ += 257;
        return true;
    }
    void stop() {}
private:
    recorder::AudioFormat format_;
    std::uint64_t frame_;
};
int main(int argc, char** argv) {
    try {
        using namespace recorder;
        const std::string root = argc == 2 ? argv[1] : "test-multichannel-" + std::to_string(clock_ticks());
        create_directories(root);
        const AudioFormat formats[] = {AudioFormat(16000, 4), AudioFormat(44100, 8, 0x63f),
            AudioFormat(192000, 32), AudioFormat(16000, 2, 0xc), AudioFormat(8000, 1, 4)};
        Recorder recorder;
        for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
            const AudioFormat format = formats[i];
            RecordOptions o; o.output = path_join(root, std::to_string(format.channels));
            o.fast = true; o.duration_seconds = i == 0 ? 2.013 : 0.023;
            o.segment_seconds = format.channels == 32 ? 600 : 1;
            o.depth_pattern = i == 0 ? "gradient" : "off";
            recorder.start(o, std::unique_ptr<AudioSource>(new ArraySource(format))); recorder.wait();
            const RecorderStatus s = recorder.status();
            require(s.error.empty(), s.error.c_str());
            require(s.frames == static_cast<std::uint64_t>(std::llround(o.duration_seconds * format.sample_rate)), "Exact multichannel frame count");
            require(s.format.channels == format.channels && s.format.channel_mask == format.channel_mask, "Channel layout retained");
            require(s.peak.size() == format.channels && s.rms.size() == format.channels, "Meter count follows source and resets between takes");
            require(s.warnings == (format.channels == 32 ? 1U : 0U), "RIFF rotation cap warns once; other captures are clean");
            const std::uint64_t first = (s.frames - 1) / 257 * 257;
            for (unsigned c = 0; c < format.channels; ++c) {
                float peak = 0; double power = 0;
                for (std::uint64_t f = first; f < s.frames; ++f) {
                    const float value = sample(f, c);
                    peak = std::max(peak, std::abs(value)); power += static_cast<double>(value) * value;
                }
                require(std::abs(s.peak[c] - peak) < 1e-6 &&
                    std::abs(s.rms[c] - std::sqrt(power / (s.frames - first))) < 1e-6, "Independent meters retain channel order");
            }
            const std::uint64_t limit = wav_max_frames(format);
            const std::uint64_t overhead = wav_extensible(format) ? 72 : 50;
            require(limit * format.channels * 4 + overhead <= 0xffffffffULL &&
                (limit + 1) * format.channels * 4 + overhead > 0xffffffffULL, "RIFF sample limit avoids overflow");
        }
        RecordOptions invalid; invalid.output = path_join(root, "invalid"); invalid.duration_seconds = 0.01;
        recorder.start(invalid, std::unique_ptr<AudioSource>(new ArraySource(AudioFormat(16000, 33)))); recorder.wait();
        require(recorder.status().state == "Interrupted" && !path_exists(invalid.output), "Unsupported channel count rejected before writing");
        std::cout << "Multichannel capture, meters, rotation, memory bounds and format rejection passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
