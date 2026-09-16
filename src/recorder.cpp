#include "recorder/recorder.h"
#include "recorder/platform.h"
#include "recorder/wav_writer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace recorder {
namespace {
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
    bool pop(AudioPacket& p) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return size_ || done_ || aborted_; });
        if (!size_ || aborted_) return false;
        p = packets_[head_]; head_ = (head_ + 1) % packets_.size(); --size_;
        changed_.notify_one();
        return true;
    }
    void finish() { std::lock_guard<std::mutex> lock(mutex_); done_ = true; changed_.notify_all(); }
    void abort() { std::lock_guard<std::mutex> lock(mutex_); aborted_ = true; changed_.notify_all(); }
private:
    std::vector<AudioPacket> packets_;
    std::size_t head_, tail_, size_;
    bool done_, aborted_;
    std::mutex mutex_;
    std::condition_variable changed_;
};

struct AudioFile { std::string path; std::uint64_t start, count; };
class SessionWriter {
public:
    SessionWriter(const RecordOptions& o, AudioFormat f, const AudioSource& source, std::uint64_t origin)
        : options_(o), format_(f), source_name_(source.name()), device_id_(source.device_id()),
          origin_(origin), frames_(0), packets_(0), timestamp_errors_(0), checkpoint_frames_(0),
          timing_(0), events_(0), created_(utc_now()) {
        create_new_directory(o.output);
        create_directories(path_join(o.output, "audio"));
        create_directories(path_join(o.output, "timing"));
        manifest("recording", "");
        timing_ = open_file(path_join(o.output, "timing/audio-packets.jsonl"), "wb");
        try { events_ = open_file(path_join(o.output, "timing/events.jsonl"), "wb"); }
        catch (...) { std::fclose(timing_); timing_ = 0; throw; }
    }
    ~SessionWriter() { if (timing_) std::fclose(timing_); if (events_) std::fclose(events_); }
    std::uint64_t frames() const { return frames_; }
    std::uint64_t packets() const { return packets_; }
    std::uint64_t timestamp_errors() const { return timestamp_errors_; }
    void write(const AudioPacket& packet) {
        const std::uint64_t packet_frames = packet.samples.size() / format_.channels;
        const std::uint64_t segment_frames = static_cast<std::uint64_t>(options_.segment_seconds) * format_.sample_rate;
        std::uint64_t offset = 0;
        if (packet.flags & TimestampError) ++timestamp_errors_;
        while (offset < packet_frames) {
            if (!wav_ || wav_->frames() == segment_frames) {
                if (wav_) wav_->close();
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
        if (frames_ - checkpoint_frames_ >= format_.sample_rate) checkpoint();
    }
    void finish(const std::string& error, bool writer_healthy) {
        if (!error.empty()) write_text(events_, "{\"type\":\"interrupted\",\"reason\":" + json_string(error) + "}\n");
        // A failed write may leave unmatched payload/timing bytes. Retain the previous
        // known-good checkpoint instead of claiming the tail is mutually consistent.
        if (writer_healthy) checkpoint();
        if (wav_) wav_->close();
        manifest(error.empty() ? "complete" : "interrupted", error);
    }
private:
    static void write_text(std::FILE* file, const std::string& text) {
        if (std::fwrite(text.data(), 1, text.size(), file) != text.size()) throw std::runtime_error("Timing log write failed");
    }
    void checkpoint() {
        if (wav_) wav_->checkpoint();
        sync_file(timing_); sync_file(events_);
        std::ostringstream c;
        c << "{\"schema\":1,\"committed_frames\":" << frames_ << ",\"committed_packets\":" << packets_
          << ",\"timing_bytes\":" << file_position(timing_) << ",\"events_bytes\":" << file_position(events_) << "}\n";
        write_atomic(path_join(options_.output, "checkpoint.json"), c.str());
        manifest("recording", "");
        checkpoint_frames_ = frames_;
    }
    void manifest(const std::string& state, const std::string& error) {
        std::ostringstream m;
        m.imbue(std::locale::classic());
        m << std::setprecision(17)
          << "{\n  \"schema\": \"kinect-audio-prototype/1\",\n  \"application\": \"0.1.0\",\n"
          << "  \"kind\": \"audio-capture\",\n  \"created_utc\": " << json_string(created_)
          << ",\n  \"state\": " << json_string(state) << ",\n  \"error\": " << json_string(error)
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
        m << "\n  ]\n}\n";
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
};
}

RecorderStatus::RecorderStatus() : state("Idle"), frames(0), packets(0), timestamp_errors(0), active(false) {
    peak[0] = peak[1] = rms[0] = rms[1] = 0;
}
Recorder::Recorder() : stop_(false) {}
Recorder::~Recorder() { request_stop(); wait(); }
RecorderStatus Recorder::status() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }
void Recorder::request_stop() { stop_ = true; }
void Recorder::wait() { if (thread_.joinable()) thread_.join(); }
void Recorder::start(const RecordOptions& options, std::unique_ptr<AudioSource> source) {
    validate_options(options);
    if (status().active) throw std::runtime_error("A recording is already active");
    wait(); stop_ = false;
    { std::lock_guard<std::mutex> lock(mutex_); status_ = RecorderStatus(); status_.active = true;
      status_.state = "Preparing"; status_.output = options.output; }
    try { thread_ = std::thread(&Recorder::run, this, options, std::move(source)); }
    catch (...) { std::lock_guard<std::mutex> lock(mutex_); status_.active = false; status_.state = "Interrupted"; throw; }
}
void Recorder::run(RecordOptions options, std::unique_ptr<AudioSource> source) {
    std::string error, writer_error;
    std::unique_ptr<SessionWriter> writer;
    std::unique_ptr<PacketQueue> queue;
    std::thread disk_thread;
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
        AudioPacket packet; packet.samples.reserve(packet_samples);
        writer.reset(new SessionWriter(options, format, *source, clock_100ns()));
        { std::lock_guard<std::mutex> lock(mutex_); status_.format = format; status_.source_name = source->name(); }
        disk_thread = std::thread([&, packet_samples] {
            try {
                AudioPacket pending; pending.samples.reserve(packet_samples);
                while (queue->pop(pending)) {
                    writer->write(pending);
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.frames = writer->frames(); status_.packets = writer->packets();
                    status_.timestamp_errors = writer->timestamp_errors();
                }
            } catch (const std::exception& e) { writer_error = e.what(); stop_ = true; queue->abort(); }
        });
        source->start();
        { std::lock_guard<std::mutex> lock(mutex_); status_.state = "Recording"; }
        const std::uint64_t limit = options.duration_seconds > 0 ?
            static_cast<std::uint64_t>(std::llround(options.duration_seconds * format.sample_rate)) : 0;
        std::uint64_t captured = 0, expected_device_frame = 0;
        bool previous_valid = false;
        while (!stop_ && (!limit || captured < limit) && source->read(packet, stop_)) {
            if (packet.samples.empty() || packet.samples.size() > packet_samples || packet.samples.size() % format.channels)
                throw std::runtime_error("Source returned an invalid packet size");
            std::uint64_t count = packet.samples.size() / format.channels;
            if (limit && count > limit - captured) { count = limit - captured; packet.samples.resize(static_cast<std::size_t>(count) * format.channels); }
            const bool valid = !(packet.flags & TimestampError);
            const bool gap = captured && ((packet.flags & Discontinuity) || (valid && previous_valid && packet.device_frame != expected_device_frame));
            if (gap) packet.flags |= Discontinuity;
            expected_device_frame = packet.device_frame + count; previous_valid = valid;
            float peak[2] = {0, 0}; double power[2] = {0, 0};
            for (std::size_t i = 0; i < packet.samples.size(); ++i) {
                const float v = packet.samples[i];
                if (!std::isfinite(v)) throw std::runtime_error("Audio source returned a non-finite sample");
                const unsigned c = static_cast<unsigned>(i % format.channels);
                peak[c] = std::max(peak[c], std::abs(v)); power[c] += static_cast<double>(v) * v;
            }
            if (!queue->push(packet, options.fast, stop_)) {
                if (!stop_) error = "Audio writer queue full; capture stopped without silently dropping packets";
                break;
            }
            captured += count;
            { std::lock_guard<std::mutex> lock(mutex_); for (unsigned c = 0; c < format.channels; ++c) {
                status_.peak[c] = peak[c]; status_.rms[c] = static_cast<float>(std::sqrt(power[c] / count)); } }
            if (gap) { error = "Audio discontinuity detected; inspect timing/events.jsonl before using this take"; break; }
        }
        if (!captured && error.empty()) error = "Stopped before any audio was captured";
    } catch (const std::exception& e) { error = e.what(); }
    { std::lock_guard<std::mutex> lock(mutex_); status_.state = "Finalizing"; }
    if (source) { try { source->stop(); } catch (const std::exception& e) { if (error.empty()) error = e.what(); } }
    if (queue) queue->finish();
    if (disk_thread.joinable()) disk_thread.join();
    if (!writer_error.empty()) error = writer_error;
    if (writer) { try { writer->finish(error, writer_error.empty()); } catch (const std::exception& e) { error += (error.empty() ? "" : "; ") + std::string(e.what()); } }
    { std::lock_guard<std::mutex> lock(mutex_);
      if (writer) { status_.frames = writer->frames(); status_.packets = writer->packets(); status_.timestamp_errors = writer->timestamp_errors(); }
      status_.error = error; status_.state = error.empty() ? "Complete" : "Interrupted"; status_.active = false; }
}
}
