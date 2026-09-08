#pragma once
#include "common.hpp"
namespace asrwin {
// A DXGI adapter as seen by IDXGIFactory1::EnumAdapters1. The index is the DirectML device_id
// (dml_provider_factory.h: "the enumeration order of hardware adapters as given by IDXGIFactory::EnumAdapters").
struct ComputeAdapter {
  int index=0;
  std::string name, luid, driver_version;
  uint32_t vendor_id=0, device_id=0;
  uint64_t dedicated_video_memory_bytes=0, shared_system_memory_bytes=0;
  bool software=false;
  Json to_json() const;
};
// Dedicated video memory required for a model = max(graph bytes (encoder + decoder graphs and weights) × this factor,
// the model's configured minimum from shipped-model.json, which comes from the peak GPU memory measured in reports/directml.json).
constexpr double kAdapterMemoryFactor=1.5;
struct ModelMemory { std::string name; uint64_t weight_bytes=0; uint64_t minimum_bytes=0; };
std::vector<ComputeAdapter> compute_adapters();                                   // every adapter, raw DXGI order
uint64_t required_adapter_memory(uint64_t model_weight_bytes, uint64_t minimum_bytes=0);
// Hardware adapters only, one per LUID (lowest index kept), dedicated memory >= required, largest memory first.
std::vector<ComputeAdapter> eligible_adapters(const std::vector<ComputeAdapter>& all, uint64_t required_bytes);
// "default" (or empty) = first eligible adapter; otherwise a DXGI index that must be a hardware adapter with enough memory.
ComputeAdapter resolve_adapter(const std::wstring& selector, const ModelMemory& model, bool skip_memory_check=false, const std::string& model_label="the shipped model");
// Payload of --adapters: eligibility per model.
Json describe_adapters(const std::vector<ModelMemory>& models, bool skip_memory_check=false);
// This process's video memory usage on an adapter (IDXGIAdapter3::QueryVideoMemoryInfo); null on failure, never throws.
Json gpu_memory_usage(int index);
std::string format_luid(LUID luid);
std::string megabytes(uint64_t bytes);  // "5,630 MB"
}
