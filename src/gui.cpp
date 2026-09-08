#include "gui.hpp"
#include "asr.hpp"
#include "capture.hpp"
#include "diagnostics.hpp"
#include "pipeline.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <set>
#include <thread>
namespace asrwin {
namespace {
enum : int { IDC_DEVICE=101, IDC_REFRESH, IDC_START, IDC_STOP, IDC_SAVE, IDC_REPORT, IDC_NOTICE, IDC_STATUS, IDC_PROVISIONAL, IDC_TRANSCRIPT };
enum : UINT { WM_APP_PROGRESS=WM_APP+1, WM_APP_LOADED, WM_APP_EVENT };
struct Loaded { std::unique_ptr<Engine> engine; std::string error; };
const wchar_t* NOTICE=L"Captures computer playback audio. Does not record the microphone.";

struct App {
  HINSTANCE instance=nullptr; GuiOptions options;
  HWND window=nullptr, device=nullptr, refresh=nullptr, start=nullptr, stop=nullptr, save=nullptr, report=nullptr, notice=nullptr, status=nullptr, provisional=nullptr, transcript=nullptr;
  HFONT ui_font=nullptr, caption_font=nullptr;
  std::unique_ptr<Engine> engine; std::unique_ptr<LivePipeline> pipeline; std::thread loader;
  std::vector<RenderDevice> devices; std::wstring device_name;
  uint64_t session=0, last_final=0, last_revision=0; std::set<uint64_t> finalized;
  std::wstring text;  // finalized transcript exactly as displayed
  bool loaded=false, listening=false, stopping=false, closing=false;
  Json last_statistics;

  void set_status(const std::wstring& value) { SetWindowTextW(status,value.c_str()); }
  void update_buttons() {
    EnableWindow(start,loaded && !listening && !stopping);
    EnableWindow(stop,listening && !stopping);
    EnableWindow(device,loaded && !listening && !stopping);
    EnableWindow(refresh,!listening && !stopping);
    EnableWindow(save,!text.empty());
    EnableWindow(report,loaded);
  }
  void fonts() {
    UINT dpi=GetDpiForWindow(window);
    if (ui_font) DeleteObject(ui_font); if (caption_font) DeleteObject(caption_font);
    ui_font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    caption_font=CreateFontW(-MulDiv(14,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for (HWND h:{device,refresh,start,stop,save,report,notice,status}) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(ui_font),TRUE);
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
  void append_transcript(const std::wstring& line) {
    text+=line+L"\r\n";
    int length=GetWindowTextLengthW(transcript);
    SendMessageW(transcript,EM_SETSEL,length,length);
    SendMessageW(transcript,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>((line+L"\r\n").c_str()));
    SendMessageW(transcript,EM_SCROLLCARET,0,0);
  }
  void begin_listening() {
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
  void save_report() {
    wchar_t path[MAX_PATH]=L"asrwin-report.json";
    OPENFILENAMEW ofn{sizeof(ofn)}; ofn.hwndOwner=window; ofn.lpstrFilter=L"JSON (*.json)\0*.json\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH; ofn.lpstrDefExt=L"json"; ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    try {
      Json body={{"engine",engine?engine->inspect():Json()},{"diagnostics",diagnostics()},{"session",session},{"device",utf8(device_name)},
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
      SendMessageW(app->transcript,EM_SETLIMITTEXT,0,0);
      app->fonts(); app->layout(); app->fill_devices(); app->update_buttons();
      app->loader=std::thread([app]{
        auto* result=new Loaded;
        try {
          EngineOptions o; o.threads=app->options.threads; o.max_tokens=app->options.max_tokens; o.variant=app->options.variant; o.directml=app->options.directml;
          o.verify_distribution=app->options.verify_distribution;
          o.progress=[app](const std::string& m){ PostMessageW(app->window,WM_APP_PROGRESS,0,reinterpret_cast<LPARAM>(new std::wstring(wide(m)))); };
          result->engine=std::make_unique<Engine>(app->options.model,o);
        } catch (const std::exception& e) { result->error=e.what(); }
        PostMessageW(app->window,WM_APP_LOADED,0,reinterpret_cast<LPARAM>(result));
      });
      return 0; }
    case WM_APP_PROGRESS: { std::unique_ptr<std::wstring> m(reinterpret_cast<std::wstring*>(lp)); if (!app->loaded) app->set_status(L"Loading model: "+*m+L"…"); return 0; }
    case WM_APP_LOADED: {
      std::unique_ptr<Loaded> result(reinterpret_cast<Loaded*>(lp));
      if (app->loader.joinable()) app->loader.join();
      if (result->engine) { app->engine=std::move(result->engine); app->loaded=true; app->set_status(L"Ready. Select a playback device and press Start."); }
      else { app->set_status(L"Error: "+wide(result->error)); MessageBoxW(h,wide(result->error).c_str(),L"AsrWin cannot load the model",MB_ICONERROR|MB_OK); }
      app->update_buttons();
      if (app->closing) DestroyWindow(h);
      return 0; }
    case WM_APP_EVENT: { std::unique_ptr<CaptionEvent> e(reinterpret_cast<CaptionEvent*>(lp)); app->on_event(*e); return 0; }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_START: if (HIWORD(wp)==BN_CLICKED) app->begin_listening(); return 0;
        case IDC_STOP: if (HIWORD(wp)==BN_CLICKED) app->request_stop("Stop requested"); return 0;
        case IDC_SAVE: if (HIWORD(wp)==BN_CLICKED) app->save_transcript(); return 0;
        case IDC_REPORT: if (HIWORD(wp)==BN_CLICKED) app->save_report(); return 0;
        case IDC_REFRESH: if (HIWORD(wp)==BN_CLICKED) app->fill_devices(); return 0;
      }
      break;
    case WM_SIZE: app->layout(); return 0;
    case WM_GETMINMAXINFO: { auto* info=reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize={640,360}; return 0; }
    case WM_DPICHANGED: { auto* rc=reinterpret_cast<RECT*>(lp); SetWindowPos(h,nullptr,rc->left,rc->top,rc->right-rc->left,rc->bottom-rc->top,SWP_NOZORDER|SWP_NOACTIVATE); app->fonts(); app->layout(); return 0; }
    case WM_CLOSE:
      if (app->listening) { app->closing=true; app->request_stop("Window closed"); return 0; }
      if (app->loader.joinable() && !app->loaded) { app->closing=true; app->set_status(L"Waiting for model loading to finish before exit…"); return 0; }
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
  WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc=window_proc; wc.hInstance=instance; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1); wc.lpszClassName=L"AsrWinMain"; wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
  if (!RegisterClassExW(&wc)) throw std::runtime_error("Cannot register window class");
  HWND h=CreateWindowExW(0,wc.lpszClassName,L"AsrWin — playback captions (test candidate)",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,640,nullptr,nullptr,instance,&app);
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
