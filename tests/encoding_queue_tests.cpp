#include "recorder/recorder.h"
#include "recorder/platform.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main(int argc, char** argv) {
    try {
        using namespace recorder;
        // Stand in for an encoder that creates an incomplete file and then fails.
        if (argc > 1 && std::string(argv[1]) == "-hide_banner") {
#ifdef _WIN32
            if (GetPriorityClass(GetCurrentProcess()) != IDLE_PRIORITY_CLASS) return 38;
#endif
            std::FILE* partial = open_file(argv[argc - 1], "wb");
            std::fputs("incomplete", partial); std::fclose(partial);
            std::cerr << "Injected encoder failure after writing partial output\n";
            return 37;
        }
        std::vector<std::string> order;
        std::atomic<unsigned> executing(0), maximum(0);
        std::promise<void> entered, release;
        const std::shared_future<void> gate = release.get_future().share();
        EncodingQueue queue([&](const EncodingJob& job) {
            const unsigned count = ++executing;
            if (count > maximum) maximum = count;
            if (job.output == "first") {
                entered.set_value();
                gate.wait_for(std::chrono::seconds(5)); // Bound test failure, including destructor drain.
            }
            order.push_back(job.output); --executing;
            if (job.output == "fail") throw std::runtime_error("Injected encoder failure");
        });
        EncodingJob job; job.output = "first"; require(queue.enqueue(job), "First job accepted");
        require(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Worker starts");
        job.output = "fail"; require(queue.enqueue(job), "Enqueue does not wait for blocked encoder");
        job.output = "last"; require(queue.enqueue(job), "Next job accepted");
        const EncodingStatus blocked = queue.status();
        release.set_value(); queue.wait();
        require(blocked.active && blocked.pending == 2 && blocked.completed == 0, "Queue exposes pending work without blocking");
        require(maximum == 1 && order == std::vector<std::string>({"first", "fail", "last"}), "Exactly one worker, FIFO even after failure");
        require(queue.status().completed == 2 && queue.status().failed == 1 && !queue.status().busy(), "Errors isolated and drain completes");
        job.output = "another take"; queue.enqueue(job); queue.wait();
        require(queue.status().completed == 3, "Queue survives wait and accepts another take");

        std::promise<void> full_entered, full_release;
        const std::shared_future<void> full_gate = full_release.get_future().share();
        std::atomic<unsigned> calls(0);
        EncodingQueue bounded([&](const EncodingJob&) {
            if (++calls == 1) { full_entered.set_value(); full_gate.wait_for(std::chrono::seconds(5)); }
        });
        bounded.enqueue(job);
        require(full_entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Capacity test starts");
        bool accepted = true;
        for (unsigned i = 0; i < 1024; ++i) accepted = bounded.enqueue(job) && accepted;
        const bool overflow = bounded.enqueue(job);
        const EncodingStatus full = bounded.status();
        full_release.set_value(); bounded.wait();
        require(accepted && !overflow && full.pending == 1024 && full.failed == 1, "Bounded queue rejects overflow without waiting");
        require(calls == 1025, "Accepted work drained exactly once");

        // A blocked encoder must not delay Stop, wait() or another capture on the same Recorder.
        std::promise<void> capture_release;
        const std::shared_future<void> capture_gate = capture_release.get_future().share();
        std::atomic<unsigned> encoded(0);
        Recorder recorder([&](const EncodingJob& next) {
            capture_gate.wait_for(std::chrono::seconds(5));
            require(path_exists(next.depth) && path_exists(next.audio), "Both source files present");
            ++encoded;
        });
        RecordOptions options; options.depth_pattern = "gradient"; options.encode_depth = true;
        options.fast = true; options.duration_seconds = 1.01; options.segment_seconds = 1;
        options.output = "test-encoding-" + std::to_string(clock_ticks());
        recorder.start(options); recorder.wait();
        const bool first_ok = recorder.status().state == "Complete" && recorder.encoding_status().busy() && encoded == 0;
        options.output += "-next"; options.duration_seconds = 0.01;
        recorder.start(options); recorder.wait();
        const bool second_ok = recorder.status().state == "Complete" && encoded == 0;
        capture_release.set_value(); recorder.wait_for_encodings();
        require(first_ok && second_ok && encoded == 3, "Capture and new takes are independent of encoder backlog");

        unsigned drained = 0;
        { EncodingQueue finishing([&](const EncodingJob&) { ++drained; }); finishing.enqueue(job); finishing.enqueue(job); }
        require(drained == 2, "Orderly destruction drains and joins");

        Recorder failing;
        options.output = "test-encoding-" + std::to_string(clock_ticks()); options.ffmpeg = argv[0];
        failing.start(options); failing.wait(); failing.wait_for_encodings();
        const std::string output = path_join(options.output, "video/000000.mkv");
        require(failing.status().state == "Complete" && failing.encoding_status().failed == 1,
            "Process failure leaves capture successful");
        require(failing.encoding_status().last_error.find("exit 37") != std::string::npos,
            "Encoder ran at background priority and its exit code was reported");
        require(!path_exists(output) && path_exists(output + ".part"), "Incomplete output never published as MKV");
        require(path_exists(output + ".log") && path_exists(output + ".json"), "Persistent failure diagnostics");
        write_atomic(output, "existing video");
        bool replaced = true;
        try { publish_file(output + ".part", output); } catch (const std::exception&) { replaced = false; }
        require(!replaced && path_exists(output + ".part"), "Publishing never replaces an existing output");
        std::cout << "FIFO, single worker, bounded backlog, failure isolation, Stop/new take and shutdown passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
