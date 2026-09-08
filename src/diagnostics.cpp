#include "diagnostics.hpp"
#include <psapi.h>
#include <onnxruntime_cxx_api.h>
#include <dxgi1_2.h>
namespace asrwin {
Json diagnostics() {
    Json result={{"architecture","x64"},{"onnxruntime_version",Ort::GetVersionString()},
        {"compiled_runtime_distribution","Microsoft.ML.OnnxRuntime.DirectML 1.24.4"},
        {"available_providers",Ort::GetAvailableProviders()},{"enabled_provider","CPU"},
        {"directml_validated",false},{"memory",memory_usage()}};
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    result["logical_processors"]=system.dwNumberOfProcessors;
    MEMORYSTATUSEX memory{};
    memory.dwLength=sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("Cannot read system memory");
    result["physical_memory_bytes"]=memory.ullTotalPhys;
    result["available_memory_bytes"]=memory.ullAvailPhys;
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Enumerate graphics adapters");
    result["graphics_adapters"]=Json::array();
    for(UINT index=0;;++index) {
        ComPtr<IDXGIAdapter1> adapter;HRESULT hr=factory->EnumAdapters1(index,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"Read graphics adapter");
        DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Read graphics adapter properties");
        result["graphics_adapters"].push_back({{"directml_index",index},{"name",utf8(desc.Description)},{"vendor_id",desc.VendorId},
            {"device_id",desc.DeviceId},{"dedicated_video_memory_bytes",desc.DedicatedVideoMemory},{"software",bool(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)}});
    }
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
