#include "recorder/platform.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace recorder {
static std::string system_error() {
#ifdef _WIN32
    char text[256]; strerror_s(text, sizeof(text), errno); return text;
#else
    return std::strerror(errno);
#endif
}
#ifdef _WIN32
std::wstring from_utf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), 0, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8 path or device ID");
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}
std::string to_utf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), 0, 0, 0, 0);
    if (!n) throw std::runtime_error("Invalid UTF-16 string");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &out[0], n, 0, 0);
    return out;
}
#endif
std::uint64_t clock_ticks() {
#ifdef _WIN32
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return static_cast<std::uint64_t>(n.QuadPart);
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
std::uint64_t clock_frequency() {
#ifdef _WIN32
    LARGE_INTEGER n;
    QueryPerformanceFrequency(&n);
    return static_cast<std::uint64_t>(n.QuadPart);
#else
    return 1000000000ULL;
#endif
}
std::uint64_t clock_100ns() {
    const std::uint64_t ticks = clock_ticks(), f = clock_frequency();
    return (ticks / f) * 10000000ULL + (ticks % f) * 10000000ULL / f;
}
std::uint64_t frames_to_100ns(std::uint64_t frames, unsigned rate) {
    if (!rate) throw std::invalid_argument("Sample rate must be positive");
    return frames / rate * 10000000ULL + frames % rate * 10000000ULL / rate;
}
std::string utc_now() {
    std::time_t now = std::time(0);
    std::tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}
std::string default_take_path() {
    std::string stamp = utc_now();
    for (std::size_t i = 0; i < stamp.size(); ++i) if (stamp[i] == ':') stamp[i] = '-';
    return "recordings/take-" + stamp + "-" + std::to_string(clock_ticks() % 1000000000ULL);
}
std::string json_string(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}
std::string path_join(const std::string& directory, const std::string& leaf) {
    return directory + "/" + leaf;
}
static bool directory_exists(const std::string& path) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(from_utf8(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}
static void mkdir_checked(const std::string& path) {
#ifdef _WIN32
    int result = _wmkdir(from_utf8(path).c_str());
#else
    int result = mkdir(path.c_str(), 0777);
#endif
    if (result) throw std::runtime_error("Cannot create new directory '" + path + "': " + system_error());
}
void create_directories(const std::string& path) {
    if (path.empty() || directory_exists(path)) return;
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0) create_directories(path.substr(0, slash));
    // Another process may create a parent concurrently; only the take directory must be exclusive.
    try { mkdir_checked(path); } catch (...) { if (!directory_exists(path)) throw; }
}
void create_new_directory(const std::string& path) {
    if (path.empty()) throw std::invalid_argument("Output directory is empty");
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0) create_directories(path.substr(0, slash));
    mkdir_checked(path);
}
std::FILE* open_file(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::FILE* f = 0;
    _wfopen_s(&f, from_utf8(path).c_str(), from_utf8(mode).c_str());
#else
    std::FILE* f = std::fopen(path.c_str(), mode);
#endif
    if (!f) throw std::runtime_error("Cannot open '" + path + "': " + system_error());
    return f;
}
bool path_exists(const std::string& path) {
#ifdef _WIN32
    if (GetFileAttributesW(from_utf8(path).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw std::runtime_error("Cannot inspect output path (Windows error " + std::to_string(error) + ")");
#else
    struct stat info;
    if (lstat(path.c_str(), &info) == 0) return true;
    if (errno == ENOENT) return false;
    throw std::runtime_error("Cannot inspect output path: " + system_error());
#endif
}
void sync_file(std::FILE* file) {
    if (std::fflush(file)) throw std::runtime_error("File flush failed");
#ifdef _WIN32
    if (_commit(_fileno(file))) throw std::runtime_error("File checkpoint failed");
#else
    if (fsync(fileno(file))) throw std::runtime_error("File checkpoint failed");
#endif
}
std::uint64_t file_position(std::FILE* file) {
#ifdef _WIN32
    const __int64 position = _ftelli64(file);
#else
    const off_t position = ftello(file);
#endif
    if (position < 0) throw std::runtime_error("Cannot read file position");
    return static_cast<std::uint64_t>(position);
}
void write_atomic(const std::string& path, const std::string& content) {
    const std::string temp = path + ".tmp";
    std::FILE* file = open_file(temp, "wb");
    try {
        if (std::fwrite(content.data(), 1, content.size(), file) != content.size())
            throw std::runtime_error("Metadata write failed");
        sync_file(file);
    } catch (...) { std::fclose(file); throw; }
    if (std::fclose(file)) throw std::runtime_error("Metadata close failed");
#ifdef _WIN32
    if (!MoveFileExW(from_utf8(temp).c_str(), from_utf8(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit metadata: " + path);
#else
    if (std::rename(temp.c_str(), path.c_str())) throw std::runtime_error("Cannot commit metadata: " + path);
#endif
}
}
