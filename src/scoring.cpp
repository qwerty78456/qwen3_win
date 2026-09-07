#include "scoring.hpp"
#include <numeric>
namespace asrwin {
std::vector<std::string> score_tokens(std::string_view text, bool characters) {
    auto source = wide(text);
    if (source.empty()) return {};
    int count = NormalizeString(NormalizationKC, source.data(), static_cast<int>(source.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Cannot normalize scoring text");
    std::wstring normal(count, L'\0');
    count = NormalizeString(NormalizationKC, source.data(), static_cast<int>(source.size()), normal.data(), count);
    if (count <= 0) throw std::runtime_error("Cannot normalize scoring text");
    normal.resize(count);
    std::vector<std::string> tokens;
    std::string word;
    auto flush = [&] { if (!word.empty()) { tokens.push_back(word); word.clear(); } };
    for (size_t i=0; i<normal.size(); ++i) {
        uint32_t cp = normal[i];
        size_t begin = i;
        if (cp >= 0xd800 && cp <= 0xdbff && i+1<normal.size()) {
            cp = 0x10000 + ((cp-0xd800)<<10) + (normal[++i]-0xdc00);
        }
        bool chinese = (cp>=0x3400 && cp<=0x9fff) || (cp>=0xf900 && cp<=0xfaff) || (cp>=0x20000 && cp<=0x323af);
        if (chinese) { flush(); tokens.push_back(utf8(std::wstring_view(normal).substr(begin, i-begin+1))); }
        else if ((cp>='A'&&cp<='Z') || (cp>='a'&&cp<='z') || (cp>='0'&&cp<='9')) {
            char c = static_cast<char>(cp>='A'&&cp<='Z' ? cp+32 : cp);
            if (characters) { flush(); tokens.push_back(std::string(1,c)); } else word.push_back(c);
        } else if (cp!='\'' && cp!=0x2019) { flush(); }
    }
    flush();
    return tokens;
}
bool normalized_equal(std::string_view a, std::string_view b) { return score_tokens(a)==score_tokens(b); }
Json error_rate(std::string_view reference, std::string_view hypothesis, bool characters) {
    auto ref=score_tokens(reference,characters), hyp=score_tokens(hypothesis,characters);
    std::vector<size_t> previous(hyp.size()+1), current(hyp.size()+1);
    std::iota(previous.begin(),previous.end(),0);
    for(size_t i=1;i<=ref.size();++i) {
        current[0]=i;
        for(size_t j=1;j<=hyp.size();++j) current[j]=std::min({previous[j]+1,current[j-1]+1,previous[j-1]+(ref[i-1]!=hyp[j-1])});
        previous.swap(current);
    }
    Json result={{"edits",previous.back()},{"reference_units",ref.size()},{"hypothesis_units",hyp.size()}};
    result["rate"]=ref.empty()?Json(nullptr):Json(double(previous.back())/ref.size());
    return result;
}
}
