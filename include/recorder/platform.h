#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace recorder {
// Clock values have the same 100 ns unit as WASAPI's QPC position.
std::uint64_t clock_100ns();
std::uint64_t clock_ticks();
std::uint64_t clock_frequency();
std::uint64_t frames_to_100ns(std::uint64_t frames, unsigned rate);
std::string utc_now();
std::string default_take_path();
std::string json_string(const std::string& text);
std::string path_join(const std::string& directory, const std::string& leaf);
void create_directories(const std::string& path);
void create_new_directory(const std::string& path);
bool path_exists(const std::string& path);
std::FILE* open_file(const std::string& path, const char* mode);
void sync_file(std::FILE* file);
std::uint64_t file_position(std::FILE* file);
void write_atomic(const std::string& path, const std::string& content);
int run_process(const std::vector<std::string>& arguments);
#ifdef _WIN32
std::wstring from_utf8(const std::string& value);
std::string to_utf8(const std::wstring& value);
#endif
}
