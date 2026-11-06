// Symbol browser in the terminal: connects to a TwinCAT PLC (through the local TwinCAT router or over an ADS route,
// also with Secure ADS), adds the route on the target if asked, and shows all variables of the PLC down to single
// values with their current values. Values can be watched with notifications. It only reads.
//
// Uses ncurses with wide characters (UTF-8); built everywhere but Windows, tested on Linux. Runs over ssh as well,
// e.g. on a controller with TwinCAT for Linux or TwinCAT/BSD that has no desktop.
//
// Usage: symbol-browser-tui [options] [<target netid>[:<port>]]
//   without options       the local TwinCAT router; without target the PLC of this system (port 851)
//   --route <ip>          over an ADS route to <ip>, plain ADS (the target needs a route to this computer)
//   --ssc <ip>            over Secure ADS with self-signed certificates
//   --psk <ip>[:<port>]   over Secure ADS with a pre-shared key: asks for identity and password
//   --add-route <user>    with --route or --ssc: first adds the route on the target, asks for the user's password
//   --own-netid <netid>   own AmsNetId on the route way (default: the own IP address that faces the target + .1.1)
// Without target AmsNetId the route way asks the target for it (UDP 48899).

#ifndef NCURSES_WIDECHAR
#define NCURSES_WIDECHAR 1
#endif

#include "AdsDevice.h"
#include "AdsException.h"
#include "AdsLib.h"
#include "AdsNotificationOOI.h"
#include "AdsSymbols.h"
#include "SymbolText.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <curses.h>

#include <atomic>
#include <chrono>
#include <clocale>
#include <csignal>
#include <ctime>
#include <cwchar>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using symbol_text::Format;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------------------------
// Command line and connecting, before the screen is set up

enum class Mode { Local, Route, Ssc, Psk };

struct Options {
    Mode mode = Mode::Local;
    std::string ip; // with --psk maybe "ip:port"
    AmsNetId target{};
    bool hasTarget = false;
    uint16_t port = 851;
    std::string addRouteUser;
    AmsNetId own{};
    bool hasOwn = false;
};

int Usage() {
    std::fprintf(stderr,
                 "usage: symbol-browser-tui [options] [<target netid>[:<port>]]\n"
                 "  without options       the local TwinCAT router; without target the PLC of this system (port 851)\n"
                 "  --route <ip>          over an ADS route to <ip>, plain ADS\n"
                 "  --ssc <ip>            over Secure ADS with self-signed certificates\n"
                 "  --psk <ip>[:<port>]   over Secure ADS with a pre-shared key (asks for identity and password)\n"
                 "  --add-route <user>    with --route or --ssc: add the route on the target first (asks for the password)\n"
                 "  --own-netid <netid>   own AmsNetId on the route way (default: own IP address + .1.1)\n");
    return 2;
}

bool ParseTarget(const std::string& text, Options& o) {
    const size_t colon = text.find(':');
    if (colon != std::string::npos) {
        const unsigned long port = std::strtoul(text.c_str() + colon + 1, nullptr, 10);
        if (!port || port > 65535) return false;
        o.port = static_cast<uint16_t>(port);
    }
    o.hasTarget = symbol_text::ParseNetId(text.substr(0, colon), o.target);
    return o.hasTarget;
}

bool ParseOptions(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if ((a == "--route" || a == "--ssc" || a == "--psk") && hasValue) {
            o.mode = a == "--route" ? Mode::Route : a == "--ssc" ? Mode::Ssc : Mode::Psk;
            o.ip = argv[++i];
        } else if (a == "--add-route" && hasValue) {
            o.addRouteUser = argv[++i];
        } else if (a == "--own-netid" && hasValue) {
            if (!symbol_text::ParseNetId(argv[++i], o.own)) return false;
            o.hasOwn = true;
        } else if (!a.empty() && a[0] != '-' && !o.hasTarget) {
            if (!ParseTarget(a, o)) return false;
        } else {
            return false;
        }
    }
    if (!o.addRouteUser.empty() && o.mode != Mode::Route && o.mode != Mode::Ssc) return false;
    return true;
}

std::string HostOf(const std::string& ip) {
    return ip.substr(0, ip.find(':'));
}

// The address of this computer that the target sees, found by asking the system for a route to it.
std::string OwnAddressFor(const std::string& target) {
    std::string own;
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return own;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(48899);
    if (inet_pton(AF_INET, target.c_str(), &to.sin_addr) == 1 &&
        connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in me{};
        socklen_t length = sizeof(me);
        char text[INET_ADDRSTRLEN] = {};
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &length) == 0 &&
            inet_ntop(AF_INET, &me.sin_addr, text, sizeof(text))) {
            own = text;
        }
    }
    close(s);
    return own;
}

std::string ReadLine(const char* prompt) {
    std::fprintf(stderr, "%s", prompt);
    std::string line;
    std::getline(std::cin, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

// Reads a line without echo when standard input is a terminal.
std::string ReadPassword(const char* prompt) {
    termios old{};
    const bool terminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (terminal) {
        termios silent = old;
        silent.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &silent);
    }
    const std::string line = ReadLine(prompt);
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        std::fprintf(stderr, "\n");
    }
    return line;
}

struct Connection {
    std::unique_ptr<AdsDevice> device;
    std::unique_ptr<AdsSymbolTable> table;
    std::string title; // target and way for the header
    bool routeAdded = false; // the library's route, removed at the end
    AmsNetId target{};
};

// Connects as the options say and uploads the symbols; messages go to standard error.
Connection Connect(Options o) {
    Connection c;
    if (o.mode != Mode::Local) {
        const std::string host = HostOf(o.ip);
        if (!o.hasTarget) {
            std::fprintf(stderr, "Asking %s for its AmsNetId (UDP 48899) ...\n", host.c_str());
            const long error = bhf::ads::GetRemoteAddress(host, o.target);
            if (error) throw AdsException(error);
            o.hasTarget = true;
        }
        if (!o.hasOwn) {
            const std::string ownIp = OwnAddressFor(host);
            if (ownIp.empty() || !symbol_text::ParseNetId(ownIp + ".1.1", o.own)) {
                throw std::runtime_error("no own address towards " + host + ", give --own-netid");
            }
        }
        if (!o.addRouteUser.empty()) {
            const std::string password = ReadPassword(("Password of " + o.addRouteUser + " on " + host + ": ").c_str());
            long error;
            if (o.mode == Mode::Ssc) {
                // Inside TLS on TCP 8016: the password does not go over the network in plain text.
                error = AdsAddRemoteRouteSecure(host.c_str(), o.own, o.addRouteUser.c_str(), password.c_str(), nullptr,
                                                nullptr, nullptr);
            } else {
                char name[256] = {};
                gethostname(name, sizeof(name) - 1);
                error = bhf::ads::AddRemoteRoute(host, o.own, OwnAddressFor(host), name, o.addRouteUser, password);
            }
            if (error) throw AdsException(error);
            std::fprintf(stderr, "Route added on %s\n", host.c_str());
        }
        AdsSetLocalAddress(o.own);
        long error = 0;
        if (o.mode == Mode::Route) error = AdsAddRoute(o.target, o.ip.c_str());
        if (o.mode == Mode::Ssc) error = AdsAddRouteSecure(o.target, o.ip.c_str(), nullptr);
        if (o.mode == Mode::Psk) {
            const std::string identity = ReadLine("PSK identity: ");
            const std::string password = ReadPassword(("PSK password of " + identity + ": ").c_str());
            error = AdsAddRoutePsk(o.target, o.ip.c_str(), identity.c_str(), password.c_str());
        }
        if (error) throw AdsException(error);
        c.routeAdded = true;
        c.target = o.target;
    }
    std::fprintf(stderr, "Connecting and uploading the symbols ...\n");
    try {
        c.device = o.mode == Mode::Local ? std::unique_ptr<AdsDevice>(new AdsDevice(o.target, o.port))
                                         : std::unique_ptr<AdsDevice>(new AdsDevice(o.target, o.port, AdsRouteAdded{}));
        c.table.reset(new AdsSymbolTable(*c.device));
    } catch (...) {
        c.table.reset();
        c.device.reset();
        if (c.routeAdded) AdsDelRoute(c.target);
        throw;
    }
    const char* way[] = {"local router", "route", "Secure ADS (self-signed)", "Secure ADS (PSK)"};
    c.title = (o.hasTarget ? symbol_text::NetIdText(o.target) : std::string("this system")) + ":" +
              std::to_string(o.port) + "  " + way[static_cast<int>(o.mode)];
    return c;
}

// ---------------------------------------------------------------------------------------------------------------
// Text on the screen: UTF-8 to wide characters, cut or padded to a width in columns

std::wstring Wide(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<uint8_t>(s[i]);
        const int n = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 0;
        if (!n || i + static_cast<size_t>(n) > s.size()) {
            w += L'\xFFFD';
            ++i;
            continue;
        }
        uint32_t u = n == 1 ? c : c & (0x7F >> n);
        for (int k = 1; k < n; ++k) u = u << 6 | (static_cast<uint8_t>(s[i + static_cast<size_t>(k)]) & 0x3F);
        w += static_cast<wchar_t>(u);
        i += static_cast<size_t>(n);
    }
    return w;
}

int CharWidth(wchar_t c) {
    return c < 32 ? 1 : std::max(0, wcwidth(c));
}

// Cut to width columns (with an ellipsis) or pad with blanks.
std::wstring Fit(const std::wstring& text, int width) {
    int total = 0;
    for (const wchar_t c : text) total += CharWidth(c);
    std::wstring out;
    int used = 0;
    const int room = total > width ? width - 1 : width;
    for (const wchar_t c : text) {
        const int w = CharWidth(c);
        if (used + w > room) break;
        out += c < 32 ? L'?' : c;
        used += w;
    }
    if (total > width && width > 0) {
        out += L'\x2026';
        ++used;
    }
    return out + std::wstring(static_cast<size_t>(std::max(0, width - used)), L' ');
}

void Put(int y, int x, int width, const std::string& text, attr_t attr = A_NORMAL) {
    if (width <= 0) return;
    const std::wstring w = Fit(Wide(text), width);
    attron(attr);
    mvaddnwstr(y, x, w.c_str(), -1);
    attroff(attr);
}

// ---------------------------------------------------------------------------------------------------------------
// The browser

struct Node {
    AdsElement element;
    int depth;
    bool expanded = false;
    bool loaded = false;
    std::vector<int> children;
    std::string value;
    bool error = false;
    Clock::time_point changed{};
};

struct Note {
    uint32_t id;
    int64_t timeStamp;
    std::vector<uint8_t> data;
};

struct Watch {
    std::unique_ptr<AdsNotification> notification;
    AdsElement element;
};

enum Pair : short { PAIR_BAR = 1, PAIR_CHANGED, PAIR_ERROR };

struct App {
    Connection connection;
    std::vector<Node> nodes;
    std::vector<int> roots;
    std::vector<int> visible;
    int cursor = 0;
    int top = 0;

    std::string filter;
    bool editingFilter = false;
    std::string filterBefore;
    bool libraries = false;
    bool autoRead = true;
    Clock::time_point nextRead{};
    std::string state;

    std::deque<std::string> log;
    std::map<std::string, uint32_t> watchedPaths;
    std::map<uint32_t, Watch> watches;
    uint32_t nextWatch = 1;
    std::mutex notesMutex;
    std::vector<Note> notes; // from the library's thread
    bool colors = false;
};

App app;
std::atomic<bool> quit{false};

void Log(const std::string& text) {
    app.log.push_back(text);
    while (app.log.size() > 500) app.log.pop_front();
}

// Screen areas: header, column titles, tree, separator, log, full path, keys
struct Layout {
    int treeTop, treeRows, logTop, logRows, pathRow, keysRow;
    int nameW, typeW, sizeW, valueW;
};

Layout GetLayout() {
    Layout l{};
    l.logRows = LINES >= 24 ? 5 : LINES >= 16 ? 3 : 1;
    l.keysRow = LINES - 1;
    l.pathRow = LINES - 2;
    l.logTop = l.pathRow - l.logRows;
    l.treeTop = 2;
    l.treeRows = std::max(1, l.logTop - 1 - l.treeTop);
    l.sizeW = 6;
    l.typeW = COLS >= 100 ? 22 : 16;
    l.valueW = std::max(12, (COLS - l.typeW - l.sizeW) * 2 / 5);
    l.nameW = std::max(10, COLS - l.typeW - l.sizeW - l.valueW - 3);
    return l;
}

void ClampCursor() {
    const int rows = GetLayout().treeRows;
    const int count = static_cast<int>(app.visible.size());
    app.cursor = std::max(0, std::min(app.cursor, count - 1));
    if (app.cursor < app.top) app.top = app.cursor;
    if (app.cursor >= app.top + rows) app.top = app.cursor - rows + 1;
    app.top = std::max(0, std::min(app.top, std::max(0, count - rows)));
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
    ClampCursor();
}

void BuildRoots() {
    app.nodes.clear();
    app.roots.clear();
    const std::string filter = symbol_text::Upper(app.filter);
    for (const auto& s : app.connection.table->Symbols()) {
        if (!app.libraries && symbol_text::IsLibrarySymbol(s.name)) continue;
        if (!filter.empty() && symbol_text::Upper(s.name).find(filter) == std::string::npos) continue;
        Node n;
        n.element = app.connection.table->Root(s);
        n.depth = 0;
        app.roots.push_back(static_cast<int>(app.nodes.size()));
        app.nodes.push_back(std::move(n));
    }
    app.cursor = 0;
    app.top = 0;
    RebuildVisible();
    app.nextRead = Clock::now();
}

Node* NodeAt(int row) {
    if (row < 0 || row >= static_cast<int>(app.visible.size())) return nullptr;
    return &app.nodes[static_cast<size_t>(app.visible[static_cast<size_t>(row)])];
}

void Toggle(int row, int expand) { // expand: 1 open, 0 close, -1 switch
    Node* n = NodeAt(row);
    if (!n || !n->element.hasChildren) return;
    const bool open = expand < 0 ? !n->expanded : expand == 1;
    if (open == n->expanded) return;
    const int i = app.visible[static_cast<size_t>(row)];
    if (open && !n->loaded) {
        const auto children = app.connection.table->Children(n->element);
        const int depth = n->depth + 1;
        n->loaded = true;
        std::vector<int> ids;
        for (const auto& c : children) { // may move the nodes, so no use of n below
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
    app.nextRead = Clock::now();
}

// Left on an open node closes it, otherwise goes to the parent.
void Left() {
    Node* n = NodeAt(app.cursor);
    if (!n) return;
    if (n->expanded) {
        Toggle(app.cursor, 0);
        return;
    }
    for (int row = app.cursor - 1; row >= 0; --row) {
        if (NodeAt(row)->depth < n->depth) {
            app.cursor = row;
            ClampCursor();
            return;
        }
    }
}

void UploadAgain() {
    app.watches.clear();
    app.watchedPaths.clear();
    try {
        app.connection.table.reset(new AdsSymbolTable(*app.connection.device));
        Log(Format("Symbols uploaded again: %zu symbols, version %u", app.connection.table->Symbols().size(),
                   app.connection.table->Version()));
        app.autoRead = true;
    } catch (const AdsException& e) {
        Log(Format("Upload failed: 0x%lX", e.errorCode));
        app.connection.table.reset(new AdsSymbolTable({}, {}, 0));
    }
    BuildRoots();
}

// Reads the values of the leaves on screen with sum reads; also checks that the PLC still has the program the table
// was uploaded from.
void ReadVisible() {
    try {
        const auto s = app.connection.device->GetState();
        app.state = Format("%s (%d)", symbol_text::StateName(s.ads), static_cast<int>(s.ads));
    } catch (const AdsException& e) {
        app.state = Format("error 0x%lX", e.errorCode);
    }
    const int rows = GetLayout().treeRows;
    std::vector<int> ids;
    std::vector<AdsElement> elements;
    for (int row = app.top; row < app.top + rows && row < static_cast<int>(app.visible.size()); ++row) {
        const int i = app.visible[static_cast<size_t>(row)];
        const auto& e = app.nodes[static_cast<size_t>(i)].element;
        if (e.hasChildren) continue; // structs and arrays have no value of their own
        ids.push_back(i);
        elements.push_back(e);
    }
    if (elements.empty()) return;
    std::vector<AdsReadResult> results;
    try {
        results = ReadElements(*app.connection.device, *app.connection.table, elements);
    } catch (const AdsException& e) {
        if (e.errorCode == ADSERR_DEVICE_SYMBOLVERSIONINVALID) {
            app.autoRead = false;
            Log("The PLC has a new program: press u to upload its symbols again");
        } else {
            Log(Format("Reading failed: 0x%lX", e.errorCode));
        }
        return;
    }
    const auto now = Clock::now();
    for (size_t k = 0; k < ids.size(); ++k) {
        Node& n = app.nodes[static_cast<size_t>(ids[k])];
        const auto& r = results[k];
        const std::string value =
            r.error ? Format("<error 0x%lX>", r.error) : symbol_text::ValueText(n.element, r.data);
        if (!n.value.empty() && value != n.value) n.changed = now;
        n.value = value;
        n.error = r.error != 0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Notifications

void OnNotification(const AmsAddr*, const AdsNotificationHeader* header, uint32_t id) {
    // Called in a thread of the library: hand a copy to the main loop.
    std::lock_guard<std::mutex> lock(app.notesMutex);
    app.notes.push_back({id, header->nTimeStamp, std::vector<uint8_t>(header->data, header->data + header->cbSampleSize)});
}

void ToggleWatch() {
    Node* n = NodeAt(app.cursor);
    if (!n || n->element.hasChildren) return;
    const AdsElement e = n->element;
    const auto found = app.watchedPaths.find(e.path);
    if (found != app.watchedPaths.end()) {
        app.watches.erase(found->second);
        app.watchedPaths.erase(found);
        Log("Notification removed: " + e.path);
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
        const AdsDevice& device = *app.connection.device;
        w.notification.reset(e.kind == AdsElement::Kind::Property
                                 ? new AdsNotification(device, e.path, attrib, &OnNotification, id)
                                 : new AdsNotification(device, e.indexGroup, e.indexOffset, attrib, &OnNotification, id));
        app.watches[id] = std::move(w);
        app.watchedPaths[e.path] = id;
        Log("Notification added: " + e.path);
    } catch (const AdsException& ex) {
        Log(Format("Notification for %s failed: 0x%lX", e.path.c_str(), ex.errorCode));
    }
}

void ShowNotes() {
    std::vector<Note> notes;
    {
        std::lock_guard<std::mutex> lock(app.notesMutex);
        notes.swap(app.notes);
    }
    for (const auto& m : notes) {
        const auto w = app.watches.find(m.id);
        if (w == app.watches.end()) continue; // removed in the meantime
        // ADS time stamps count 100 ns since 1601-01-01 (UTC).
        const int64_t unix100ns = m.timeStamp - 116444736000000000LL;
        const time_t seconds = static_cast<time_t>(unix100ns / 10000000);
        tm local{};
        localtime_r(&seconds, &local);
        Log(Format("%02d:%02d:%02d.%03d  ", local.tm_hour, local.tm_min, local.tm_sec,
                   static_cast<int>(unix100ns / 10000 % 1000)) +
            w->second.element.path + "  " + symbol_text::ValueText(w->second.element, m.data));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing

attr_t Color(Pair pair, attr_t fallback) {
    return app.colors ? static_cast<attr_t>(COLOR_PAIR(pair)) : fallback;
}

void Draw() {
    erase();
    if (LINES < 10 || COLS < 50) {
        Put(0, 0, COLS, "Terminal too small (at least 50 x 10)");
        refresh();
        return;
    }
    const Layout l = GetLayout();
    const auto& table = *app.connection.table;
    const attr_t bar = Color(PAIR_BAR, A_REVERSE);

    std::string header = " " + app.connection.title + "   " + app.state + "   " +
                         Format("%zu symbols, version %u", table.Symbols().size(), table.Version());
    if (!app.filter.empty() || app.editingFilter) header += "   filter: " + app.filter;
    if (app.libraries) header += "   [libraries]";
    if (!app.autoRead) header += "   [paused]";
    Put(0, 0, COLS, header, bar);
    Put(1, 0, l.nameW + 1, " Name", A_BOLD);
    Put(1, l.nameW + 1, l.typeW + 1, "Type", A_BOLD);
    Put(1, l.nameW + l.typeW + 2, l.sizeW, Format("%*s", l.sizeW - 1, "Size"), A_BOLD);
    Put(1, l.nameW + l.typeW + l.sizeW + 3, l.valueW, "Value", A_BOLD);

    const auto now = Clock::now();
    for (int r = 0; r < l.treeRows; ++r) {
        const int row = app.top + r;
        const Node* n = NodeAt(row);
        if (!n) break;
        const int y = l.treeTop + r;
        const bool selected = row == app.cursor;
        const attr_t base = selected ? A_REVERSE : A_NORMAL;
        const bool watched = app.watchedPaths.count(n->element.path) != 0;
        std::string name = std::string(static_cast<size_t>(n->depth) * 2, ' ');
        name += n->element.hasChildren ? (n->expanded ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ") : "  ";
        name += symbol_text::RowName(n->element.path, n->depth);
        if (watched) name += " \xE2\x97\x8F"; // ●
        Put(y, 0, l.nameW, " " + name, base);
        Put(y, l.nameW, 1, "", base);
        Put(y, l.nameW + 1, l.typeW, n->element.typeName, base);
        Put(y, l.nameW + l.typeW + 1, 1, "", base);
        Put(y, l.nameW + l.typeW + 2, l.sizeW + 1, Format("%*u ", l.sizeW - 1, n->element.size), base);
        attr_t valueAttr = base;
        if (n->error) valueAttr |= Color(PAIR_ERROR, A_BOLD);
        else if (now - n->changed < std::chrono::seconds(1)) valueAttr |= Color(PAIR_CHANGED, A_BOLD);
        Put(y, l.nameW + l.typeW + l.sizeW + 3, l.valueW, n->element.hasChildren ? "" : n->value, valueAttr);
    }
    if (app.visible.empty()) Put(l.treeTop, 1, COLS - 1, app.filter.empty() ? "(no symbols)" : "(no symbol matches the filter)");

    mvhline(l.logTop - 1, 0, ACS_HLINE, COLS);
    const size_t first = app.log.size() > static_cast<size_t>(l.logRows) ? app.log.size() - static_cast<size_t>(l.logRows) : 0;
    for (size_t k = first; k < app.log.size(); ++k) {
        Put(l.logTop + static_cast<int>(k - first), 0, COLS, " " + app.log[k]);
    }

    const Node* n = NodeAt(app.cursor);
    std::string path;
    if (n) {
        path = " " + n->element.path;
        if (!n->element.hasChildren && !n->value.empty()) path += " = " + n->value;
        path += Format("   (group 16#%X, offset 16#%X)", n->element.indexGroup, n->element.indexOffset);
    }
    Put(l.pathRow, 0, COLS, path, A_BOLD);
    const std::string keys = app.editingFilter
                                 ? " Filter: " + app.filter + "_   Enter keep   Esc cancel"
                                 : " \xE2\x86\x91\xE2\x86\x93 move  \xE2\x86\x92\xE2\x86\x90/Enter open/close  / filter  b libraries  "
                                   "n notification  p pause  r read  u upload  c clear log  q quit";
    Put(l.keysRow, 0, COLS, keys, bar);
    refresh();
}

// ---------------------------------------------------------------------------------------------------------------
// Input

void Move(int delta) {
    app.cursor += delta;
    ClampCursor();
    app.nextRead = Clock::now();
}

void Scroll(int delta) {
    app.top += delta;
    const int rows = GetLayout().treeRows;
    app.top = std::max(0, std::min(app.top, std::max(0, static_cast<int>(app.visible.size()) - rows)));
    app.cursor = std::max(app.top, std::min(app.cursor, app.top + rows - 1));
    ClampCursor();
    app.nextRead = Clock::now();
}

void AppendUtf8(std::string& s, wint_t c) {
    symbol_text::AppendUtf8(s, static_cast<uint32_t>(c));
}

void EraseLastChar(std::string& s) {
    while (!s.empty()) {
        const auto c = static_cast<uint8_t>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

void OnFilterKey(bool isKey, wint_t c) {
    if (isKey && c == KEY_BACKSPACE) {
        EraseLastChar(app.filter);
    } else if (!isKey && (c == 127 || c == 8)) {
        EraseLastChar(app.filter);
    } else if ((isKey && c == KEY_ENTER) || (!isKey && (c == '\n' || c == '\r'))) {
        app.editingFilter = false;
        return;
    } else if (!isKey && c == 27) {
        app.editingFilter = false;
        app.filter = app.filterBefore;
    } else if (!isKey && c >= 32) {
        AppendUtf8(app.filter, c);
    } else {
        return;
    }
    BuildRoots();
}

void OnMouse() {
    MEVENT m{};
    if (getmouse(&m) != OK) return;
    const Layout l = GetLayout();
    if (m.bstate & BUTTON4_PRESSED) {
        Scroll(-3);
        return;
    }
#ifdef BUTTON5_PRESSED
    if (m.bstate & BUTTON5_PRESSED) {
        Scroll(3);
        return;
    }
#endif
    if (m.y < l.treeTop || m.y >= l.treeTop + l.treeRows) return;
    const int row = app.top + m.y - l.treeTop;
    const Node* n = NodeAt(row);
    if (!n) return;
    app.cursor = row;
    ClampCursor();
    if (m.bstate & (BUTTON3_CLICKED | BUTTON3_PRESSED)) {
        ToggleWatch();
    } else if (m.bstate & BUTTON1_DOUBLE_CLICKED) {
        Toggle(row, -1);
    } else if (m.bstate & BUTTON1_CLICKED) {
        const int arrow = 1 + n->depth * 2; // column of ▸/▾
        if (m.x >= arrow && m.x <= arrow + 1) Toggle(row, -1);
    }
    app.nextRead = Clock::now();
}

void OnKey(bool isKey, wint_t c) {
    if (app.editingFilter) {
        OnFilterKey(isKey, c);
        return;
    }
    const int page = std::max(1, GetLayout().treeRows - 1);
    if (isKey) {
        switch (c) {
        case KEY_UP: Move(-1); break;
        case KEY_DOWN: Move(1); break;
        case KEY_PPAGE: Move(-page); break;
        case KEY_NPAGE: Move(page); break;
        case KEY_HOME: Move(-static_cast<int>(app.visible.size())); break;
        case KEY_END: Move(static_cast<int>(app.visible.size())); break;
        case KEY_RIGHT: Toggle(app.cursor, 1); break;
        case KEY_LEFT: Left(); break;
        case KEY_ENTER: Toggle(app.cursor, -1); break;
        case KEY_MOUSE: OnMouse(); break;
        case KEY_F(10): quit = true; break;
        default: break;
        }
        return;
    }
    switch (c) {
    case '\n':
    case '\r':
    case ' ': Toggle(app.cursor, -1); break;
    case '+': Toggle(app.cursor, 1); break;
    case '-': Left(); break;
    case '/':
        app.editingFilter = true;
        app.filterBefore = app.filter;
        break;
    case 'b':
        app.libraries = !app.libraries;
        BuildRoots();
        break;
    case 'n': ToggleWatch(); break;
    case 'p':
        app.autoRead = !app.autoRead;
        app.nextRead = Clock::now();
        break;
    case 'r': app.nextRead = Clock::now(); break;
    case 'u': UploadAgain(); break;
    case 'c': app.log.clear(); break;
    case 'q': quit = true; break;
    default: break;
    }
}

void OnSignal(int) {
    quit = true;
}

void Run() {
    BuildRoots();
    Log(Format("%zu symbols, %zu data types; Enter opens, n watches a value, / filters, q quits",
               app.connection.table->Symbols().size(), app.connection.table->DataTypes().size()));
    while (!quit) {
        ShowNotes();
        // Every 500 ms, and at once after anything that changes the rows on screen (also while paused)
        if (Clock::now() >= app.nextRead) {
            ReadVisible();
            app.nextRead = app.autoRead ? Clock::now() + std::chrono::milliseconds(500) : Clock::time_point::max();
        }
        Draw();
        wint_t c = 0;
        const int kind = get_wch(&c);
        if (kind == ERR) continue;
        if (kind == KEY_CODE_YES && c == KEY_RESIZE) continue;
        OnKey(kind == KEY_CODE_YES, c);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) return Usage();
    try {
        app.connection = Connect(options);
    } catch (const AdsException& e) {
        std::fprintf(stderr, "ADS error 0x%lX\n", e.errorCode);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    // Only the character set: numbers keep the decimal point whatever the language of the system is.
    std::setlocale(LC_CTYPE, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
    timeout(100);
    mousemask(BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON3_CLICKED | BUTTON3_PRESSED | BUTTON4_PRESSED
#ifdef BUTTON5_PRESSED
                  | BUTTON5_PRESSED
#endif
              ,
              nullptr);
    mouseinterval(200);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(PAIR_BAR, COLOR_BLACK, COLOR_CYAN);
        init_pair(PAIR_CHANGED, COLOR_YELLOW, -1);
        init_pair(PAIR_ERROR, COLOR_RED, -1);
        app.colors = true;
    }

    Run();
    endwin();

    // Notifications go before the device they belong to, the device before its route.
    app.watches.clear();
    app.connection.table.reset();
    app.connection.device.reset();
    if (app.connection.routeAdded) AdsDelRoute(app.connection.target);
    return 0;
}
