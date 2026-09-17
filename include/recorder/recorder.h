#pragma once
#include "recorder/audio.h"
#include "recorder/depth.h"
#include "recorder/encoding_queue.h"
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
    std::uint64_t depth_gap_intervals;
    std::uint64_t warnings;
    std::string last_warning;
    std::shared_ptr<const DepthFrame> depth_preview;
    float peak[2];
    float rms[2];
    bool active;
    RecorderStatus();
};

class Recorder {
public:
    explicit Recorder(EncodingQueue::Executor encoder = EncodingQueue::Executor());
    ~Recorder();
    // start/wait belong to the controller thread; status/request_stop may be called concurrently.
    // A supplied source is opened/read/destroyed on the recording thread.
    void start(const RecordOptions& options, std::unique_ptr<AudioSource> source = std::unique_ptr<AudioSource>(),
        std::unique_ptr<DepthSource> depth = std::unique_ptr<DepthSource>());
    void request_stop();
    void wait();
    RecorderStatus status() const;
    EncodingStatus encoding_status() const { return encodings_.status(); }
    void wait_for_encodings() { encodings_.wait(); }
    bool export_preview(const std::string& take);
private:
    Recorder(const Recorder&);
    Recorder& operator=(const Recorder&);
    void run(RecordOptions options, std::unique_ptr<AudioSource> source, std::unique_ptr<DepthSource> depth);
    mutable std::mutex mutex_;
    RecorderStatus status_;
    std::atomic<bool> stop_;
    std::thread thread_;
    EncodingQueue encodings_; // Survives Stop/start; wait() joins acquisition only.
};
}
