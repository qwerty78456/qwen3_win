#include "compute.hpp"
#include <dxgi1_4.h>
#include <cstdio>
#include <map>
namespace asrwin {
namespace {
ComPtr<IDXGIFactory1> dxgi_factory() { ComPtr<IDXGIFactory1> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Enumerate graphics adapters"); return factory; }
std::string driver_string(LARGE_INTEGER v) {
  char b[64]; snprintf(b,sizeof b,"%u.%u.%u.%u",unsigned(v.HighPart>>16),unsigned(v.HighPart&0xFFFF),unsigned(v.LowPart>>16),unsigned(v.LowPart&0xFFFF)); return b;
}
bool digits(const std::wstring& s) { return !s.empty() && s.size()<=6 && std::all_of(s.begin(),s.end(),[](wchar_t c){ return c>=L'0' && c<=L'9'; }); }
}
std::string format_luid(LUID luid) { char b[32]; snprintf(b,sizeof b,"0x%08lX%08lX",static_cast<unsigned long>(luid.HighPart),luid.LowPart); return b; }
std::string megabytes(uint64_t bytes) {
  std::string n=std::to_string(bytes/1000000), out;
  for (size_t i=0;i<n.size();++i) { if (i && (n.size()-i)%3==0) out+=','; out+=n[i]; }
  return out+" MB";
}
Json ComputeAdapter::to_json() const {
  return {{"directml_index",index},{"name",name},{"luid",luid},{"driver_version",driver_version},{"vendor_id",vendor_id},{"device_id",device_id},
          {"dedicated_video_memory_bytes",dedicated_video_memory_bytes},{"shared_system_memory_bytes",shared_system_memory_bytes},{"software",software}};
}
std::vector<ComputeAdapter> compute_adapters() {
  auto factory=dxgi_factory(); std::vector<ComputeAdapter> out;
  for (UINT index=0;;++index) {
    ComPtr<IDXGIAdapter1> adapter; HRESULT hr=factory->EnumAdapters1(index,&adapter);
    if (hr==DXGI_ERROR_NOT_FOUND) break; check(hr,"Read graphics adapter");
    DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc),"Read graphics adapter properties");
    ComputeAdapter a; a.index=static_cast<int>(index); a.name=utf8(desc.Description); a.luid=format_luid(desc.AdapterLuid); a.vendor_id=desc.VendorId; a.device_id=desc.DeviceId;
    a.dedicated_video_memory_bytes=desc.DedicatedVideoMemory; a.shared_system_memory_bytes=desc.SharedSystemMemory; a.software=(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0;
    LARGE_INTEGER umd{}; if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&umd))) a.driver_version=driver_string(umd);
    out.push_back(std::move(a));
  }
  return out;
}
uint64_t required_adapter_memory(uint64_t model_weight_bytes, uint64_t minimum_bytes) { return std::max(minimum_bytes,static_cast<uint64_t>(std::ceil(double(model_weight_bytes)*kAdapterMemoryFactor))); }
std::vector<ComputeAdapter> eligible_adapters(const std::vector<ComputeAdapter>& all, uint64_t required_bytes) {
  std::vector<ComputeAdapter> out; std::vector<std::string> seen;
  for (auto& a:all) {
    if (a.software || std::find(seen.begin(),seen.end(),a.luid)!=seen.end()) continue;
    seen.push_back(a.luid);
    if (a.dedicated_video_memory_bytes>=required_bytes) out.push_back(a);
  }
  std::stable_sort(out.begin(),out.end(),[](const ComputeAdapter& x,const ComputeAdapter& y){ return x.dedicated_video_memory_bytes>y.dedicated_video_memory_bytes; });
  return out;
}
ComputeAdapter resolve_adapter(const std::wstring& selector, const ModelMemory& model, bool skip_memory_check, const std::string& model_label) {
  auto all=compute_adapters();
  uint64_t required=skip_memory_check?0:required_adapter_memory(model.weight_bytes,model.minimum_bytes);
  if (selector.empty() || selector==L"default") {
    auto eligible=eligible_adapters(all,required);
    if (eligible.empty()) throw std::runtime_error("No eligible GPU adapter for DirectML: "+std::to_string(all.size())+" adapters enumerated, none is a hardware adapter with at least "+megabytes(required)+" of dedicated video memory (see --adapters); CPU remains available with --provider cpu");
    return eligible.front();
  }
  if (selector[0]==L'-') throw std::runtime_error("Adapter index must not be negative (see --adapters)");
  if (!digits(selector)) throw std::runtime_error("Invalid --adapter value: expected 'default' or a DXGI adapter index (see --adapters)");
  size_t index=static_cast<size_t>(std::stoul(selector));
  if (index>=all.size()) throw std::runtime_error("Adapter index "+std::to_string(index)+" does not exist: "+std::to_string(all.size())+" adapters enumerated (see --adapters)");
  const auto& a=all[index];
  if (a.software) throw std::runtime_error("Adapter "+std::to_string(index)+" ("+a.name+") is a software renderer and cannot run DirectML inference (see --adapters)");
  if (a.dedicated_video_memory_bytes<required)
    throw std::runtime_error("Adapter "+std::to_string(index)+" ("+a.name+") has "+megabytes(a.dedicated_video_memory_bytes)+" of dedicated video memory; "+model_label+" needs at least "+megabytes(required)+" (graphs "+megabytes(model.weight_bytes)+" x 1.5, configured minimum "+megabytes(model.minimum_bytes)+"); use --skip-adapter-check to override for experiments");
  return a;
}
Json describe_adapters(const std::vector<ModelMemory>& models, bool skip_memory_check) {
  auto all=compute_adapters();
  Json result={{"memory_factor",kAdapterMemoryFactor},{"skip_memory_check",skip_memory_check},{"models",Json::array()},{"adapters",Json::array()}};
  for (auto& m:models) result["models"].push_back({{"name",m.name},{"weight_bytes",m.weight_bytes},{"minimum_dedicated_video_memory_bytes",m.minimum_bytes},{"required_dedicated_video_memory_bytes",required_adapter_memory(m.weight_bytes,m.minimum_bytes)}});
  std::map<std::string,int> first_index; std::string default_luid;  // LUID -> lowest hardware index
  if (!models.empty()) { auto e=eligible_adapters(all,skip_memory_check?0:required_adapter_memory(models.front().weight_bytes,models.front().minimum_bytes)); if (!e.empty()) default_luid=e.front().luid; }
  for (auto& a:all) {
    Json row=a.to_json(); Json eligible_models=Json::array(); Json duplicate_of; std::string reason;
    if (a.software) reason="software renderer";
    else if (auto it=first_index.find(a.luid); it!=first_index.end()) { reason="same adapter as index "+std::to_string(it->second)+" (LUID)"; duplicate_of=it->second; }
    else {
      first_index[a.luid]=a.index;
      for (auto& m:models) if (skip_memory_check || a.dedicated_video_memory_bytes>=required_adapter_memory(m.weight_bytes,m.minimum_bytes)) eligible_models.push_back(m.name);
      if (eligible_models.empty() && !models.empty()) reason="dedicated video memory below "+megabytes(required_adapter_memory(models.front().weight_bytes,models.front().minimum_bytes));
    }
    row["eligible"]=!eligible_models.empty(); row["eligible_models"]=eligible_models; row["reason"]=reason; row["duplicate_of"]=duplicate_of;
    row["default"]=!default_luid.empty() && a.luid==default_luid && reason.empty();
    result["adapters"].push_back(row);
  }
  if (models.empty()) result["note"]="No model manifest was readable; memory eligibility not evaluated";
  return result;
}
Json gpu_memory_usage(int index) {
  try {
    if (index<0) return Json();
    auto factory=dxgi_factory(); ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(index),&adapter))) return Json();
    ComPtr<IDXGIAdapter3> adapter3; if (FAILED(adapter.As(&adapter3))) return Json();
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local))) return Json();
    if (FAILED(adapter3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal))) nonlocal={};
    return {{"local_usage_bytes",local.CurrentUsage},{"local_budget_bytes",local.Budget},{"nonlocal_usage_bytes",nonlocal.CurrentUsage},{"nonlocal_budget_bytes",nonlocal.Budget}};
  } catch (...) { return Json(); }
}
}
