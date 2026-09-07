#pragma once
#include "common.hpp"
namespace asrwin {
struct GuiOptions { fs::path model; int threads=4; int max_tokens=512; std::string variant="fp32"; };
int run_gui(HINSTANCE instance, const GuiOptions& options);
}
