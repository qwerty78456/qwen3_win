#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace asrwin {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
inline double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
void check(HRESULT hr, const char* operation);
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::string read_text(const fs::path& path);
void write_json(const fs::path& path, const Json& value);
std::vector<float> read_floats(const fs::path& path);
void write_floats(const fs::path& path, std::span<const float> values);
fs::path executable_dir();
Json memory_usage();
std::string sha256_file(const fs::path& path);
void verify_files(const fs::path& model_dir, const std::string& variant="fp32");
class MappedFile {
 public:
  explicit MappedFile(const fs::path& path);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  const void* data() const { return data_; }
  size_t size() const { return size_; }
 private:
  HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
  void* data_ = nullptr;
  size_t size_ = 0;
};
struct ComScope {
 HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
 ComScope() { check(result, "Initialize COM"); }
 ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
}
