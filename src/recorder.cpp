#include "recorder/recorder.h"
#include "recorder/platform.h"
#include "recorder/wav_writer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace recorder {
namespace {
struct CaptureWarning {
    std::string code, message;
    std::uint64_t count, first_ticks, last_ticks;
};
void retry_pause(const std::atomic<bool>& stop) {
    for (unsigned i = 0; i < 10 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
// Both storage and packet capacity are fixed before acquisition starts.
class PacketQueue {
public:
    PacketQueue(unsigned slots, std::size_t samples) : packets_(slots), head_(0), tail_(0), size_(0), done_(false), aborted_(false) {
        for (std::size_t i = 0; i < packets_.size(); ++i) packets_[i].samples.reserve(samples);
    }
    bool push(const AudioPacket& p, bool wait, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (size_ == packets_.size() && wait && !aborted_ && !stop)
            changed_.wait_for(lock, std::chrono::milliseconds(20));
        if (size_ == packets_.size() || aborted_ || stop) return false;
        packets_[tail_] = p;
        tail_ = (tail_ + 1) % packets_.size(); ++size_;
        changed_.notify_one();
        return true;
    }
    bool push_depth(const DepthFrame& frame, const std::atomic<bool>& stop) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Two seconds of native depth, independent of the audio queue capacity.
        if (depth_.size() >= 60 || aborted_ || stop) return false;
        depth_.push_back(std::shared_ptr<DepthFrame>(new DepthFrame(frame)));
        changed_.notify_one();
        return true;
    }
    void warn(const std::string& code, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t now = clock_ticks();
        std::map<std::string, CaptureWarning>::iterator it = warnings_.find(code);
        if (it == warnings_.end()) {
            CaptureWarning warning = {code, message, 1, now, now}; warnings_[code] = warning;
        } else { ++it->second.count; it->second.last_ticks = now; it->second.message = message; }
        changed_.notify_one();
    }
    bool pop(AudioPacket& p, std::shared_ptr<DepthFrame>& depth, std::vector<CaptureWarning>& warnings) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return size_ || !depth_.empty() || !warnings_.empty() || done_ || aborted_; });
        if (aborted_ || (!size_ && depth_.empty() && warnings_.empty())) return false;
        warnings.clear();
        for (std::map<std::string, CaptureWarning>::const_iterator it = warnings_.begin(); it != warnings_.end(); ++it)
            warnings.push_back(it->second);
        warnings_.clear();
        p.samples.clear();
        depth.reset();
        if (!depth_.empty() && (!size_ || depth_.front()->receipt_ticks <= packets_[head_].receipt_ticks)) {
            depth = depth_.front(); depth_.pop_front(); return true;
        }
        if (!size_) return true; // Warnings only; no invented media packet.
        p = packets_[head_]; head_ = (head_ + 1) % packets_.size(); --size_;
        changed_.notify_one();
        return true;
    }
    void finish() { std::lock_guard<std::mutex> lock(mutex_); done_ = true; changed_.notify_all(); }
    void abort() { std::lock_guard<std::mutex> lock(mutex_); aborted_ = true; changed_.notify_all(); }
private:
    std::vector<AudioPacket> packets_;
    std::deque<std::shared_ptr<DepthFrame> > depth_;
    std::map<std::string, CaptureWarning> warnings_; // Coalesce pending warnings by fixed event code.
    std::size_t head_, tail_, size_;
    bool done_, aborted_;
    std::mutex mutex_;
    std::condition_variable changed_;
};

struct AudioFile { std::string path; std::uint64_t start, count; };
class SessionWriter {
public:
    SessionWriter(const RecordOptions& o, AudioFormat f, const AudioSource& source, std::uint64_t origin, EncodingQueue& encodings,
        const std::string& depth_id, const std::string& calibration)
        : options_(o), format_(f), source_name_(source.name()), device_id_(source.device_id()),
          origin_(origin), frames_(0), packets_(0), timestamp_errors_(0), checkpoint_frames_(0),
          timing_(0), events_(0), created_(utc_now()), encodings_(encodings), finalized_audio_(0), submitted_(0), warnings_(0) {
        create_new_directory(o.output);
        create_directories(path_join(o.output, "audio"));
        create_directories(path_join(o.output, "timing"));
        if (o.depth_pattern != "off") depth_.reset(new DepthWriter(o.output, o.depth_pattern, f.sample_rate, o.segment_seconds, depth_id, calibration, !o.strict_capture));
        manifest("recording", "");
        timing_ = open_file(path_join(o.output, "timing/audio-packets.jsonl"), "wb");
        try { events_ = open_file(path_join(o.output, "timing/events.jsonl"), "wb"); }
        catch (...) { std::fclose(timing_); timing_ = 0; throw; }
    }
    ~SessionWriter() { if (timing_) std::fclose(timing_); if (events_) std::fclose(events_); }
    std::uint64_t frames() const { return frames_; }
    std::uint64_t packets() const { return packets_; }
    std::uint64_t timestamp_errors() const { return timestamp_errors_; }
    std::uint64_t depth_frames() const { return depth_ ? depth_->frames() : 0; }
    std::uint64_t depth_gap_intervals() const { return depth_ ? depth_->gap_intervals() : 0; }
    std::shared_ptr<const DepthFrame> depth_preview() const { return depth_ ? depth_->latest() : std::shared_ptr<const DepthFrame>(); }
    std::uint64_t warnings() const { return warnings_; }
    std::string last_warning() const { return last_warning_; }
    void warn(const CaptureWarning& warning) {
        warnings_ += warning.count; last_warning_ = warning.message;
        std::ostringstream event;
        event << "{\"type\":\"warning\",\"code\":" << json_string(warning.code)
              << ",\"message\":" << json_string(warning.message) << ",\"count\":" << warning.count
              << ",\"first_ticks\":\"" << warning.first_ticks << "\",\"last_ticks\":\"" << warning.last_ticks << "\"}\n";
        write_text(events_, event.str());
    }
    void write_depth(const DepthFrame& frame) {
        const std::uint64_t before = depth_->gap_intervals();
        depth_->write(frame);
        if (depth_->gap_intervals() > before) {
            CaptureWarning warning = {"depth_discontinuity", "Kinect depth gap or clock reset; native timing preserved", 1, frame.receipt_ticks, frame.receipt_ticks};
            warn(warning);
        }
        // Keep checkpoints moving even when the microphone temporarily stalls.
        if (clock_100ns() - checkpoint_time_ >= 10000000ULL) checkpoint();
    }
    void write(const AudioPacket& packet) {
        const std::uint64_t packet_frames = packet.samples.size() / format_.channels;
        const std::uint64_t segment_frames = static_cast<std::uint64_t>(options_.segment_seconds) * format_.sample_rate;
        std::uint64_t offset = 0;
        if (packet.flags & TimestampError) ++timestamp_errors_;
        while (offset < packet_frames) {
            if (!wav_ || wav_->frames() == segment_frames) {
                if (wav_) { wav_->close(); ++finalized_audio_; }
                std::ostringstream name;
                name << "audio/" << std::setw(6) << std::setfill('0') << files_.size() << ".wav";
                wav_.reset(new WavWriter(path_join(options_.output, name.str()), format_));
                AudioFile entry = {name.str(), frames_, 0}; files_.push_back(entry);
            }
            const std::uint64_t n = std::min(packet_frames - offset, segment_frames - wav_->frames());
            const std::uint64_t file_offset = wav_->frames();
            wav_->write(packet.samples.data() + offset * format_.channels, n);
            files_.back().count += n; frames_ += n;
            std::ostringstream line;
            line.imbue(std::locale::classic());
            line << "{\"packet\":" << packets_ << ",\"file\":" << json_string(files_.back().path)
                 << ",\"file_frame\":" << file_offset << ",\"frames\":" << n
                 << ",\"packet_offset_frames\":" << offset << ",\"packet_frames\":" << packet_frames
                 << ",\"device_frame\":\"" << packet.device_frame << "\",\"timestamp_100ns\":\"" << packet.timestamp_100ns
                 << "\",\"receipt_ticks\":\"" << packet.receipt_ticks << "\",\"flags\":" << packet.flags
                 << ",\"timestamp_valid\":" << ((packet.flags & TimestampError) ? "false" : "true") << "}\n";
            write_text(timing_, line.str());
            offset += n;
        }
        if (packet.flags) {
            std::ostringstream event;
            event << "{\"type\":\"packet_flags\",\"packet\":" << packets_
                  << ",\"device_frame\":\"" << packet.device_frame << "\",\"flags\":" << packet.flags << "}\n";
            write_text(events_, event.str());
        }
        ++packets_;
        if (depth_) depth_->advance(frames_);
        queue_finalized_segments();
        if (frames_ - checkpoint_frames_ >= format_.sample_rate) checkpoint();
    }
    void finish(const std::string& error, bool writer_healthy) {
        if (!error.empty()) write_text(events_, "{\"type\":\"interrupted\",\"reason\":" + json_string(error) + "}\n");
        // A failed write may leave unmatched payload/timing bytes. Retain the previous
        // known-good checkpoint instead of claiming the tail is mutually consistent.
        if (writer_healthy) checkpoint();
        if (wav_) { wav_->close(); ++finalized_audio_; }
        if (depth_ && writer_healthy) depth_->finish();
        if (writer_healthy) queue_finalized_segments();
        manifest(error.empty() ? "complete" : "interrupted", error);
    }
private:
    void queue_finalized_segments() {
        if (!depth_ || !options_.encode_depth) return;
        const std::size_t ready = std::min(finalized_audio_, depth_->finalized_segments());
        while (submitted_ < ready) {
            std::ostringstream stem; stem << std::setw(6) << std::setfill('0') << submitted_;
            EncodingJob job;
            job.depth = path_join(options_.output, "depth/" + stem.str() + ".kd16");
            job.audio = path_join(options_.output, "audio/" + stem.str() + ".wav");
            job.output = path_join(options_.output, "video/" + stem.str() + ".mkv");
            job.ffmpeg = options_.ffmpeg;
            encodings_.enqueue(job); // Failure is reported separately; never stops capture.
            ++submitted_;
        }
    }
    static void write_text(std::FILE* file, const std::string& text) {
        if (std::fwrite(text.data(), 1, text.size(), file) != text.size()) throw std::runtime_error("Timing log write failed");
    }
    void checkpoint() {
        if (wav_) wav_->checkpoint();
        if (depth_) depth_->checkpoint();
        sync_file(timing_); sync_file(events_);
        std::ostringstream c;
        c << "{\"schema\":1,\"committed_frames\":" << frames_ << ",\"committed_packets\":" << packets_
          << ",\"timing_bytes\":" << file_position(timing_) << ",\"events_bytes\":" << file_position(events_)
          << ",\"depth_frames\":" << depth_frames() << ",\"depth_timing_bytes\":" << (depth_ ? depth_->timing_bytes() : 0) << "}\n";
        write_atomic(path_join(options_.output, "checkpoint.json"), c.str());
        manifest("recording", "");
        checkpoint_frames_ = frames_;
        checkpoint_time_ = clock_100ns();
    }
    void manifest(const std::string& state, const std::string& error) {
        std::ostringstream m;
        m.imbue(std::locale::classic());
        m << std::setprecision(17)
          << "{\n  \"schema\": " << json_string(options_.depth_pattern == "kinect" ? "kinect-depth-audio-prototype/2" :
                depth_ ? "kinect-depth-audio-prototype/1" : "kinect-audio-prototype/1")
          << ",\n  \"application\": \"0.1.0\",\n  \"kind\": " << json_string(options_.depth_pattern == "kinect" ?
                "kinect-depth-audio-capture" : depth_ ? "simulated-depth-audio-capture" : "audio-capture")
          << ",\n  \"created_utc\": " << json_string(created_)
          << ",\n  \"state\": " << json_string(state) << ",\n  \"error\": " << json_string(error)
          << ",\n  \"capture_policy\": " << json_string(options_.strict_capture ? "strict" : "continue-with-warnings")
          << ",\n  \"warnings\": " << warnings_ << ",\n  \"last_warning\": " << json_string(last_warning_)
          << ",\n  \"source\": " << json_string(options_.source) << ",\n  \"source_name\": " << json_string(source_name_)
          << ",\n  \"device_id\": " << json_string(device_id_)
          << ",\n  \"clock\": " << json_string(options_.source == "simulate" ? (options_.fast ? "synthetic-unpaced" : "synthetic-realtime") : "wasapi-qpc")
          << ",\n  \"host_clock_frequency\": \"" << clock_frequency() << "\",\n  \"origin_100ns\": \"" << origin_
          << "\",\n  \"sample_rate\": " << format_.sample_rate << ",\n  \"channels\": " << format_.channels
          << ",\n  \"channel_mask\": " << format_.channel_mask << ",\n  \"sample_format\": \"float32-le\",\n"
          << "  \"frames\": " << frames_ << ",\n  \"packets\": " << packets_
          << ",\n  \"timestamp_errors\": " << timestamp_errors_
          << ",\n  \"stored_duration_seconds\": " << static_cast<double>(frames_) / format_.sample_rate
          << ",\n  \"requested_duration_seconds\": " << options_.duration_seconds
          << ",\n  \"simulation\": {\"signal\": " << json_string(options_.signal) << ", \"frequency\": " << options_.frequency
          << ", \"amplitude\": " << options_.amplitude << "},\n  \"audio_files\": [";
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (i) m << ',';
            m << "\n    {\"path\": " << json_string(files_[i].path) << ", \"first_stored_frame\": " << files_[i].start
              << ", \"frames\": " << files_[i].count << '}';
        }
        m << "\n  ],\n  \"depth\": " << (depth_ ? depth_->json() : "null")
          << ",\n  \"background_encoding\": {\"enabled\":" << (options_.encode_depth ? "true" : "false")
          << ",\"directory\":\"video\",\"codec\":\"ffv1\"}\n}\n";
        write_atomic(path_join(options_.output, "manifest.json"), m.str());
    }
    RecordOptions options_;
    AudioFormat format_;
    std::string source_name_, device_id_;
    std::uint64_t origin_, frames_, packets_, timestamp_errors_, checkpoint_frames_;
    std::FILE* timing_;
    std::FILE* events_;
    std::string created_;
    std::vector<AudioFile> files_;
    std::unique_ptr<WavWriter> wav_;
    std::unique_ptr<DepthWriter> depth_;
    EncodingQueue& encodings_;
    std::size_t finalized_audio_, submitted_;
    std::uint64_t warnings_;
    std::uint64_t checkpoint_time_ = clock_100ns();
    std::string last_warning_;
};
}

RecorderStatus::RecorderStatus() : state("Idle"), frames(0), packets(0), timestamp_errors(0), depth_frames(0), depth_gap_intervals(0), warnings(0), active(false) {
    peak[0] = peak[1] = rms[0] = rms[1] = 0;
}
Recorder::Recorder(EncodingQueue::Executor encoder) : stop_(false), encodings_(encoder) {}
Recorder::~Recorder() { request_stop(); wait(); }
RecorderStatus Recorder::status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
void Recorder::request_stop() { stop_ = true; }
void Recorder::wait() { if (thread_.joinable()) thread_.join(); }
void Recorder::start(const RecordOptions& options, std::unique_ptr<AudioSource> source, std::unique_ptr<DepthSource> depth) {
    validate_options(options);
    if (status().active) throw std::runtime_error("A recording is already active");
    wait(); stop_ = false;
    RecordOptions resolved = options;
    if (resolved.timestamped_output) resolved.output = timestamped_take_path(options.output);
    { std::lock_guard<std::mutex> lock(mutex_); status_ = RecorderStatus(); status_.active = true;
      status_.state = "Preparing"; status_.output = resolved.output; }
    try { thread_ = std::thread(&Recorder::run, this, resolved, std::move(source), std::move(depth)); }
    catch (...) { std::lock_guard<std::mutex> lock(mutex_); status_.active = false; status_.state = "Interrupted"; throw; }
}
void Recorder::run(RecordOptions options, std::unique_ptr<AudioSource> source, std::unique_ptr<DepthSource> depth) {
    std::string error, writer_error, depth_error;
    std::unique_ptr<SessionWriter> writer;
    std::unique_ptr<PacketQueue> queue;
    std::thread disk_thread;
    std::thread depth_thread;
    std::atomic<bool> depth_begin(false);
    std::promise<std::pair<std::string, std::string> > depth_ready;
    try {
        if (!source) source = make_audio_source(options);
        const AudioFormat format = source->open();
        if (!format.channels || format.channels > 2 || !format.sample_rate || format.sample_rate > 192000)
            throw std::runtime_error("Unsupported source format");
        if (options.duration_seconds > 0 && options.duration_seconds * format.sample_rate < 0.5)
            throw std::runtime_error("Duration is shorter than one sample at the actual source rate");
        const unsigned capacity = source->max_packet_frames();
        if (!capacity || capacity > format.sample_rate) throw std::runtime_error("Unsupported audio packet capacity");
        const std::size_t packet_samples = static_cast<std::size_t>(capacity) * format.channels;
        // Around five seconds at the backend's maximum packet size; bounded to 64 MiB.
        const unsigned slots = std::max(2U, std::min(2048U, format.sample_rate * 5 / capacity));
        if (static_cast<std::uint64_t>(slots) * packet_samples * sizeof(float) > 64ULL * 1024 * 1024)
            throw std::runtime_error("Audio queue would exceed 64 MiB");
        queue.reset(new PacketQueue(slots, packet_samples));
        std::pair<std::string, std::string> depth_info("", "null");
        if (options.depth_pattern == "kinect") {
            std::future<std::pair<std::string, std::string> > ready = depth_ready.get_future();
            depth_thread = std::thread([&] {
                bool opened = false;
                try {
                    if (!depth) depth = make_kinect_depth_source();
                    depth->open(stop_);
                    if (stop_) throw std::runtime_error("Stopped while preparing Kinect");
                    depth_ready.set_value(std::make_pair(depth->device_id(), depth->calibration_json()));
                    opened = true;
                    while (!depth_begin && !stop_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    DepthFrame frame;
                    while (!stop_) {
                        try {
                            if (!depth->read(frame, stop_)) {
                                if (stop_) break;
                                throw std::runtime_error("Kinect depth stream returned no frame");
                            }
                            if (frame.millimetres.size() != DepthWidth * DepthHeight || frame.relative_time_100ns < 0)
                                throw std::runtime_error("Kinect returned an invalid depth frame; frame skipped");
                        } catch (const std::exception& e) {
                            if (options.strict_capture) throw;
                            queue->warn("depth_read", e.what()); retry_pause(stop_); continue;
                        }
                        if (!queue->push_depth(frame, stop_)) {
                            if (stop_) break;
                            if (options.strict_capture) throw std::runtime_error("Depth writer queue full");
                            queue->warn("depth_queue_full", "Depth writer queue full; incoming image skipped");
                        }
                    }
                } catch (const std::exception& e) {
                    depth_error = e.what(); stop_ = true;
                    if (!opened) depth_ready.set_exception(std::current_exception());
                }
                depth.reset(); // Release SDK/COM resources on their acquisition thread.
            });
            depth_info = ready.get();
        }
        AudioPacket packet; packet.samples.reserve(packet_samples);
        writer.reset(new SessionWriter(options, format, *source, clock_100ns(), encodings_, depth_info.first, depth_info.second));
        { std::lock_guard<std::mutex> lock(mutex_); status_.format = format; status_.source_name = source->name(); }
        disk_thread = std::thread([&, packet_samples] {
            try {
                AudioPacket pending; pending.samples.reserve(packet_samples);
                std::shared_ptr<DepthFrame> pending_depth;
                std::vector<CaptureWarning> warnings;
                while (queue->pop(pending, pending_depth, warnings)) {
                    for (std::size_t i = 0; i < warnings.size(); ++i) writer->warn(warnings[i]);
                    if (pending_depth) writer->write_depth(*pending_depth);
                    else if (!pending.samples.empty()) writer->write(pending);
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.frames = writer->frames(); status_.packets = writer->packets();
                    status_.timestamp_errors = writer->timestamp_errors();
                    status_.depth_frames = writer->depth_frames(); status_.depth_preview = writer->depth_preview();
                    status_.depth_gap_intervals = writer->depth_gap_intervals();
                    status_.warnings = writer->warnings(); status_.last_warning = writer->last_warning();
                }
            } catch (const std::exception& e) { writer_error = e.what(); stop_ = true; queue->abort(); }
        });
        source->start();
        depth_begin = true;
        { std::lock_guard<std::mutex> lock(mutex_); status_.state = "Recording"; }
        const std::uint64_t limit = options.duration_seconds > 0 ?
            static_cast<std::uint64_t>(std::llround(options.duration_seconds * format.sample_rate)) : 0;
        std::uint64_t captured = 0, expected_device_frame = 0;
        bool previous_valid = false, audio_failed = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
            static_cast<std::int64_t>(options.duration_seconds * 1000));
        while (!stop_ && (!limit || captured < limit)) {
            // An absent microphone must not make a finite diagnostic take run forever.
            if (audio_failed && limit && std::chrono::steady_clock::now() >= deadline) break;
            try {
                if (!source->read(packet, stop_)) {
                    if (stop_) break;
                    throw std::runtime_error("Audio stream returned no packet");
                }
                if (packet.samples.empty() || packet.samples.size() > packet_samples || packet.samples.size() % format.channels)
                    throw std::runtime_error("Source returned an invalid audio packet; packet skipped");
                for (std::size_t i = 0; i < packet.samples.size(); ++i)
                    if (!std::isfinite(packet.samples[i])) throw std::runtime_error("Non-finite audio samples; packet skipped");
            } catch (const std::exception& e) {
                if (options.strict_capture) throw;
                queue->warn("audio_read", e.what()); audio_failed = true; retry_pause(stop_); continue;
            }
            std::uint64_t count = packet.samples.size() / format.channels;
            if (limit && count > limit - captured) { count = limit - captured; packet.samples.resize(static_cast<std::size_t>(count) * format.channels); }
            const bool valid = !(packet.flags & TimestampError);
            const bool gap = captured && ((packet.flags & Discontinuity) || (valid && previous_valid && packet.device_frame != expected_device_frame));
            if (gap) packet.flags |= Discontinuity;
            if (gap && !options.strict_capture) queue->warn("audio_discontinuity", "Audio discontinuity; captured samples and native timing retained");
            if (!valid) queue->warn("audio_timestamp", "Audio timestamp marked invalid by the device; samples retained");
            expected_device_frame = packet.device_frame + count; previous_valid = valid;
            float peak[2] = {0, 0}; double power[2] = {0, 0};
            for (std::size_t i = 0; i < packet.samples.size(); ++i) {
                const float v = packet.samples[i];
                const unsigned c = static_cast<unsigned>(i % format.channels);
                peak[c] = std::max(peak[c], std::abs(v)); power[c] += static_cast<double>(v) * v;
            }
            if (!queue->push(packet, options.fast, stop_)) {
                if (stop_) break;
                if (options.strict_capture) { error = "Audio writer queue full"; break; }
                queue->warn("audio_queue_full", "Audio writer queue full; incoming packet skipped");
            }
            captured += count;
            { std::lock_guard<std::mutex> lock(mutex_); for (unsigned c = 0; c < format.channels; ++c) {
                status_.peak[c] = peak[c]; status_.rms[c] = static_cast<float>(std::sqrt(power[c] / count)); } }
            if (gap && options.strict_capture) { error = "Audio discontinuity detected; inspect timing/events.jsonl before using this take"; break; }
        }
        if (!captured && error.empty()) {
            if (options.strict_capture) error = "Stopped before any audio was captured";
            else queue->warn("audio_empty", "No audio was captured; any available Kinect frames were retained");
        }
    } catch (const std::exception& e) { error = e.what(); }
    stop_ = true;
    if (depth_thread.joinable()) depth_thread.join();
    if (!depth_error.empty()) error = depth_error;
    { std::lock_guard<std::mutex> lock(mutex_); status_.state = "Finalizing"; }
    if (source) { try { source->stop(); } catch (const std::exception& e) {
        if (!options.strict_capture && queue) queue->warn("audio_stop", e.what());
        else if (error.empty()) error = e.what();
    } }
    if (queue) queue->finish();
    if (disk_thread.joinable()) disk_thread.join();
    if (!writer_error.empty()) error = writer_error;
    if (writer) { try {
        if (options.depth_pattern == "kinect" && !writer->depth_frames() && error.empty()) {
            if (options.strict_capture) error = "Stopped before any Kinect depth frame was captured";
            else {
                CaptureWarning warning = {"depth_empty", "No Kinect depth was captured; audio was retained", 1, clock_ticks(), clock_ticks()};
                writer->warn(warning);
            }
        }
        writer->finish(error, writer_error.empty());
    } catch (const std::exception& e) { error += (error.empty() ? "" : "; ") + std::string(e.what()); } }
    { std::lock_guard<std::mutex> lock(mutex_);
      if (writer) { status_.frames = writer->frames(); status_.packets = writer->packets(); status_.timestamp_errors = writer->timestamp_errors();
          status_.depth_frames = writer->depth_frames(); status_.depth_preview = writer->depth_preview(); }
      if (writer) status_.depth_gap_intervals = writer->depth_gap_intervals();
      if (writer) { status_.warnings = writer->warnings(); status_.last_warning = writer->last_warning(); }
      status_.error = error; status_.state = error.empty() ? (status_.warnings ? "Complete with warnings" : "Complete") : "Interrupted"; status_.active = false; }
}
}
