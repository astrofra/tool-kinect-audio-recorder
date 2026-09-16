#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace recorder {
struct AudioFormat {
    unsigned sample_rate;
    unsigned channels;
    unsigned channel_mask;
    AudioFormat(unsigned rate = 48000, unsigned count = 1, unsigned mask = 0)
        : sample_rate(rate), channels(count), channel_mask(mask) {}
};

enum PacketFlags { Discontinuity = 1, Silent = 2, TimestampError = 4 };
struct AudioPacket {
    std::vector<float> samples;
    std::uint64_t device_frame;
    std::uint64_t timestamp_100ns;
    std::uint64_t receipt_ticks;
    unsigned flags;
    AudioPacket() : device_frame(0), timestamp_100ns(0), receipt_ticks(0), flags(0) {}
};

struct RecordOptions {
    std::string output;
    std::string source;
    std::string device_id;
    std::string signal;
    std::string depth_pattern; // off, gradient or noise (synthetic, audio-clock driven).
    unsigned sample_rate;
    unsigned channels;
    double frequency;
    double amplitude;
    double duration_seconds; // Zero means until Stop.
    unsigned segment_seconds;
    bool fast;
    RecordOptions();
};
void validate_options(const RecordOptions& options);

class AudioSource {
public:
    virtual ~AudioSource() {}
    virtual AudioFormat open() = 0;
    virtual unsigned max_packet_frames() const = 0;
    virtual std::string name() const = 0;
    virtual std::string device_id() const = 0;
    virtual void start() = 0;
    // Reuse the caller's preallocated packet. Return false only on stop/end.
    virtual bool read(AudioPacket& packet, const std::atomic<bool>& stop) = 0;
    virtual void stop() = 0;
};

struct AudioDevice { std::string id; std::string name; };
std::vector<AudioDevice> enumerate_audio_devices();
std::unique_ptr<AudioSource> make_simulated_source(const RecordOptions& options);
std::unique_ptr<AudioSource> make_wasapi_source(const RecordOptions& options);
std::unique_ptr<AudioSource> make_audio_source(const RecordOptions& options);
float simulated_sample(const RecordOptions& options, std::uint64_t frame, unsigned channel);
}
