#include "gui.hpp"
#include "asr.hpp"
#include "capture.hpp"
#include "compute.hpp"
#include "diagnostics.hpp"
#include "pipeline.hpp"
#include "settings.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <functional>
#include <set>
#include <thread>
namespace asrwin {
namespace {
// Control IDs are mirrored positionally by scripts/gui_test.py: only ever append new IDs at the end.
enum : int { IDC_DEVICE=101, IDC_REFRESH, IDC_START, IDC_STOP, IDC_SAVE, IDC_REPORT, IDC_NOTICE, IDC_STATUS, IDC_PROVISIONAL, IDC_TRANSCRIPT, IDC_COMPUTE };
enum : UINT { WM_APP_PROGRESS=WM_APP+1, WM_APP_LOADED, WM_APP_EVENT };
// One entry of the compute list: the CPU, or a DirectML adapter running the default or the large model.
struct ComputeChoice { bool directml=false; int adapter=-1; ComputeAdapter info; bool large_model=false; std::wstring label; };
struct Loaded { std::unique_ptr<Engine> engine; std::string error; int choice=0; double seconds=0; };
const wchar_t* NOTICE=L"Captures computer playback audio. Does not record the microphone.";
std::wstring gigabytes(uint64_t bytes) { wchar_t b[32]; swprintf_s(b,L"%.0f",double(bytes)/1e9); return b; }

struct App {
  HINSTANCE instance=nullptr; GuiOptions options;
  HWND window=nullptr, device=nullptr, refresh=nullptr, start=nullptr, stop=nullptr, save=nullptr, report=nullptr, notice=nullptr, status=nullptr, provisional=nullptr, transcript=nullptr, compute=nullptr;
  HFONT ui_font=nullptr, caption_font=nullptr;
  std::unique_ptr<Engine> engine; std::unique_ptr<LivePipeline> pipeline; std::thread loader;
  std::vector<RenderDevice> devices; std::wstring device_name;
  std::vector<ComputeChoice> choices; int selected_compute=-1, loaded_compute=-1;  // combo index being loaded / currently loaded
  uint64_t session=0, last_final=0, last_revision=0; std::set<uint64_t> finalized;
  std::wstring text;  // finalized transcript exactly as displayed
  bool loaded=false, loading=false, listening=false, stopping=false, closing=false;
  std::wstring startup_note; std::string last_load_error, choice_source="default";
  fs::path settings_path; std::string settings_warning; bool settings_saved=false;
  Json last_statistics;

  void set_status(const std::wstring& value) { SetWindowTextW(status,value.c_str()); }
  void update_buttons() {
    EnableWindow(start,loaded && !listening && !stopping);
    EnableWindow(stop,listening && !stopping);
    EnableWindow(device,loaded && !listening && !stopping);
    EnableWindow(refresh,!listening && !stopping && !loading);
    EnableWindow(compute,!loading && !listening && !stopping);  // stays usable after a failed GPU load so CPU can be selected
    EnableWindow(save,!text.empty());
    EnableWindow(report,!loading);
  }
  void fonts() {
    UINT dpi=GetDpiForWindow(window);
    if (ui_font) DeleteObject(ui_font); if (caption_font) DeleteObject(caption_font);
    ui_font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    caption_font=CreateFontW(-MulDiv(14,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for (HWND h:{device,refresh,start,stop,save,report,notice,status,compute}) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
    for (HWND h:{provisional,transcript}) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(caption_font),TRUE);
  }
  void layout() {
    RECT rc; GetClientRect(window,&rc);
    UINT dpi=GetDpiForWindow(window);
    auto s=[&](int v){ return MulDiv(v,dpi,96); };
    int x=s(8), y=s(8), w=rc.right-s(16), h=s(26);
    int buttons=s(70)+s(80)*3+s(90)+s(8)*5;
    SetWindowPos(device,nullptr,x,y,std::max(s(120),w-buttons),s(300),SWP_NOZORDER);
    int bx=rc.right-s(8)-s(90); SetWindowPos(report,nullptr,bx,y,s(90),h,SWP_NOZORDER);
    bx-=s(8)+s(80); SetWindowPos(save,nullptr,bx,y,s(80),h,SWP_NOZORDER);
    bx-=s(8)+s(80); SetWindowPos(stop,nullptr,bx,y,s(80),h,SWP_NOZORDER);
    bx-=s(8)+s(80); SetWindowPos(start,nullptr,bx,y,s(80),h,SWP_NOZORDER);
    bx-=s(8)+s(70); SetWindowPos(refresh,nullptr,bx,y,s(70),h,SWP_NOZORDER);
    y+=h+s(8); SetWindowPos(compute,nullptr,x,y,std::min(w,s(560)),s(300),SWP_NOZORDER);  // compute list on its own row
    y+=h+s(8); SetWindowPos(notice,nullptr,x,y,w,s(20),SWP_NOZORDER);
    y+=s(22); SetWindowPos(status,nullptr,x,y,w,s(20),SWP_NOZORDER);
    y+=s(26); SetWindowPos(provisional,nullptr,x,y,w,s(84),SWP_NOZORDER);
    y+=s(92); SetWindowPos(transcript,nullptr,x,y,w,std::max(s(60),int(rc.bottom)-y-s(8)),SWP_NOZORDER);
  }
  void fill_devices() {
    SendMessageW(device,CB_RESETCONTENT,0,0);
    SendMessageW(device,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Default playback device"));
    try { devices=render_devices(); } catch (const std::exception& e) { devices.clear(); set_status(L"Cannot list playback devices: "+wide(e.what())); }
    for (auto& d:devices) { std::wstring label=d.name+(d.is_default?L"  (current default)":L""); SendMessageW(device,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str())); }
    SendMessageW(device,CB_SETCURSEL,0,0);
  }
  // CPU first, then one item per eligible adapter for the default model and (when the adapter has the memory) the large model.
  void fill_compute() {
    SendMessageW(compute,CB_RESETCONTENT,0,0); choices.clear();
    std::wstring base=wide(model_short_name(options.model));
    choices.push_back({false,-1,{},false,L"CPU — "+base+L" ("+std::to_wstring(options.threads)+L" threads)"});
    if (ASRWIN_DIRECTML_VALIDATED || options.experimental) {
      try {
        uint64_t weights=model_weight_bytes(options.model,options.variant);
        std::optional<uint64_t> large_weights; std::wstring large;
        if (!options.large_model_dir.empty() && fs::exists(options.large_model_dir/"manifest.json")) { large_weights=model_weight_bytes(options.large_model_dir,"fp32"); large=wide(model_short_name(options.large_model_dir)); }
        auto all=compute_adapters();
        auto eligible=eligible_adapters(all,options.skip_adapter_check?0:required_adapter_memory(weights,ASRWIN_MIN_VRAM_BYTES));
        const std::wstring experimental=ASRWIN_DIRECTML_VALIDATED?L"":L" (experimental)";
        for (auto& a:eligible) {
          std::wstring gpu=L"GPU: "+wide(a.name)+L" ("+gigabytes(a.dedicated_video_memory_bytes)+L" GB)";
          if (std::count_if(eligible.begin(),eligible.end(),[&](const ComputeAdapter& o){ return o.name==a.name; })>1) gpu+=L" [adapter "+std::to_wstring(a.index)+L"]";
          choices.push_back({true,a.index,a,false,gpu+L" — "+base+experimental});
          if (large_weights && (options.skip_adapter_check || a.dedicated_video_memory_bytes>=required_adapter_memory(*large_weights,ASRWIN_LARGE_MODEL_MIN_VRAM_BYTES)))
            choices.push_back({true,a.index,a,true,gpu+L" — "+large+L" (higher accuracy)"+experimental});
        }
      } catch (const std::exception& e) { startup_note=L" GPU list unavailable: "+wide(e.what()); }
    }
    for (auto& c:choices) SendMessageW(compute,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(c.label.c_str()));
    SendMessageW(compute,CB_SETCURSEL,0,0);
  }
  int find_choice(const std::function<bool(const ComputeChoice&)>& match) const {
    for (size_t i=1;i<choices.size();++i) if (match(choices[i])) return static_cast<int>(i);
    return -1;
  }
  // Command line > saved settings > compiled default. Returns the combo index to load first.
  int initial_compute() {
    if (options.provider_from_cli) {
      choice_source="command-line";
      if (!options.directml) return 0;
      auto selector=options.adapter_selector;
      int index=find_choice([&](const ComputeChoice& c){
        if (c.large_model!=options.large_model) return false;
        if (selector.empty() || selector==L"default") return true;
        return std::to_wstring(c.adapter)==selector; });
      if (index<0) startup_note=L" GPU not used: no eligible adapter matches --adapter "+(selector.empty()?L"default":selector)+(options.large_model?L" for the large model":L"")+L"; using CPU.";
      return std::max(index,0);
    }
    if (options.use_settings) {
      std::string warning; auto saved=load_settings(settings_path,warning);
      if (!warning.empty()) { settings_warning=warning; startup_note=L" Settings ignored ("+wide(warning)+L")."; return 0; }
      if (saved && saved->provider=="directml") {
        choice_source="settings";
        int index=find_choice([&](const ComputeChoice& c){ return c.info.luid==saved->adapter_luid && c.large_model==saved->large_model; });
        if (index<0 && saved->large_model) { index=find_choice([&](const ComputeChoice& c){ return c.info.luid==saved->adapter_luid && !c.large_model; }); if (index>=0) startup_note=L" The large model is no longer available on the saved GPU; using the default model."; }
        if (index<0) startup_note=L" Saved GPU ("+wide(saved->adapter_name)+L") is not available; using CPU.";
        return std::max(index,0);
      }
      if (saved) choice_source="settings";
    }
    if (std::string(ASRWIN_DEFAULT_PROVIDER)=="directml") { int index=find_choice([](const ComputeChoice& c){ return !c.large_model; }); if (index>0) return index; }
    return 0;
  }
  // Replace the engine with the selected compute choice. The pipeline holds an Engine& and must go first.
  void start_loading(int choice) {
    if (loading || listening || stopping || choice<0 || choice>=static_cast<int>(choices.size())) return;
    const ComputeChoice c=choices[static_cast<size_t>(choice)];
    pipeline.reset(); auto old=std::move(engine);
    loaded=false; loading=true; selected_compute=choice; loaded_compute=-1; last_load_error.clear();
    SendMessageW(compute,CB_SETCURSEL,choice,0);
    set_status(L"Loading model ("+c.label+L")…"); update_buttons();
    if (loader.joinable()) loader.join();
    fs::path model=c.large_model?options.large_model_dir:options.model; int threads=c.large_model?options.large_model_threads:options.threads;
    loader=std::thread([this,old=std::move(old),c,model,threads,choice]() mutable {
      old.reset();  // release the previous model on this thread so the window stays responsive
      auto* result=new Loaded; result->choice=choice; auto started=Clock::now();
      try {
        EngineOptions o; o.threads=threads; o.max_tokens=options.max_tokens; o.variant=c.large_model?"fp32":options.variant; o.directml=c.directml; o.adapter=c.adapter; o.large_model=c.large_model;
        o.verify_distribution=options.verify_distribution;
        o.progress=[this](const std::string& m){ PostMessageW(window,WM_APP_PROGRESS,0,reinterpret_cast<LPARAM>(new std::wstring(wide(m)))); };
        result->engine=std::make_unique<Engine>(model,o);
        if (c.directml) { o.progress("Warming up the GPU"); result->engine->warm_up(); }
      } catch (const std::exception& e) { result->error=e.what(); }
      result->seconds=seconds(started);
      PostMessageW(window,WM_APP_LOADED,0,reinterpret_cast<LPARAM>(result));
    });
  }
  void on_loaded(Loaded& result) {
    const ComputeChoice& c=choices[static_cast<size_t>(result.choice)];
    if (result.engine) {
      engine=std::move(result.engine); loaded=true; loaded_compute=result.choice;
      std::wstring note=startup_note; startup_note.clear();
      if (options.use_settings) {
        Settings s; s.provider=c.directml?"directml":"cpu"; s.adapter_luid=c.info.luid; s.adapter_name=c.info.name; s.large_model=c.large_model;
        try { save_settings(settings_path,s); settings_saved=true; settings_warning.clear(); } catch (const std::exception& e) { settings_warning=e.what(); note+=L" Settings could not be saved: "+wide(e.what()); }
      }
      set_status(L"Ready ("+c.label+L"). Select a playback device and press Start."+note);
    } else {
      last_load_error=result.error;
      set_status(L"Error: "+wide(result.error)+(c.directml?L" — select CPU in the compute list to continue.":L""));
      MessageBoxW(window,wide(result.error).c_str(),c.directml?L"AsrWin — GPU unavailable":L"AsrWin cannot load the model",MB_ICONERROR|MB_OK);
    }
  }
  void on_compute_changed() {
    int selected=static_cast<int>(SendMessageW(compute,CB_GETCURSEL,0,0));
    if (selected<0 || selected>=static_cast<int>(choices.size())) return;
    if (loading || listening || stopping) { SendMessageW(compute,CB_SETCURSEL,selected_compute,0); return; }  // posted commands bypass EnableWindow
    if (selected==selected_compute && loaded) return;
    start_loading(selected);
  }
  void append_transcript(const std::wstring& line) {
    text+=line+L"\r\n";
    int length=GetWindowTextLengthW(transcript);
    SendMessageW(transcript,EM_SETSEL,length,length);
    SendMessageW(transcript,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>((line+L"\r\n").c_str()));
    SendMessageW(transcript,EM_SCROLLCARET,0,0);
  }
  void begin_listening() {
    if (!loaded || !engine || loading || listening) return;  // posted commands bypass EnableWindow
    int selected=static_cast<int>(SendMessageW(device,CB_GETCURSEL,0,0));
    std::wstring id=selected>0 && selected-1<int(devices.size())?devices[selected-1].id:L"";
    try {
      if (!pipeline) {
        HWND target=window;
        pipeline=std::make_unique<LivePipeline>(*engine,[target](const CaptionEvent& e){ PostMessageW(target,WM_APP_EVENT,0,reinterpret_cast<LPARAM>(new CaptionEvent(e))); });
      }
      ++session; last_final=0; last_revision=0; finalized.clear();
      auto* p=pipeline.get();
      CaptureEvents events{[p](const std::wstring& reason){ p->stop(utf8(reason)); },
                           [p]{ p->stop("Capture buffer reached its stop threshold"); },
                           [p](const std::string& error){ p->stop("Capture failed: "+error); }};
      auto source=std::make_unique<LoopbackCapture>(id,events);
      device_name=source->name();
      SetWindowTextW(provisional,L"");
      pipeline->start(std::move(source),session);
      listening=true; stopping=false;
      set_status(L"Listening: "+device_name);
    } catch (const std::exception& e) {
      set_status(L"Error: "+wide(e.what()));
      MessageBoxW(window,wide(e.what()).c_str(),L"AsrWin",MB_ICONERROR|MB_OK);
    }
    update_buttons();
  }
  void request_stop(const char* reason) { if (pipeline && listening && !stopping) { stopping=true; set_status(L"Stopping…"); pipeline->stop(reason); update_buttons(); } }
  void on_event(const CaptionEvent& e) {
    if (e.session!=session) return;  // stale session
    switch (e.kind) {
      case CaptionEvent::Kind::Status:
        if (e.status.rfind("Stopping",0)==0) stopping=true;
        if (!stopping || e.status.rfind("Stopping",0)==0) set_status(wide(e.status)+(e.status=="Listening"?L": "+device_name:L""));
        break;
      case CaptionEvent::Kind::Provisional:
        if (stopping || e.utterance<=last_final || finalized.count(e.utterance) || e.revision<last_revision) return;  // stale
        last_revision=e.revision;
        SetWindowTextW(provisional,wide(e.text+(e.incomplete?"  …":"")).c_str());
        break;
      case CaptionEvent::Kind::Final: {
        if (finalized.count(e.utterance)) return;  // duplicate finalization is never displayed twice
        finalized.insert(e.utterance); last_final=std::max(last_final,e.utterance);
        SetWindowTextW(provisional,L"");
        if (e.completion=="error") { append_transcript(L"[inference error: "+wide(e.status)+L"]"); set_status(L"Error: inference failed"); }
        else if (e.incomplete) { append_transcript(wide(e.text)+L" [incomplete: "+wide(e.completion)+L"]"); set_status(L"Warning: utterance hit the token limit; shown as incomplete"); }
        else if (!e.text.empty()) append_transcript(wide(e.text));
        break; }
      case CaptionEvent::Kind::Error: set_status(L"Error: "+wide(e.status)); break;
      case CaptionEvent::Kind::Stopped:
        if (pipeline) pipeline->wait();
        last_statistics=e.detail; listening=false; stopping=false;
        SetWindowTextW(provisional,L"");
        set_status(L"Stopped: "+wide(e.status));
        if (closing) DestroyWindow(window);
        break;
    }
    update_buttons();
  }
  void save_transcript() {
    wchar_t path[MAX_PATH]=L"transcript.txt";
    OPENFILENAMEW ofn{sizeof(ofn)}; ofn.hwndOwner=window; ofn.lpstrFilter=L"UTF-8 text (*.txt)\0*.txt\0All files\0*.*\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH; ofn.lpstrDefExt=L"txt"; ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    try { std::ofstream f(path,std::ios::binary); std::string body=utf8(text); f.write("\xEF\xBB\xBF",3); f.write(body.data(),body.size()); if (!f) throw std::runtime_error("Cannot write transcript"); set_status(L"Saved transcript: "+std::wstring(path)); }
    catch (const std::exception& e) { MessageBoxW(window,wide(e.what()).c_str(),L"AsrWin",MB_ICONERROR|MB_OK); }
  }
  // Diagnostic report: engine, machine, compute selection and pipeline statistics. Never any caption text.
  void save_report() {
    wchar_t path[MAX_PATH]=L"asrwin-report.json";
    OPENFILENAMEW ofn{sizeof(ofn)}; ofn.hwndOwner=window; ofn.lpstrFilter=L"JSON (*.json)\0*.json\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH; ofn.lpstrDefExt=L"json"; ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    try {
      Json selection;
      if (selected_compute>=0 && selected_compute<static_cast<int>(choices.size())) {
        const auto& c=choices[static_cast<size_t>(selected_compute)];
        selection={{"label",utf8(c.label)},{"provider",c.directml?"directml":"cpu"},{"adapter",c.directml?c.info.to_json():Json()},{"large_model",c.large_model},{"loaded",loaded && loaded_compute==selected_compute}};
      }
      selection["source"]=choice_source; selection["last_load_error"]=last_load_error; selection["choices"]=Json::array();
      for (auto& c:choices) selection["choices"].push_back(utf8(c.label));
      Json settings={{"path",utf8(settings_path.wstring())},{"enabled",options.use_settings},{"saved",settings_saved},{"warning",settings_warning}};
      Json body={{"engine",engine?engine->inspect():Json()},{"gpu_memory",engine?engine->gpu_memory():Json()},{"diagnostics",diagnostics()},{"compute",selection},{"settings",settings},{"session",session},{"device",utf8(device_name)},
                 {"pipeline",pipeline&&listening?pipeline->statistics():last_statistics},{"finalized_utterances",finalized.size()}};
      write_json(path,body); set_status(L"Saved diagnostic report: "+std::wstring(path));
    } catch (const std::exception& e) { MessageBoxW(window,wide(e.what()).c_str(),L"AsrWin",MB_ICONERROR|MB_OK); }
  }
};
App* app_of(HWND h) { return reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA)); }
LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  App* app=app_of(h);
  switch (msg) {
    case WM_NCCREATE: SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams)); return DefWindowProcW(h,msg,wp,lp);
    case WM_CREATE: {
      app->window=h;
      auto make=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id,DWORD ex=0){ return CreateWindowExW(ex,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,h,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),app->instance,nullptr); };
      app->device=make(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,IDC_DEVICE);
      app->refresh=make(L"BUTTON",L"Refresh",WS_TABSTOP,IDC_REFRESH);
      app->start=make(L"BUTTON",L"Start",WS_TABSTOP|BS_DEFPUSHBUTTON,IDC_START);
      app->stop=make(L"BUTTON",L"Stop",WS_TABSTOP,IDC_STOP);
      app->save=make(L"BUTTON",L"Save…",WS_TABSTOP,IDC_SAVE);
      app->report=make(L"BUTTON",L"Report…",WS_TABSTOP,IDC_REPORT);
      app->notice=make(L"STATIC",NOTICE,SS_LEFT,IDC_NOTICE);
      app->status=make(L"STATIC",L"Loading model…",SS_LEFT|SS_ENDELLIPSIS,IDC_STATUS);
      app->provisional=make(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,IDC_PROVISIONAL,WS_EX_CLIENTEDGE);
      app->transcript=make(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,IDC_TRANSCRIPT,WS_EX_CLIENTEDGE);
      app->compute=make(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,IDC_COMPUTE);
      SendMessageW(app->transcript,EM_SETLIMITTEXT,0,0);
      app->fonts(); app->layout(); app->fill_devices(); app->fill_compute(); app->update_buttons();
      app->start_loading(app->initial_compute());
      return 0; }
    case WM_APP_PROGRESS: { std::unique_ptr<std::wstring> m(reinterpret_cast<std::wstring*>(lp)); if (app->loading) app->set_status(L"Loading model: "+*m+L"…"); return 0; }
    case WM_APP_LOADED: {
      std::unique_ptr<Loaded> result(reinterpret_cast<Loaded*>(lp));
      if (app->loader.joinable()) app->loader.join();
      app->loading=false;
      if (app->closing) { DestroyWindow(h); return 0; }
      app->on_loaded(*result);
      app->update_buttons();
      return 0; }
    case WM_APP_EVENT: { std::unique_ptr<CaptionEvent> e(reinterpret_cast<CaptionEvent*>(lp)); app->on_event(*e); return 0; }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_START: if (HIWORD(wp)==BN_CLICKED) app->begin_listening(); return 0;
        case IDC_STOP: if (HIWORD(wp)==BN_CLICKED) app->request_stop("Stop requested"); return 0;
        case IDC_SAVE: if (HIWORD(wp)==BN_CLICKED) app->save_transcript(); return 0;
        case IDC_REPORT: if (HIWORD(wp)==BN_CLICKED) app->save_report(); return 0;
        case IDC_REFRESH: if (HIWORD(wp)==BN_CLICKED) app->fill_devices(); return 0;
        case IDC_COMPUTE: if (HIWORD(wp)==CBN_SELCHANGE) app->on_compute_changed(); return 0;
      }
      break;
    case WM_SIZE: app->layout(); return 0;
    case WM_GETMINMAXINFO: { auto* info=reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize={640,400}; return 0; }
    case WM_DPICHANGED: { auto* rc=reinterpret_cast<RECT*>(lp); SetWindowPos(h,nullptr,rc->left,rc->top,rc->right-rc->left,rc->bottom-rc->top,SWP_NOZORDER|SWP_NOACTIVATE); app->fonts(); app->layout(); return 0; }
    case WM_CLOSE:
      if (app->listening) { app->closing=true; app->request_stop("Window closed"); return 0; }
      if (app->loading) { app->closing=true; app->set_status(L"Waiting for model loading to finish before exit…"); return 0; }
      DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h,msg,wp,lp);
}
}
int run_gui(HINSTANCE instance, const GuiOptions& options) {
  HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) throw std::runtime_error("Cannot initialize COM for the interface");
  INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
  App app; app.instance=instance; app.options=options;
  if (options.use_settings) { try { app.settings_path=options.settings_path.empty()?default_settings_path():options.settings_path; } catch (const std::exception& e) { app.options.use_settings=false; app.settings_warning=e.what(); app.startup_note=L" Settings unavailable: "+wide(e.what()); } }
  WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc=window_proc; wc.hInstance=instance; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1); wc.lpszClassName=L"AsrWinMain"; wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
  if (!RegisterClassExW(&wc)) throw std::runtime_error("Cannot register window class");
  HWND h=CreateWindowExW(0,wc.lpszClassName,L"AsrWin — playback captions (test candidate)",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,680,nullptr,nullptr,instance,&app);
  if (!h) throw std::runtime_error("Cannot create main window");
  ShowWindow(h,SW_SHOWNORMAL); UpdateWindow(h);
  MSG msg;
  while (GetMessageW(&msg,nullptr,0,0)>0) { if (!IsDialogMessageW(h,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
  app.pipeline.reset(); app.engine.reset();
  if (app.loader.joinable()) app.loader.join();
  if (app.ui_font) DeleteObject(app.ui_font); if (app.caption_font) DeleteObject(app.caption_font);
  CoUninitialize();
  return static_cast<int>(msg.wParam);
}
}
