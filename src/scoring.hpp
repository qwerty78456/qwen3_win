#pragma once
#include "common.hpp"
namespace asrwin {
std::vector<std::string> score_tokens(std::string_view text, bool characters=false);
Json error_rate(std::string_view reference, std::string_view hypothesis, bool characters);
bool normalized_equal(std::string_view a, std::string_view b);
}
