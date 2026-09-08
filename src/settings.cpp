#include "settings.hpp"
#include <shlobj.h>
namespace asrwin {
Json Settings::to_json() const { return {{"schema",1},{"provider",provider},{"adapter_luid",adapter_luid},{"adapter_name",adapter_name},{"large_model",large_model}}; }
fs::path default_settings_path() {
  PWSTR folder=nullptr;
  check(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&folder),"Locate the local application data folder");
  fs::path base(folder); CoTaskMemFree(folder);
  return base/L"AsrWin"/L"settings.json";
}
std::optional<Settings> load_settings(const fs::path& path, std::string& warning) {
  warning.clear();
  if (!fs::exists(path)) return std::nullopt;
  try {
    auto j=Json::parse(read_text(path));
    if (!j.is_object() || j.value("schema",0)!=1) { warning="unsupported settings schema"; return std::nullopt; }
    Settings s; s.provider=j.value("provider","cpu"); s.adapter_luid=j.value("adapter_luid",""); s.adapter_name=j.value("adapter_name",""); s.large_model=j.value("large_model",false);
    if (s.provider!="cpu" && s.provider!="directml") { warning="unknown provider '"+s.provider+"'"; return std::nullopt; }
    return s;
  } catch (const std::exception& e) { warning=std::string("cannot read ")+utf8(path.filename().wstring())+": "+e.what(); return std::nullopt; }
}
void save_settings(const fs::path& path, const Settings& settings) {
  fs::create_directories(path.parent_path());
  fs::path temporary=path; temporary+=L".tmp";
  { std::ofstream f(temporary,std::ios::binary); f<<settings.to_json().dump(2)<<'\n'; if (!f) throw std::runtime_error("Cannot write "+utf8(temporary.wstring())); }
  if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { auto error=GetLastError(); fs::remove(temporary); throw std::runtime_error("Cannot replace "+utf8(path.wstring())+" (error "+std::to_string(error)+")"); }
}
}
