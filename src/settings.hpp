#pragma once
#include "common.hpp"
namespace asrwin {
// Interface preferences persisted in %LOCALAPPDATA%\AsrWin\settings.json (the only file the application
// writes without an explicit Save/Report). The command line never reads it.
struct Settings {
  std::string provider="cpu";      // cpu | directml
  std::string adapter_luid, adapter_name;
  bool large_model=false;
  Json to_json() const;
};
fs::path default_settings_path();
// nullopt when the file does not exist; a corrupt or foreign file yields nullopt with a non-empty warning.
std::optional<Settings> load_settings(const fs::path& path, std::string& warning);
void save_settings(const fs::path& path, const Settings& settings);  // atomic replace
}
