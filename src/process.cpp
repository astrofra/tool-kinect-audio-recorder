#include "recorder/platform.h"
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <cstdlib>
#endif
extern char** environ;
#endif

namespace recorder {
std::string default_ffmpeg_path() {
    std::string executable;
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(0, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) throw std::runtime_error("Cannot locate the recorder executable");
    executable = to_utf8(std::wstring(buffer.data(), length));
    const char* relative = "extern/ffmpeg/ffmpeg.exe";
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(0, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        char* resolved = realpath(buffer.data(), 0);
        if (resolved) { executable = resolved; std::free(resolved); }
    }
    const char* relative = "extern/ffmpeg/ffmpeg";
#else
    std::vector<char> buffer(4096);
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) break;
        if (static_cast<std::size_t>(length) < buffer.size()) {
            executable.assign(buffer.data(), static_cast<std::size_t>(length)); break;
        }
        if (buffer.size() >= 1024 * 1024) break;
        buffer.resize(buffer.size() * 2);
    }
    const char* relative = "extern/ffmpeg/ffmpeg";
#endif
    const std::size_t separator = executable.find_last_of("/\\");
    if (separator != std::string::npos) {
        const std::string bundled = path_join(executable.substr(0, separator), relative);
        if (path_exists(bundled)) return bundled;
    }
    return "ffmpeg";
}
int run_process(const std::vector<std::string>& arguments, bool background, const std::string& log) {
    if (arguments.empty()) throw std::invalid_argument("Empty process command");
#ifdef _WIN32
    std::wstring command;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i) command += L' ';
        command += L'"';
        const std::wstring argument = from_utf8(arguments[i]);
        unsigned slashes = 0;
        for (std::size_t j = 0; j < argument.size(); ++j) {
            const wchar_t ch = argument[j];
            if (ch == L'\\') { ++slashes; continue; }
            command.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0; command += ch;
        }
        command.append(slashes * 2, L'\\'); command += L'"';
    }
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE log_file = INVALID_HANDLE_VALUE;
    if (!log.empty()) {
        SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), 0, TRUE};
        log_file = CreateFileW(from_utf8(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
        if (log_file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create encoder log: " + log);
        startup.hStdOutput = startup.hStdError = log_file;
    }
    PROCESS_INFORMATION process = {};
    const BOOL launched = CreateProcessW(0, &command[0], 0, 0, TRUE,
        CREATE_NO_WINDOW | (background ? IDLE_PRIORITY_CLASS | CREATE_NEW_PROCESS_GROUP : 0), 0, 0, &startup, &process);
    const DWORD launch_error = GetLastError();
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    if (!launched)
        throw std::runtime_error("Cannot launch " + arguments[0] + " (Windows error " + std::to_string(launch_error) + ")");
    CloseHandle(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    const BOOL obtained = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (waited != WAIT_OBJECT_0 || !obtained) throw std::runtime_error("Cannot wait for encoder process");
    return static_cast<int>(code);
#else
    std::vector<char*> argv;
    for (std::size_t i = 0; i < arguments.size(); ++i) argv.push_back(const_cast<char*>(arguments[i].c_str()));
    argv.push_back(0);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) throw std::runtime_error("Cannot prepare encoder process: " + std::string(std::strerror(error)));
    if (!log.empty()) {
        error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    }
    posix_spawnattr_t attributes;
    const int attribute_error = posix_spawnattr_init(&attributes);
    if (attribute_error) {
        posix_spawn_file_actions_destroy(&actions);
        throw std::runtime_error("Cannot prepare encoder attributes: " + std::string(std::strerror(attribute_error)));
    }
    // Keep Ctrl+C directed at the recorder; already queued encodings drain normally.
    if (!error && background) error = posix_spawnattr_setpgroup(&attributes, 0);
    if (!error && background) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    pid_t pid;
    if (!error) error = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error) throw std::runtime_error("Cannot launch " + arguments[0] + ": " + std::strerror(error));
    // Best effort after spawn; affects only the encoder, never the capture process.
    if (background) (void)setpriority(PRIO_PROCESS, pid, 10);
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("Cannot wait for encoder process");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
}
