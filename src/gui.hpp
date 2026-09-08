#pragma once
#include "common.hpp"
namespace asrwin {
struct GuiOptions { fs::path model; int threads=ASRWIN_THREADS; int max_tokens=512; std::string variant="fp32"; bool directml=false; bool verify_distribution=true; };
int run_gui(HINSTANCE instance, const GuiOptions& options);
}
