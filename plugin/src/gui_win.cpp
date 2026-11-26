// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 mtzvfilm. PRO8 - https://github.com/mtzvfilm/PRO8
// plugin/src/gui.h for Windows: the panel (embedded core/ui.html) in a WebView2 control inside a child window of the host.
// Needs the WebView2 Runtime (part of Windows 10/11 with Edge) and the WebView2 SDK (fetched by plugin/CMakeLists.txt).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "gui.h"
#include "pro8_embedded.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace pro8gui {

static const UINT WM_P8_JS = WM_APP + 0x38;
static const UINT WM_P8_MSG = WM_APP + 0x39;   // a panel message, handled outside the WebView2 event (file dialogs are modal)
static const wchar_t* CLASS_NAME = L"PRO8PanelWindow";

static std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w;
}
static std::string utf8(const wchar_t* w) {
    if (!w || !*w) return "";
    int len = (int)wcslen(w), n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr); return s;
}

// JavaScript calls from any thread (the core calls js() from the audio thread too) go through this queue
// and are run on the UI thread when the window gets WM_P8_JS
struct JsQueue { std::mutex m; std::vector<std::string> q; HWND hwnd = nullptr; };

struct View {
    pro8::Core* core = nullptr;
    std::shared_ptr<JsQueue> jq;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> wv;
    uint32_t w = 1280, h = 784;
    bool visible = true;
    std::vector<std::string> inbox;             // panel messages (UI thread only)
    std::shared_ptr<std::atomic<bool>> alive;   // async WebView2 callbacks check it after destroy()
};

const char* window_api() { return CLAP_WINDOW_API_WIN32; }

static HINSTANCE module_instance() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(&module_instance)), &m);
    return (HINSTANCE)m;
}

static void run_js(View* v) {
    std::vector<std::string> q;
    { std::lock_guard<std::mutex> l(v->jq->m); q.swap(v->jq->q); }
    if (!v->wv) return;   // the panel is not loaded yet: it asks for the whole state ('ready') once it is
    for (auto& s : q) v->wv->ExecuteScript(wide(s).c_str(), nullptr);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    View* v = (View*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_P8_JS: if (v) run_js(v); return 0;
    case WM_P8_MSG:
        if (v) { std::vector<std::string> q; q.swap(v->inbox); for (auto& m : q) if (v->core) v->core->handle_json(m); }
        return 0;
    case WM_SIZE:
        if (v && v->ctl) { RECT r; GetClientRect(hwnd, &r); v->ctl->put_Bounds(r); }
        return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool register_class() {
    static bool done = false; if (done) return true;
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof wc; wc.lpfnWndProc = wndproc; wc.hInstance = module_instance();
    wc.lpszClassName = CLASS_NAME; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    done = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return done;
}

static std::wstring user_data_dir() {   // WebView2 needs a writable profile folder: %LOCALAPPDATA%\PRO8\WebView2 (not next to the DAW's .exe)
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = std::wstring(p) + L"\\PRO8\\WebView2";
    CoTaskMemFree(p); return d;
}

// ui.html calls window.webkit.messageHandlers.p8.postMessage(obj) (the WKWebView API): forward it to WebView2
static const wchar_t* BRIDGE_JS =
    L"window.webkit = { messageHandlers: { p8: { postMessage: function (m) { window.chrome.webview.postMessage(m); } } } };";

static void start_webview(View* v) {
    auto alive = v->alive;
    std::wstring udd = user_data_dir();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udd.empty() ? nullptr : udd.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([v, alive](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
            if (!*alive || FAILED(r) || !env) return S_OK;
            return env->CreateCoreWebView2Controller(v->hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([v, alive](HRESULT r2, ICoreWebView2Controller* ctl) -> HRESULT {
                    if (!*alive || FAILED(r2) || !ctl) return S_OK;
                    v->ctl = ctl; v->ctl->get_CoreWebView2(&v->wv);
                    ComPtr<ICoreWebView2Settings> s;
                    if (SUCCEEDED(v->wv->get_Settings(&s))) {
                        s->put_AreDefaultContextMenusEnabled(FALSE); s->put_IsStatusBarEnabled(FALSE); s->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
                        s->put_AreDevToolsEnabled(FALSE);
#endif
                    }
                    EventRegistrationToken tok;
                    v->wv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [v, alive](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            if (!*alive) return S_OK;
                            LPWSTR j = nullptr;
                            if (SUCCEEDED(a->get_WebMessageAsJson(&j)) && j) {
                                v->inbox.push_back(utf8(j)); CoTaskMemFree(j);
                                if (v->inbox.size() == 1) PostMessageW(v->hwnd, WM_P8_MSG, 0, 0);   // no modal dialog inside the WebView2 event
                            }
                            return S_OK;
                        }).Get(), &tok);
                    RECT rc; GetClientRect(v->hwnd, &rc); v->ctl->put_Bounds(rc); v->ctl->put_IsVisible(v->visible ? TRUE : FALSE);
                    // navigate once the bridge script is registered (the add call completes asynchronously)
                    v->wv->AddScriptToExecuteOnDocumentCreated(BRIDGE_JS,
                        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>([v, alive](HRESULT, LPCWSTR) -> HRESULT {
                            if (*alive && v->wv) v->wv->NavigateToString(wide(pro8_embedded_ui_html()).c_str());
                            return S_OK;
                        }).Get());
                    return S_OK;
                }).Get());
        }).Get());
    if (FAILED(hr))
        MessageBoxW(v->hwnd, L"PRO8 needs the Microsoft Edge WebView2 Runtime for its panel.\nhttps://developer.microsoft.com/microsoft-edge/webview2/",
                    L"PRO8", MB_OK | MB_ICONWARNING);
}

View* create(pro8::Core* core, const std::string&, uint32_t w, uint32_t h) {
    View* v = new View(); v->core = core; v->w = w; v->h = h;
    v->jq = std::make_shared<JsQueue>(); v->alive = std::make_shared<std::atomic<bool>>(true);
    auto jq = v->jq;
    core->js = [jq](const std::string& s) {
        std::lock_guard<std::mutex> l(jq->m);
        if (!jq->hwnd) return;
        bool first = jq->q.empty(); jq->q.push_back(s);
        if (first) PostMessageW(jq->hwnd, WM_P8_JS, 0, 0);
    };
    return v;
}
bool set_parent(View* v, const clap_window_t* win) {
    if (!v || !win || !win->win32 || !register_class()) return false;
    if (v->hwnd) { SetParent(v->hwnd, (HWND)win->win32); return true; }
    v->hwnd = CreateWindowExW(0, CLASS_NAME, L"PRO8", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                              0, 0, (int)v->w, (int)v->h, (HWND)win->win32, nullptr, module_instance(), nullptr);
    if (!v->hwnd) return false;
    SetWindowLongPtrW(v->hwnd, GWLP_USERDATA, (LONG_PTR)v);
    { std::lock_guard<std::mutex> l(v->jq->m); v->jq->hwnd = v->hwnd; }
    start_webview(v);
    return true;
}
void set_size(View* v, uint32_t w, uint32_t h) {
    if (!v) return; v->w = w; v->h = h;
    if (v->hwnd) SetWindowPos(v->hwnd, nullptr, 0, 0, (int)w, (int)h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}
void set_visible(View* v, bool on) {
    if (!v) return; v->visible = on;
    if (v->hwnd) ShowWindow(v->hwnd, on ? SW_SHOW : SW_HIDE);
    if (v->ctl) v->ctl->put_IsVisible(on ? TRUE : FALSE);
}
void destroy(View* v) {
    if (!v) return;
    *v->alive = false;
    v->core->js = nullptr;
    { std::lock_guard<std::mutex> l(v->jq->m); v->jq->hwnd = nullptr; v->jq->q.clear(); }
    if (v->ctl) v->ctl->Close();
    v->wv.Reset(); v->ctl.Reset();
    if (v->hwnd) { SetWindowLongPtrW(v->hwnd, GWLP_USERDATA, 0); DestroyWindow(v->hwnd); }
    delete v;   // pending WebView2 callbacks hold their own copy of `alive` and check it before touching the View
}

}  // namespace pro8gui
