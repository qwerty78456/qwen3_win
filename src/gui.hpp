#pragma once
#include "common.hpp"
namespace asrwin {
struct GuiOptions {
  fs::path model; int threads=ASRWIN_THREADS; int max_tokens=512; std::string variant="fp32"; bool directml=false; bool verify_distribution=true;
  std::wstring adapter_selector;       // "", "default" or a DXGI index (from --adapter)
  bool skip_adapter_check=false;       // bypass the video-memory rule (experiments)
  bool experimental=false;             // --experimental-directml: show GPU items in builds that are not validated
  bool large_model=false;              // --large-model: preselect the large model (DirectML only)
  fs::path large_model_dir; int large_model_threads=ASRWIN_THREADS;  // empty when the build has no large model
  fs::path settings_path; bool use_settings=true;
  bool provider_from_cli=false;        // --provider/--adapter/--large-model given: overrides the saved settings
};
int run_gui(HINSTANCE instance, const GuiOptions& options);
}
