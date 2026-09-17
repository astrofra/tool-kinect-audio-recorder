#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace recorder {
static const unsigned MaxAudioChannels = 32;
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
    double gain_db, gain_start, gain_end;
    unsigned gain_ramp_frames;
    bool pause_boundary;
    AudioPacket() : device_frame(0), timestamp_100ns(0), receipt_ticks(0), flags(0),
        gain_db(0), gain_start(1), gain_end(1), gain_ramp_frames(0), pause_boundary(false) {}
};

struct RecordOptions {
    std::string output;
    std::string source;
    std::string device_id;
    std::string session_name;
    std::string signal;
    std::string depth_pattern; // off, gradient, noise or kinect (Microsoft SDK 2.0).
    unsigned sample_rate;
    unsigned channels;
    double frequency;
    double amplitude;
    double gain_db; // Software gain before WAV storage and metering, -24..+36 dB.
    double duration_seconds; // Zero means until Stop.
    unsigned segment_seconds;
    bool fast;
    bool encode_depth;
    bool encode_preview; // Whole-take RGB review, queued after finalization.
    bool strict_capture; // Default: warn and continue after acquisition incidents.
    bool timestamped_output; // Treat output as a prefix; resolve on every start.
    std::string ffmpeg; // Empty selects the bundled encoder, then PATH.
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
