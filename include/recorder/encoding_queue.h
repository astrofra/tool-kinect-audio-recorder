#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace recorder {
struct EncodingJob {
    std::string depth, audio, output, ffmpeg;
    std::string preview_take; // Nonempty selects whole-take RGB preview instead of archival export.
};
struct EncodingStatus {
    std::size_t pending, completed, failed;
    bool active;
    std::string current_output, last_error;
    EncodingStatus() : pending(0), completed(0), failed(0), active(false) {}
    bool busy() const { return active || pending != 0; }
};

// One FIFO for the application's lifetime, across takes. Only paths are queued.
// The worker never holds mutex_ during encoding or file I/O.
class EncodingQueue {
public:
    typedef std::function<void(const EncodingJob&)> Executor;
    explicit EncodingQueue(Executor executor = Executor());
    ~EncodingQueue(); // Drain and join; the GUI polls status before destruction.
    bool enqueue(const EncodingJob& job); // Never waits for encoding or queue space.
    EncodingStatus status() const;
    void wait(); // Drain current work; new takes can enqueue afterwards.
private:
    EncodingQueue(const EncodingQueue&);
    EncodingQueue& operator=(const EncodingQueue&);
    void run();
    Executor executor_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<EncodingJob> jobs_;
    EncodingStatus status_;
    bool closing_;
    std::thread worker_;
};
}
