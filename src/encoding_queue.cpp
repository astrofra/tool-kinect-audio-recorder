#include "recorder/encoding_queue.h"
#include "recorder/depth.h"
#include "recorder/platform.h"
#include <stdexcept>

namespace recorder {
namespace {
void job_state(const EncodingJob& job, const std::string& state, const std::string& error) {
    write_atomic(job.output + ".json", "{\"schema\":1,\"state\":" + json_string(state) +
        ",\"depth\":" + json_string(job.depth) + ",\"audio\":" + json_string(job.audio) +
        ",\"output\":" + json_string(job.output) + ",\"updated_utc\":" + json_string(utc_now()) +
        ",\"error\":" + json_string(error) + "}\n");
}
void encode(const EncodingJob& job) {
    // A .part file is never advertised as a completed video. Preserve it on failure
    // for diagnosis, and always retain the original depth, audio and timing files.
    create_directories(job.output.substr(0, job.output.find_last_of("/\\")));
    if (path_exists(job.output)) throw std::runtime_error("Export output already exists: " + job.output);
    job_state(job, "encoding", "");
    try {
        export_depth_video(job.depth, job.output + ".part", job.audio,
            job.ffmpeg.empty() ? default_ffmpeg_path() : job.ffmpeg, true, job.output + ".log");
        publish_file(job.output + ".part", job.output);
        job_state(job, "complete", "");
    } catch (const std::exception& e) {
        try { job_state(job, "failed", e.what()); } catch (...) {}
        throw;
    }
}
}
EncodingQueue::EncodingQueue(Executor executor) : executor_(executor ? executor : encode), closing_(false) {}
EncodingQueue::~EncodingQueue() {
    { std::lock_guard<std::mutex> lock(mutex_); closing_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
bool EncodingQueue::enqueue(const EncodingJob& job) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        if (closing_) throw std::runtime_error("Encoding queue is closing");
        if (jobs_.size() >= 1024) throw std::runtime_error("Encoding queue full (1024 waiting segments); originals retained");
        // Lazy creation: audio-only/manual-export users need no extra thread.
        if (!worker_.joinable()) worker_ = std::thread(&EncodingQueue::run, this);
        jobs_.push_back(job);
        status_.pending = jobs_.size();
    } catch (const std::exception& e) {
        ++status_.failed; status_.last_error = job.output + ": " + e.what();
        return false;
    }
    changed_.notify_all();
    return true;
}
EncodingStatus EncodingQueue::status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
void EncodingQueue::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return jobs_.empty() && !status_.active; });
}
void EncodingQueue::run() {
    for (;;) {
        EncodingJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return closing_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            job = std::move(jobs_.front()); jobs_.pop_front();
            status_.pending = jobs_.size(); status_.active = true; status_.current_output = job.output;
        }
        std::string error;
        try { executor_(job); }
        catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "Unknown encoding failure"; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error.empty()) ++status_.completed;
            else { ++status_.failed; status_.last_error = job.output + ": " + error; }
            status_.active = false; status_.current_output.clear();
        }
        changed_.notify_all();
    }
}
}
