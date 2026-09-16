#include "recorder/platform.h"
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace recorder {
int run_process(const std::vector<std::string>& arguments) {
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
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(0, &command[0], 0, 0, TRUE, CREATE_NO_WINDOW, 0, 0, &startup, &process))
        throw std::runtime_error("Cannot launch " + arguments[0] + " (Windows error " + std::to_string(GetLastError()) + ")");
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
    pid_t pid;
    const int error = posix_spawnp(&pid, argv[0], 0, 0, argv.data(), environ);
    if (error) throw std::runtime_error("Cannot launch " + arguments[0] + ": " + std::strerror(error));
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("Cannot wait for encoder process");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
}
