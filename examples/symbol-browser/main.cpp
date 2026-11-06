// Symbol browser: a Windows program that connects to a TwinCAT PLC (through the local TwinCAT router or over an ADS
// route, also with Secure ADS), adds the route on the target if asked, and shows all variables of the PLC down to
// single values with their current values. Values can be watched with notifications (right click). It only reads.
//
// Uses the Win32 API and the common controls, nothing else; built only on Windows.

#include "AdsDevice.h"
#include "AdsException.h"
#include "AdsLib.h"
#include "AdsNotificationOOI.h"
#include "AdsSymbols.h"
#include "SymbolText.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Text helpers

std::wstring Widen(const std::string& s, UINT codePage = CP_UTF8) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

template <typename... Args> std::wstring Format(const wchar_t* format, Args... args) {
    wchar_t buffer[512];
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), format, args...);
    return buffer;
}

using symbol_text::IsLibrarySymbol;
using symbol_text::Upper;

std::wstring NetIdText(const AmsNetId& id) {
    return Widen(symbol_text::NetIdText(id));
}

bool ParseNetId(const std::wstring& text, AmsNetId& id) {
    return symbol_text::ParseNetId(Narrow(text), id);
}

std::wstring StateName(ADSSTATE s) {
    return Widen(symbol_text::StateName(s));
}

std::wstring ValueText(const AdsElement& e, const std::vector<uint8_t>& d) {
    return Widen(symbol_text::ValueText(e, d));
}

// ---------------------------------------------------------------------------------------------------------------
// The window

enum : int {
    ID_MODE = 100, ID_IP, ID_FIND, ID_TARGET, ID_PORT, ID_OWN, ID_USER, ID_PASSWORD, ID_PSK,
    ID_ADDROUTE, ID_CONNECT, ID_DISCONNECT, ID_STATE, ID_FILTER, ID_LIBRARIES, ID_AUTO, ID_REFRESH,
    ID_LIST, ID_LOG, ID_CLEAR, ID_STATUS, ID_TIMER = 1,
};
enum : UINT { WM_APP_JOB = WM_APP + 1, WM_APP_NOTE };
enum Mode { MODE_LOCAL, MODE_ROUTE, MODE_SSC, MODE_PSK };

struct Node {
    AdsElement element;
    int depth;
    bool expanded = false;
    bool loaded = false;
    std::vector<int> children;
    std::wstring value;
};

struct Watch {
    std::unique_ptr<AdsNotification> notification;
    AdsElement element;
};

struct NoteMessage {
    uint32_t id;
    int64_t timeStamp;
    std::vector<uint8_t> data;
};

struct Settings {
    Mode mode;
    std::wstring ip, target, port, own, user, password, psk;
};

struct App {
    HWND window = nullptr;
    HFONT font = nullptr;
    std::map<int, HWND> controls;
    std::vector<std::pair<HWND, int>> labels; // label and the control it belongs to

    std::unique_ptr<AdsDevice> device;
    std::unique_ptr<AdsSymbolTable> table;
    Mode connectedMode = MODE_LOCAL;
    AmsNetId connectedTarget{};

    std::vector<Node> nodes;
    std::vector<int> roots;
    std::vector<int> visible;
    unsigned generation = 0; // changes with every new tree; read results of an old tree are dropped

    std::map<std::string, uint32_t> watchedPaths;
    std::map<uint32_t, Watch> watches;
    uint32_t nextWatch = 1;

    std::thread worker;
    bool busy = false;
};

App app;

HWND Control(int id) {
    return app.controls[id];
}

std::wstring Text(int id) {
    const HWND h = Control(id);
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    if (!s.empty()) GetWindowTextW(h, &s[0], static_cast<int>(s.size()) + 1);
    return s;
}

void SetText(int id, const std::wstring& text) {
    SetWindowTextW(Control(id), text.c_str());
}

void Status(const std::wstring& text) {
    SendMessageW(Control(ID_STATUS), SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void Log(const std::wstring& text) {
    const HWND log = Control(ID_LOG);
    if (SendMessageW(log, LB_GETCOUNT, 0, 0) > 1000) SendMessageW(log, LB_DELETESTRING, 0, 0);
    const auto i = SendMessageW(log, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(log, LB_SETTOPINDEX, static_cast<WPARAM>(i), 0);
}

Mode CurrentMode() {
    return static_cast<Mode>(SendMessageW(Control(ID_MODE), CB_GETCURSEL, 0, 0));
}

bool Connected() {
    return app.table != nullptr;
}

void UpdateControls() {
    const Mode m = CurrentMode();
    const bool route = m != MODE_LOCAL;
    const bool connected = Connected();
    const bool idle = !app.busy;
    for (int id : {ID_MODE, ID_IP, ID_TARGET, ID_PORT, ID_OWN, ID_USER, ID_PASSWORD, ID_PSK}) {
        bool on = !connected;
        if (id == ID_IP || id == ID_OWN) on = on && route;
        if (id == ID_USER) on = on && (m == MODE_ROUTE || m == MODE_SSC);
        if (id == ID_PASSWORD) on = on && route;
        if (id == ID_PSK) on = on && m == MODE_PSK;
        EnableWindow(Control(id), on);
    }
    EnableWindow(Control(ID_FIND), idle && !connected && route);
    EnableWindow(Control(ID_ADDROUTE), idle && !connected && (m == MODE_ROUTE || m == MODE_SSC));
    EnableWindow(Control(ID_CONNECT), idle && !connected);
    EnableWindow(Control(ID_DISCONNECT), connected);
    EnableWindow(Control(ID_REFRESH), idle && connected);
}

// ---------------------------------------------------------------------------------------------------------------
// Work in the background: ADS calls can wait for their timeout, the window must not. The work returns what is to be
// done afterwards in the window's thread.

using Done = std::function<void()>;

bool Run(std::function<Done()> work) {
    if (app.busy) return false;
    app.busy = true;
    UpdateControls();
    const HWND window = app.window;
    app.worker = std::thread([work, window] {
        Done done;
        try {
            done = work();
        } catch (const AdsException& e) {
            const long code = e.errorCode;
            done = [code] { Status(Format(L"ADS error 0x%lX", code)); };
        } catch (const std::exception& e) {
            const std::wstring what = Widen(e.what());
            done = [what] { Status(L"Error: " + what); };
        }
        auto message = new Done(std::move(done));
        if (!PostMessageW(window, WM_APP_JOB, 0, reinterpret_cast<LPARAM>(message))) delete message;
    });
    return true;
}

void WaitForWork() {
    if (app.worker.joinable()) app.worker.join();
    // The message with the result is dropped: its work belongs to what is closed now.
    MSG msg;
    while (PeekMessageW(&msg, app.window, WM_APP_JOB, WM_APP_JOB, PM_REMOVE)) {
        delete reinterpret_cast<Done*>(msg.lParam);
    }
    app.busy = false;
}

// ---------------------------------------------------------------------------------------------------------------
// The tree as rows of the list

int Scale(int value);

// The last column takes the rest of the width, without the vertical scroll bar when there is one.
void FitLastColumn() {
    const HWND list = Control(ID_LIST);
    RECT r;
    GetClientRect(list, &r);
    int width = r.right;
    for (int c = 0; c < 3; ++c) width -= ListView_GetColumnWidth(list, c);
    ListView_SetColumnWidth(list, 3, std::max(width, Scale(100)));
}

void RebuildVisible() {
    app.visible.clear();
    std::function<void(int)> add = [&](int i) {
        app.visible.push_back(i);
        if (app.nodes[static_cast<size_t>(i)].expanded) {
            for (int c : app.nodes[static_cast<size_t>(i)].children) add(c);
        }
    };
    for (int r : app.roots) add(r);
    ListView_SetItemCountEx(Control(ID_LIST), static_cast<int>(app.visible.size()), LVSICF_NOSCROLL);
    FitLastColumn();
    InvalidateRect(Control(ID_LIST), nullptr, FALSE);
}

void BuildRoots() {
    ++app.generation;
    app.nodes.clear();
    app.roots.clear();
    if (Connected()) {
        const bool libraries = Button_GetCheck(Control(ID_LIBRARIES)) == BST_CHECKED;
        const std::string filter = Upper(Narrow(Text(ID_FILTER)));
        for (const auto& s : app.table->Symbols()) {
            if (!libraries && IsLibrarySymbol(s.name)) continue;
            if (!filter.empty() && Upper(s.name).find(filter) == std::string::npos) continue;
            Node n;
            n.element = app.table->Root(s);
            n.depth = 0;
            app.roots.push_back(static_cast<int>(app.nodes.size()));
            app.nodes.push_back(std::move(n));
        }
    }
    RebuildVisible();
}

void Toggle(int row, int expand) { // expand: 1 open, 0 close, -1 switch
    if (row < 0 || row >= static_cast<int>(app.visible.size())) return;
    const int i = app.visible[static_cast<size_t>(row)];
    Node& n = app.nodes[static_cast<size_t>(i)];
    if (!n.element.hasChildren) return;
    const bool open = expand < 0 ? !n.expanded : expand == 1;
    if (open == n.expanded) return;
    if (open && !n.loaded) {
        const auto children = app.table->Children(n.element);
        const int depth = n.depth + 1;
        n.loaded = true;
        std::vector<int> ids;
        for (const auto& c : children) { // may move the nodes, so no reference to n below
            Node child;
            child.element = c;
            child.depth = depth;
            ids.push_back(static_cast<int>(app.nodes.size()));
            app.nodes.push_back(std::move(child));
        }
        app.nodes[static_cast<size_t>(i)].children = std::move(ids);
    }
    app.nodes[static_cast<size_t>(i)].expanded = open;
    RebuildVisible();
}

void ReadVisible();

// ---------------------------------------------------------------------------------------------------------------
// Connecting

Settings ReadSettings() {
    return {CurrentMode(), Text(ID_IP), Text(ID_TARGET), Text(ID_PORT), Text(ID_OWN), Text(ID_USER), Text(ID_PASSWORD),
            Text(ID_PSK)};
}

// The address of this computer that the target sees, found by asking the system for a route to it.
std::string OwnAddressFor(const std::string& target) {
    std::string own;
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return own;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(48899);
    if (inet_pton(AF_INET, target.c_str(), &to.sin_addr) == 1 &&
        connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in me{};
        int length = sizeof(me);
        char text[INET_ADDRSTRLEN] = {};
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &length) == 0 &&
            inet_ntop(AF_INET, &me.sin_addr, text, sizeof(text))) {
            own = text;
        }
    }
    closesocket(s);
    return own;
}

void Find() {
    const std::string ip = Narrow(Text(ID_IP));
    const bool ownMissing = Text(ID_OWN).empty();
    Status(L"Asking " + Widen(ip) + L" for its AmsNetId (UDP 48899) ...");
    Run([ip, ownMissing]() -> Done {
        AmsNetId id{};
        const long error = bhf::ads::GetRemoteAddress(ip, id);
        const std::string own = ownMissing ? OwnAddressFor(ip) : std::string();
        return [error, id, own, ip] {
            if (error) {
                Status(Format(L"No answer from %ls: 0x%lX", Widen(ip).c_str(), error));
                return;
            }
            SetText(ID_TARGET, NetIdText(id));
            if (!own.empty()) SetText(ID_OWN, Widen(own) + L".1.1");
            Status(Widen(ip) + L" has the AmsNetId " + NetIdText(id));
        };
    });
}

void AddRouteOnTarget() {
    const Settings s = ReadSettings();
    AmsNetId own{};
    if (!ParseNetId(s.own, own)) {
        Status(L"Own AmsNetId is not valid");
        return;
    }
    Status(L"Adding the route on " + s.ip + L" ...");
    Run([s, own]() -> Done {
        const std::string ip = Narrow(s.ip);
        long error;
        if (s.mode == MODE_SSC) {
            // Inside TLS on TCP 8016: the password does not go over the network in plain text.
            error = AdsAddRemoteRouteSecure(ip.c_str(), own, Narrow(s.user).c_str(), Narrow(s.password).c_str(),
                                            nullptr, nullptr, nullptr);
        } else {
            char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD length = sizeof(name);
            GetComputerNameA(name, &length);
            error = bhf::ads::AddRemoteRoute(ip, own, OwnAddressFor(ip), name, Narrow(s.user), Narrow(s.password));
        }
        return [error, s] {
            Status(error ? Format(L"Adding the route failed: 0x%lX", error)
                         : L"Route added on " + s.ip + (s.mode == MODE_SSC ? L" (Secure ADS)" : L""));
        };
    });
}

void Connect() {
    const Settings s = ReadSettings();
    AmsNetId target{};
    AmsNetId own{};
    const auto port = static_cast<uint16_t>(std::wcstoul(s.port.c_str(), nullptr, 10));
    if (!ParseNetId(s.target, target) || !port) {
        Status(L"Target AmsNetId or port is not valid");
        return;
    }
    if (s.mode != MODE_LOCAL && !ParseNetId(s.own, own)) {
        Status(L"Own AmsNetId is not valid");
        return;
    }
    Status(L"Connecting and uploading the symbols ...");
    struct Result {
        std::unique_ptr<AdsDevice> device;
        std::unique_ptr<AdsSymbolTable> table;
        AdsDeviceState state;
        long long ms;
    };
    Run([s, target, own, port]() -> Done {
        auto result = std::make_shared<Result>();
        const std::string ip = Narrow(s.ip);
        long error = 0;
        if (s.mode != MODE_LOCAL) {
            AdsSetLocalAddress(own);
            if (s.mode == MODE_ROUTE) error = AdsAddRoute(target, ip.c_str());
            if (s.mode == MODE_SSC) error = AdsAddRouteSecure(target, ip.c_str(), nullptr);
            if (s.mode == MODE_PSK) {
                error = AdsAddRoutePsk(target, ip.c_str(), Narrow(s.psk).c_str(), Narrow(s.password).c_str());
            }
            if (error) throw AdsException(error);
        }
        try {
            const auto start = std::chrono::steady_clock::now();
            result->device = s.mode == MODE_LOCAL ? std::make_unique<AdsDevice>(target, port)
                                                  : std::make_unique<AdsDevice>(target, port, AdsRouteAdded{});
            result->state = result->device->GetState();
            result->table = std::make_unique<AdsSymbolTable>(*result->device);
            result->ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                             .count();
        } catch (...) {
            result->table.reset();
            result->device.reset();
            if (s.mode != MODE_LOCAL) AdsDelRoute(target);
            throw;
        }
        return [result, s, target] {
            app.device = std::move(result->device);
            app.table = std::move(result->table);
            app.connectedMode = s.mode;
            app.connectedTarget = target;
            SetText(ID_STATE, Format(L"%ls (%d)", StateName(result->state.ads).c_str(), static_cast<int>(result->state.ads)));
            BuildRoots();
            Status(Format(L"%zu symbols, %zu data types, uploaded in %lld ms", app.table->Symbols().size(),
                          app.table->DataTypes().size(), result->ms));
            // After the job is finished (busy is cleared by the caller), read the first values.
            PostMessageW(app.window, WM_COMMAND, MAKEWPARAM(ID_REFRESH, BN_CLICKED), 0);
        };
    });
}

void Disconnect() {
    KillTimer(app.window, ID_TIMER);
    WaitForWork();
    app.watches.clear(); // notifications go before the device they belong to
    app.watchedPaths.clear();
    app.table.reset();
    app.device.reset();
    if (app.connectedMode != MODE_LOCAL) AdsDelRoute(app.connectedTarget);
    BuildRoots();
    SetText(ID_STATE, L"");
    Status(L"Disconnected");
    UpdateControls();
}

// ---------------------------------------------------------------------------------------------------------------
// Reading the values of the rows on screen

void ReadVisible() {
    if (!Connected() || app.busy) return;
    const HWND list = Control(ID_LIST);
    const int top = ListView_GetTopIndex(list);
    const int count = ListView_GetCountPerPage(list) + 1;
    std::vector<int> ids;
    std::vector<AdsElement> elements;
    for (int row = top; row < top + count && row < static_cast<int>(app.visible.size()); ++row) {
        const int i = app.visible[static_cast<size_t>(row)];
        const auto& e = app.nodes[static_cast<size_t>(i)].element;
        if (e.hasChildren) continue; // structs and arrays have no value of their own
        ids.push_back(i);
        elements.push_back(e);
    }
    if (elements.empty()) return;
    const unsigned generation = app.generation;
    const AdsDevice* device = app.device.get();
    const AdsSymbolTable* table = app.table.get();
    Run([ids, elements, generation, device, table]() -> Done {
        const auto start = std::chrono::steady_clock::now();
        std::vector<AdsReadResult> read;
        try {
            // Also checks that the PLC still has the program the table was uploaded from.
            read = ReadElements(*device, *table, elements);
        } catch (const AdsException& e) {
            if (e.errorCode != ADSERR_DEVICE_SYMBOLVERSIONINVALID) throw;
            return [] {
                KillTimer(app.window, ID_TIMER);
                Button_SetCheck(Control(ID_AUTO), BST_UNCHECKED);
                Status(L"The PLC has a new program: disconnect and connect again to see its variables");
            };
        }
        const auto results = std::make_shared<std::vector<AdsReadResult>>(std::move(read));
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        return [ids, results, generation, ms] {
            if (generation != app.generation) return;
            for (size_t k = 0; k < ids.size(); ++k) {
                Node& n = app.nodes[static_cast<size_t>(ids[k])];
                const auto& r = (*results)[k];
                n.value = r.error ? Format(L"<error 0x%lX>", r.error) : ValueText(n.element, r.data);
            }
            InvalidateRect(Control(ID_LIST), nullptr, FALSE);
            Status(Format(L"%zu symbols, %zu rows, %zu values read in %lld ms", app.roots.size(), app.visible.size(),
                          ids.size(), static_cast<long long>(ms)));
        };
    });
}

// ---------------------------------------------------------------------------------------------------------------
// Notifications

void OnNotification(const AmsAddr*, const AdsNotificationHeader* header, uint32_t id) {
    // Called in a thread of the library: hand a copy to the window.
    auto message = new NoteMessage{id, header->nTimeStamp,
                                   std::vector<uint8_t>(header->data, header->data + header->cbSampleSize)};
    if (!PostMessageW(app.window, WM_APP_NOTE, 0, reinterpret_cast<LPARAM>(message))) delete message;
}

void ToggleWatch(const AdsElement& e) {
    const auto found = app.watchedPaths.find(e.path);
    if (found != app.watchedPaths.end()) {
        app.watches.erase(found->second);
        app.watchedPaths.erase(found);
        Log(L"Notification removed: " + Widen(e.path));
        InvalidateRect(Control(ID_LIST), nullptr, FALSE);
        return;
    }
    AdsNotificationAttrib attrib{};
    attrib.cbLength = e.size;
    attrib.nTransMode = ADSTRANS_SERVERONCHA;
    attrib.nMaxDelay = 0;
    attrib.nCycleTime = 1000000; // 100 ms in 100 ns
    const uint32_t id = app.nextWatch++;
    try {
        Watch w;
        w.element = e;
        w.notification = e.kind == AdsElement::Kind::Property
                             ? std::make_unique<AdsNotification>(*app.device, e.path, attrib, &OnNotification, id)
                             : std::make_unique<AdsNotification>(*app.device, e.indexGroup, e.indexOffset, attrib,
                                                                 &OnNotification, id);
        app.watches[id] = std::move(w);
        app.watchedPaths[e.path] = id;
        Log(L"Notification added: " + Widen(e.path));
        InvalidateRect(Control(ID_LIST), nullptr, FALSE);
    } catch (const AdsException& ex) {
        Status(Format(L"Notification for %ls failed: 0x%lX", Widen(e.path).c_str(), ex.errorCode));
    }
}

void ShowNotification(const NoteMessage& m) {
    const auto w = app.watches.find(m.id);
    if (w == app.watches.end()) return; // removed in the meantime
    FILETIME utc;
    utc.dwLowDateTime = static_cast<DWORD>(m.timeStamp);
    utc.dwHighDateTime = static_cast<DWORD>(static_cast<uint64_t>(m.timeStamp) >> 32);
    FILETIME local;
    SYSTEMTIME t{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &t);
    Log(Format(L"%02u:%02u:%02u.%03u  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds) +
        Widen(w->second.element.path) + L"  " + ValueText(w->second.element, m.data));
}

// ---------------------------------------------------------------------------------------------------------------
// Layout

int Scale(int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(app.window)), 96);
}

HWND Create(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    const HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, app.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                                   nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    if (id) app.controls[id] = h;
    return h;
}

void Label(const wchar_t* text, int id) {
    app.labels.emplace_back(Create(L"STATIC", text, SS_RIGHT | SS_CENTERIMAGE, 0), id);
}

HWND LabelOf(int id) {
    for (const auto& l : app.labels) {
        if (l.second == id) return l.first;
    }
    return nullptr;
}

void CreateControls() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, GetDpiForWindow(app.window));
    app.font = CreateFontIndirectW(&metrics.lfMessageFont);

    Label(L"Connection", ID_MODE);
    const HWND mode = Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, ID_MODE);
    for (const wchar_t* m : {L"Local TwinCAT router", L"Route", L"Route, Secure ADS (self-signed)",
                             L"Route, Secure ADS (pre-shared key)"}) {
        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m));
    }
    SendMessageW(mode, CB_SETCURSEL, MODE_LOCAL, 0);
    Label(L"Target IP", ID_IP);
    Create(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_IP, WS_EX_CLIENTEDGE);
    Create(WC_BUTTONW, L"Find", BS_PUSHBUTTON | WS_TABSTOP, ID_FIND);
    Label(L"Target AmsNetId", ID_TARGET);
    Create(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_TARGET, WS_EX_CLIENTEDGE);
    Label(L"Port", ID_PORT);
    Create(WC_EDITW, L"851", ES_NUMBER | WS_TABSTOP, ID_PORT, WS_EX_CLIENTEDGE);

    Label(L"Own AmsNetId", ID_OWN);
    Create(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_OWN, WS_EX_CLIENTEDGE);
    Label(L"User", ID_USER);
    Create(WC_EDITW, L"Administrator", ES_AUTOHSCROLL | WS_TABSTOP, ID_USER, WS_EX_CLIENTEDGE);
    Label(L"Password", ID_PASSWORD);
    Create(WC_EDITW, L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, ID_PASSWORD, WS_EX_CLIENTEDGE);
    Label(L"PSK identity", ID_PSK);
    Create(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_PSK, WS_EX_CLIENTEDGE);

    Create(WC_BUTTONW, L"Add route", BS_PUSHBUTTON | WS_TABSTOP, ID_ADDROUTE);
    Create(WC_BUTTONW, L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_CONNECT);
    Create(WC_BUTTONW, L"Disconnect", BS_PUSHBUTTON | WS_TABSTOP, ID_DISCONNECT);
    Label(L"State", ID_STATE);
    Create(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, ID_STATE);

    Label(L"Filter", ID_FILTER);
    Create(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_FILTER, WS_EX_CLIENTEDGE);
    Create(WC_BUTTONW, L"Beckhoff libraries", BS_AUTOCHECKBOX | WS_TABSTOP, ID_LIBRARIES);
    Create(WC_BUTTONW, L"Auto refresh 1 s", BS_AUTOCHECKBOX | WS_TABSTOP, ID_AUTO);
    Create(WC_BUTTONW, L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, ID_REFRESH);

    const HWND list = Create(WC_LISTVIEWW, L"",
                             LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST,
                             WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    const struct {
        const wchar_t* name;
        int width;
    } columns[] = {{L"Name", 380}, {L"Type", 220}, {L"Size", 60}, {L"Value", 300}};
    for (int c = 0; c < 4; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = c == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.pszText = const_cast<wchar_t*>(columns[c].name);
        col.cx = Scale(columns[c].width);
        ListView_InsertColumn(list, c, &col);
    }
    Create(WC_LISTBOXW, L"", LBS_NOINTEGRALHEIGHT | LBS_NOSEL | WS_VSCROLL, ID_LOG, WS_EX_CLIENTEDGE);
    Create(WC_BUTTONW, L"Clear", BS_PUSHBUTTON | WS_TABSTOP, ID_CLEAR);
    Create(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, ID_STATUS);
}

void Layout() {
    RECT client;
    GetClientRect(app.window, &client);
    const int margin = Scale(8), gap = Scale(6), height = Scale(24), labelGap = Scale(4);
    SendMessageW(Control(ID_STATUS), WM_SIZE, 0, 0);
    RECT status;
    GetWindowRect(Control(ID_STATUS), &status);
    const int statusHeight = status.bottom - status.top;

    int x = margin, y = margin;
    auto place = [&](int id, int width, int labelWidth) {
        if (const HWND label = LabelOf(id)) {
            MoveWindow(label, x, y, Scale(labelWidth), height, TRUE);
            x += Scale(labelWidth) + labelGap;
        }
        MoveWindow(Control(id), x, y, Scale(width), id == ID_MODE ? Scale(200) : height, TRUE);
        x += Scale(width) + gap;
    };
    auto nextRow = [&] {
        x = margin;
        y += height + gap;
    };
    place(ID_MODE, 230, 70);
    place(ID_IP, 120, 60);
    place(ID_FIND, 50, 0);
    place(ID_TARGET, 130, 95);
    place(ID_PORT, 50, 30);
    nextRow();
    place(ID_OWN, 130, 85);
    place(ID_USER, 110, 35);
    place(ID_PASSWORD, 110, 60);
    place(ID_PSK, 110, 70);
    nextRow();
    place(ID_ADDROUTE, 80, 0);
    place(ID_CONNECT, 80, 0);
    place(ID_DISCONNECT, 80, 0);
    place(ID_STATE, 120, 40);
    nextRow();
    place(ID_FILTER, 200, 35);
    place(ID_LIBRARIES, 130, 0);
    place(ID_AUTO, 120, 0);
    place(ID_REFRESH, 80, 0);
    nextRow();

    const int width = client.right - 2 * margin;
    const int logHeight = Scale(130);
    const int listHeight = client.bottom - statusHeight - margin - logHeight - gap - y;
    MoveWindow(Control(ID_LIST), margin, y, width, std::max(listHeight, Scale(80)), TRUE);
    FitLastColumn();
    const int logY = y + std::max(listHeight, Scale(80)) + gap;
    MoveWindow(Control(ID_LOG), margin, logY, width - Scale(80) - gap, logHeight, TRUE);
    MoveWindow(Control(ID_CLEAR), margin + width - Scale(80), logY, Scale(80), height, TRUE);
}

// ---------------------------------------------------------------------------------------------------------------
// Messages

void OnGetDispInfo(NMLVDISPINFOW* info) {
    LVITEMW& item = info->item;
    if (!(item.mask & LVIF_TEXT) || item.iItem < 0 || item.iItem >= static_cast<int>(app.visible.size())) return;
    const Node& n = app.nodes[static_cast<size_t>(app.visible[static_cast<size_t>(item.iItem)])];
    std::wstring text;
    switch (item.iSubItem) {
    case 0: {
        text.assign(static_cast<size_t>(n.depth) * 4, L' ');
        text += n.element.hasChildren ? (n.expanded ? L"\x25BE " : L"\x25B8 ") : L"   ";
        // Only the last part of the path below the symbol: "stAxis", "[3]"
        text += Widen(symbol_text::RowName(n.element.path, n.depth));
        if (app.watchedPaths.count(n.element.path)) text += L"  \x25CF";
        break;
    }
    case 1: text = Widen(n.element.typeName); break;
    case 2: text = std::to_wstring(n.element.size); break;
    case 3: text = n.element.hasChildren ? L"" : n.value; break;
    default: break;
    }
    lstrcpynW(item.pszText, text.c_str(), item.cchTextMax);
}

void OnContextMenu(int x, int y) {
    const HWND list = Control(ID_LIST);
    int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (x != -1 || y != -1) {
        LVHITTESTINFO hit{};
        hit.pt = {x, y};
        ScreenToClient(list, &hit.pt);
        row = ListView_HitTest(list, &hit);
        if (row >= 0) ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    } else if (row >= 0) {
        RECT r;
        ListView_GetItemRect(list, row, &r, LVIR_LABEL);
        POINT p{r.left, r.bottom};
        ClientToScreen(list, &p);
        x = p.x;
        y = p.y;
    }
    if (row < 0 || row >= static_cast<int>(app.visible.size()) || !Connected()) return;
    const AdsElement element = app.nodes[static_cast<size_t>(app.visible[static_cast<size_t>(row)])].element;
    if (element.hasChildren) return;
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, app.watchedPaths.count(element.path) ? L"Remove notification" : L"Add notification");
    const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, app.window, nullptr);
    DestroyMenu(menu);
    if (chosen == 1) ToggleWatch(element);
}

LRESULT OnNotify(NMHDR* header) {
    if (header->idFrom != ID_LIST) return 0;
    switch (header->code) {
    case LVN_GETDISPINFOW: OnGetDispInfo(reinterpret_cast<NMLVDISPINFOW*>(header)); break;
    case NM_DBLCLK: {
        const auto* item = reinterpret_cast<NMITEMACTIVATE*>(header);
        Toggle(item->iItem, -1);
        ReadVisible();
        break;
    }
    case NM_CLICK: {
        // A click on the arrow opens or closes the node.
        const auto* item = reinterpret_cast<NMITEMACTIVATE*>(header);
        if (item->iItem >= 0 && item->iItem < static_cast<int>(app.visible.size())) {
            const Node& n = app.nodes[static_cast<size_t>(app.visible[static_cast<size_t>(item->iItem)])];
            SIZE indent;
            const std::wstring prefix(static_cast<size_t>(n.depth) * 4 + 2, L' ');
            const HDC dc = GetDC(header->hwndFrom);
            const HGDIOBJ old = SelectObject(dc, app.font);
            GetTextExtentPoint32W(dc, prefix.c_str(), static_cast<int>(prefix.size()), &indent);
            SelectObject(dc, old);
            ReleaseDC(header->hwndFrom, dc);
            if (item->ptAction.x < indent.cx + Scale(12)) {
                Toggle(item->iItem, -1);
                ReadVisible();
            }
        }
        break;
    }
    case LVN_KEYDOWN: {
        const auto* key = reinterpret_cast<NMLVKEYDOWN*>(header);
        const int row = ListView_GetNextItem(header->hwndFrom, -1, LVNI_FOCUSED);
        if (key->wVKey == VK_RIGHT || key->wVKey == VK_LEFT) {
            Toggle(row, key->wVKey == VK_RIGHT ? 1 : 0);
            ReadVisible();
        }
        break;
    }
    case LVN_ENDSCROLL: ReadVisible(); break;
    default: break;
    }
    return 0;
}

void OnCommand(int id, int code) {
    switch (id) {
    case ID_MODE:
        if (code == CBN_SELCHANGE) UpdateControls();
        break;
    case ID_FIND: Find(); break;
    case ID_ADDROUTE: AddRouteOnTarget(); break;
    case ID_CONNECT: Connect(); break;
    case ID_DISCONNECT: Disconnect(); break;
    case ID_REFRESH: ReadVisible(); break;
    case ID_CLEAR: SendMessageW(Control(ID_LOG), LB_RESETCONTENT, 0, 0); break;
    case ID_LIBRARIES:
        if (Connected()) {
            BuildRoots();
            ReadVisible();
        }
        break;
    case ID_FILTER:
        if (code == EN_CHANGE && Connected()) {
            BuildRoots();
            ReadVisible();
        }
        break;
    case ID_AUTO:
        if (Button_GetCheck(Control(ID_AUTO)) == BST_CHECKED) {
            SetTimer(app.window, ID_TIMER, 1000, nullptr);
        } else {
            KillTimer(app.window, ID_TIMER);
        }
        break;
    default: break;
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        app.window = window;
        CreateControls();
        UpdateControls();
        Status(L"Not connected");
        return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {Scale(900), Scale(500)};
        return 0;
    }
    case WM_COMMAND: OnCommand(LOWORD(wParam), HIWORD(wParam)); return 0;
    case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(lParam));
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == Control(ID_LIST)) OnContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_TIMER:
        if (wParam == ID_TIMER) ReadVisible();
        return 0;
    case WM_APP_JOB: {
        std::unique_ptr<Done> done(reinterpret_cast<Done*>(lParam));
        if (app.worker.joinable()) app.worker.join();
        app.busy = false;
        if (*done) (*done)();
        UpdateControls();
        return 0;
    }
    case WM_APP_NOTE: {
        std::unique_ptr<NoteMessage> note(reinterpret_cast<NoteMessage*>(lParam));
        ShowNotification(*note);
        return 0;
    }
    case WM_DESTROY:
        if (Connected()) Disconnect();
        WaitForWork();
        DeleteObject(app.font);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.lpszClassName = L"LibAdsClientSymbolBrowser";
    RegisterClassExW(&cls);
    const HWND window = CreateWindowExW(0, cls.lpszClassName, L"libads-client Symbol Browser", WS_OVERLAPPEDWINDOW,
                                        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr, instance, nullptr);
    // The size above is in pixels at 96 dpi.
    const UINT dpi = GetDpiForWindow(window);
    SetWindowPos(window, nullptr, 0, 0, MulDiv(1200, static_cast<int>(dpi), 96), MulDiv(800, static_cast<int>(dpi), 96),
                 SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(window, show);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    WSACleanup();
    return static_cast<int>(msg.wParam);
}
