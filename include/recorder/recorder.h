#pragma once
#include "recorder/audio.h"
#include "recorder/depth.h"
#include <mutex>
#include <thread>

namespace recorder {
struct RecorderStatus {
    std::string state;
    std::string error;
    std::string source_name;
    std::string output;
    AudioFormat format;
    std::uint64_t frames;
    std::uint64_t packets;
    std::uint64_t timestamp_errors;
    std::uint64_t depth_frames;
    std::shared_ptr<const DepthFrame> depth_preview;
    float peak[2];
    float rms[2];
    bool active;
    RecorderStatus();
};

class Recorder {
public:
    Recorder();
    ~Recorder();
    // start/wait belong to the controller thread; status/request_stop may be called concurrently.
    // A supplied source is opened/read/destroyed on the recording thread.
    void start(const RecordOptions& options, std::unique_ptr<AudioSource> source = std::unique_ptr<AudioSource>());
    void request_stop();
    void wait();
    RecorderStatus status() const;
private:
    Recorder(const Recorder&);
    Recorder& operator=(const Recorder&);
    void run(RecordOptions options, std::unique_ptr<AudioSource> source);
    mutable std::mutex mutex_;
    RecorderStatus status_;
    std::atomic<bool> stop_;
    std::thread thread_;
};
}
