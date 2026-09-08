#include "diagnostics.hpp"
#include "compute.hpp"
#include <psapi.h>
#include <onnxruntime_cxx_api.h>
namespace asrwin {
Json diagnostics() {
    Json result={{"architecture","x64"},{"onnxruntime_version",Ort::GetVersionString()},
        {"compiled_runtime_distribution","Microsoft.ML.OnnxRuntime.DirectML 1.24.4"},
        {"available_providers",Ort::GetAvailableProviders()},{"default_provider",ASRWIN_DEFAULT_PROVIDER},
        {"directml_validated",bool(ASRWIN_DIRECTML_VALIDATED)},{"large_model_dir",ASRWIN_LARGE_MODEL_DIR},{"memory",memory_usage()}};
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    result["logical_processors"]=system.dwNumberOfProcessors;
    MEMORYSTATUSEX memory{};
    memory.dwLength=sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("Cannot read system memory");
    result["physical_memory_bytes"]=memory.ullTotalPhys;
    result["available_memory_bytes"]=memory.ullAvailPhys;
    result["graphics_adapters"]=Json::array();
    for (const auto& adapter:compute_adapters()) result["graphics_adapters"].push_back(adapter.to_json());
    std::vector<HMODULE> modules(256);
    DWORD bytes=0;
    if (!EnumProcessModules(GetCurrentProcess(),modules.data(),static_cast<DWORD>(modules.size()*sizeof(HMODULE)),&bytes))
        throw std::runtime_error("Cannot enumerate loaded dependencies");
    if (bytes>modules.size()*sizeof(HMODULE)) throw std::runtime_error("Loaded module inventory exceeds diagnostic capacity");
    std::array<wchar_t,32768> buffer{};
    result["loaded_modules"]=Json::array();
    for (size_t i=0;i<bytes/sizeof(HMODULE);++i) {
        DWORD length=GetModuleFileNameW(modules[i],buffer.data(),static_cast<DWORD>(buffer.size()));
        if (!length || length==buffer.size()) throw std::runtime_error("Cannot resolve loaded module path");
        result["loaded_modules"].push_back(utf8(std::wstring_view(buffer.data(),length)));
    }
    return result;
}
}
