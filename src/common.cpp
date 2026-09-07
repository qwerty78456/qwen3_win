#include "common.hpp"
#include <bcrypt.h>
#include <psapi.h>
#include <iomanip>
#include <sstream>
namespace asrwin {
void check(HRESULT hr, const char* operation) {
 if (FAILED(hr)) { std::ostringstream s; s<<operation<<" failed (0x"<<std::hex<<static_cast<unsigned long>(hr)<<")"; throw std::runtime_error(s.str()); }
}
std::string utf8(std::wstring_view v) {
 if(v.empty()) return {};
 int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),nullptr,0,nullptr,nullptr);
 if(!n) throw std::runtime_error("Invalid UTF-16");
 std::string s(n,'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),s.data(),n,nullptr,nullptr); return s;
}
std::wstring wide(std::string_view v) {
 if(v.empty()) return {};
 int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),nullptr,0);
 if(!n) throw std::runtime_error("Invalid UTF-8");
 std::wstring s(n,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,v.data(),static_cast<int>(v.size()),s.data(),n); return s;
}
std::string read_text(const fs::path& path) { std::ifstream f(path,std::ios::binary); if(!f) throw std::runtime_error("Cannot open "+utf8(path.wstring())); return {std::istreambuf_iterator<char>(f),{}}; }
void write_json(const fs::path& path,const Json& v) { if(!path.parent_path().empty()) fs::create_directories(path.parent_path()); std::ofstream f(path,std::ios::binary); if(!f || !(f<<v.dump(2)<<'\n')) throw std::runtime_error("Cannot write report"); }
std::vector<float> read_floats(const fs::path& path) { auto bytes=read_text(path); if(bytes.size()%4) throw std::runtime_error("Invalid float asset length"); std::vector<float> v(bytes.size()/4); memcpy(v.data(),bytes.data(),bytes.size()); return v; }
void write_floats(const fs::path& path,std::span<const float> v) { fs::create_directories(path.parent_path()); std::ofstream f(path,std::ios::binary); f.write(reinterpret_cast<const char*>(v.data()),v.size_bytes()); if(!f) throw std::runtime_error("Cannot write trace"); }
fs::path executable_dir() { std::vector<wchar_t> v(32768); auto n=GetModuleFileNameW(nullptr,v.data(),static_cast<DWORD>(v.size())); if(!n || n==v.size()) throw std::runtime_error("Cannot locate executable"); return fs::path(std::wstring(v.data(),n)).parent_path(); }
Json memory_usage() { PROCESS_MEMORY_COUNTERS_EX m{}; GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m)); return {{"working_set_bytes",m.WorkingSetSize},{"peak_working_set_bytes",m.PeakWorkingSetSize},{"private_bytes",m.PrivateUsage},{"peak_pagefile_bytes",m.PeakPagefileUsage}}; }
MappedFile::MappedFile(const fs::path& path) {
 file_=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file_==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open mapped asset: "+utf8(path.wstring()));
 LARGE_INTEGER n{}; if(!GetFileSizeEx(file_,&n) || n.QuadPart<=0) {CloseHandle(file_);throw std::runtime_error("Invalid mapped file size");} size_=static_cast<size_t>(n.QuadPart);
 mapping_=CreateFileMappingW(file_,nullptr,PAGE_READONLY,0,0,nullptr);
 if(mapping_) data_=MapViewOfFile(mapping_,FILE_MAP_READ,0,0,0);
 if(!data_) {if(mapping_) CloseHandle(mapping_);CloseHandle(file_);throw std::runtime_error("Cannot map asset");}
}
MappedFile::~MappedFile() { if(data_) UnmapViewOfFile(data_);if(mapping_) CloseHandle(mapping_);if(file_!=INVALID_HANDLE_VALUE) CloseHandle(file_); }
std::string sha256_file(const fs::path& path) {
 BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 unavailable");
 struct Guard { BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE& h; ~Guard(){if(h) BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);} } guard{alg,hash};
 if(BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("SHA256 initialization failed");
 std::ifstream f(path,std::ios::binary);if(!f) throw std::runtime_error("Missing asset: "+utf8(path.wstring()));
 std::vector<unsigned char> b(4*1024*1024);while(f){f.read(reinterpret_cast<char*>(b.data()),b.size());if(f.gcount() && BCryptHashData(hash,b.data(),static_cast<ULONG>(f.gcount()),0)<0) throw std::runtime_error("SHA256 read failed");}
 if(!f.eof()) throw std::runtime_error("Asset read failed");
 unsigned char digest[32]; if(BCryptFinishHash(hash,digest,32,0)<0) throw std::runtime_error("SHA256 failed");
 std::ostringstream s;for(auto c:digest)s<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(c);return s.str();
}
void verify_files(const fs::path& dir, const std::string& variant) {
 auto manifest=Json::parse(read_text(dir/"manifest.json"));
 std::vector<std::string> required_files={"config.json","tokenizer.json","prompt_reference.json","mel_filters.bin","encoder.onnx","embed_tokens.bin"};
 if(variant=="int4") required_files.insert(required_files.end(),{"decoder_init.int4.onnx","decoder_step.int4.onnx","decoder_weights.int4.data"});
 else required_files.insert(required_files.end(),{"decoder_init.onnx","decoder_step.onnx","decoder_weights.data"});
 for(const auto& required:required_files){
  bool found=false;for(const auto& item:manifest.at("files"))if(item.at("path")==required)found=true;
  if(!found)throw std::runtime_error("Model manifest omits required asset: "+required);
 }
 for(auto& item:manifest.at("files")) { auto rel=fs::path(wide(item.at("path").get<std::string>())); if(rel.is_absolute() || rel.string().find("..")!=std::string::npos)throw std::runtime_error("Invalid asset manifest path"); auto p=dir/rel;
 if(!fs::is_regular_file(p) || fs::file_size(p)!=item.at("bytes").get<uint64_t>() || sha256_file(p)!=item.at("sha256").get<std::string>())throw std::runtime_error("Missing or corrupt model asset: "+rel.string()); }
}
}
