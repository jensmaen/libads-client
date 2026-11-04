// Fake TwinCAT router for tests: behaves on TCP like the local
// TwinCAT router (port connect 0x1000, port close 0x0001, router note 0x1001) and answers the ADS requests our tests
// use as if it were the target itself, so it serves both ways: as local router and as target of an ADS route.
// It can drop all connections, stop listening, restart (ports and notifications forgotten, port numbers from the
// start again), refuse a port and go silent. Plain TCP only; Secure ADS is tested against the Linux VM.
//
// Header only and independent of libads-client, so tests can run it in their own process and the code under test
// does not test itself. Simulated device: ADS state (5 = run), a counter MAIN.nCounter (UDINT, +1 every 10 ms) at
// index group 0x4040, offset 0x1000, found by name with ADSIGRP_SYM_INFOBYNAMEEX (0xF009). Requests to port 851
// fail with AMS error 0x6 while the state is not run, like a PLC that is not running.

#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

class FakeRouter {
public:
    static constexpr uint16_t CMD_ADS = 0x0000;
    static constexpr uint16_t CMD_PORT_CLOSE = 0x0001;
    static constexpr uint16_t CMD_PORT_CONNECT = 0x1000;
    static constexpr uint16_t CMD_ROUTER_NOTE = 0x1001;
    static constexpr uint32_t COUNTER_GROUP = 0x4040;
    static constexpr uint32_t COUNTER_OFFSET = 0x1000;

    struct Options {
        uint16_t listenPort = 48900; // not 48898, that belongs to TwinCAT
        std::array<uint8_t, 6> netId{{192, 168, 0, 99, 1, 1}};
        uint16_t firstPort = 32777; // first port the router assigns, as TwinCAT for Linux
    };

    explicit FakeRouter(const Options& options) : opt(options), started(Clock::now()) {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        nextPort = opt.firstPort;
        notifier = std::thread([this] { Notify(); });
    }

    ~FakeRouter() {
        Stop(false);
        quit = true;
        notifier.join();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    FakeRouter(const FakeRouter&) = delete;
    FakeRouter& operator=(const FakeRouter&) = delete;

    /** Starts listening on 127.0.0.1:listenPort. @return false if the port cannot be bound */
    bool Start() {
        std::lock_guard<std::mutex> lock(control);
        if (listening) return true;
        Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == BAD_SOCKET) return false;
#ifndef _WIN32
        int yes = 1; // listen again right after Stop(), despite connections in TIME_WAIT
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(opt.listenPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) || listen(s, 16)) {
            CloseSocket(s);
            return false;
        }
        listener = s;
        listening = true;
        acceptor = std::thread([this] { Accept(); });
        return true;
    }

    /**
     * The router goes away: stops listening and closes every connection (new connections are refused).
     * @param announce first send 0x1001 "stopped" to every port, as TwinCAT for Linux does
     */
    void Stop(bool announce = true) {
        std::lock_guard<std::mutex> lock(control);
        if (announce) SendRouterEvent(0);
        if (listening) {
            listening = false;
            acceptor.join();
            CloseSocket(listener);
        }
        DropAllLocked();
    }

    /** Like a restart of the router process: Stop(), forget ports and notifications, Start(). */
    bool Restart(bool announce = true) {
        Stop(announce);
        {
            std::lock_guard<std::mutex> lock(mutex);
            used.clear();
            subscriptions.clear();
            nextPort = opt.firstPort;
            nextHandle = 1;
        }
        return Start();
    }

    /** Closes every connection and keeps listening (the network was cut short). */
    void DropAll() {
        std::lock_guard<std::mutex> lock(control);
        DropAllLocked();
    }

    /** Silent: connections are accepted and read, but nothing is answered or sent (the network is gone). */
    void SetSilent(bool on) { silent = on; }

    /** A port connect asking for this port is refused: the router closes the connection without an answer. */
    void RefusePort(uint16_t port, bool refuse = true) {
        std::lock_guard<std::mutex> lock(mutex);
        if (refuse) refused.insert(port); else refused.erase(port);
    }

    /** Sends 0x1001 with this state (0 stopped, 1 started, 2 removed) to every connection with a port. */
    void SendRouterEvent(uint32_t state) {
        uint8_t frame[10];
        Put16(frame, CMD_ROUTER_NOTE);
        Put32(frame + 2, 4);
        Put32(frame + 6, state);
        for (const auto& c : Snapshot()) {
            if (c->port) c->Send(frame, sizeof(frame));
        }
    }

    void SetAdsState(uint16_t state) { adsState = state; }
    uint16_t AdsState() const { return adsState; }

    /** Current value of MAIN.nCounter */
    uint32_t Counter() const {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count() / 10);
    }

    /** Open connections */
    size_t Connections() const {
        const auto all = Snapshot();
        return static_cast<size_t>(std::count_if(all.begin(), all.end(), [](const SharedConnection& c) { return !c->done; }));
    }

    /** Ports assigned to open connections */
    std::vector<uint16_t> Ports() const {
        std::lock_guard<std::mutex> lock(mutex);
        return std::vector<uint16_t>(used.begin(), used.end());
    }

    size_t Notifications() const {
        std::lock_guard<std::mutex> lock(mutex);
        return subscriptions.size();
    }

    /** Number of ADS requests answered so far */
    uint32_t Requests() const { return requests; }

    std::string Status() const {
        std::ostringstream s;
        s << (listening ? "listening" : "not listening") << " on 127.0.0.1:" << opt.listenPort
          << (silent ? ", silent" : "") << ", ADS state " << adsState << ", " << Connections() << " connections, ports";
        for (const auto p : Ports()) s << ' ' << p;
        s << ", " << Notifications() << " notifications, " << requests << " requests, counter " << Counter();
        return s.str();
    }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket BAD_SOCKET = INVALID_SOCKET;
    static void CloseSocket(Socket s) { closesocket(s); }
    static void ShutdownSocket(Socket s) { shutdown(s, SD_BOTH); }
    static constexpr int SEND_FLAGS = 0;
#else
    using Socket = int;
    static constexpr Socket BAD_SOCKET = -1;
    static void CloseSocket(Socket s) { close(s); }
    static void ShutdownSocket(Socket s) { shutdown(s, SHUT_RDWR); }
    static constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#endif
    using Clock = std::chrono::steady_clock;

    struct Connection {
        Socket socket = BAD_SOCKET;
        uint16_t port = 0; // assigned with 0x1000, 0 for a route connection
        std::mutex sending; // also guards closing the socket
        bool open = true;
        std::thread thread;
        std::atomic<bool> closing{false};
        std::atomic<bool> done{false};

        void Send(const uint8_t* data, size_t length) {
            std::lock_guard<std::mutex> lock(sending);
            while (open && length) {
                const int n = send(socket, reinterpret_cast<const char*>(data), static_cast<int>(length), SEND_FLAGS);
                if (n <= 0) return;
                data += n;
                length -= static_cast<size_t>(n);
            }
        }
        // Only asks the serving thread to close: under Windows shutdown() does not wake a blocking recv() in
        // another thread, and closing the socket from here could hit a reused handle.
        void Close() { closing = true; }
        void Release() {
            std::lock_guard<std::mutex> lock(sending);
            if (!open) return;
            open = false;
            ShutdownSocket(socket);
            CloseSocket(socket);
        }
    };
    using SharedConnection = std::shared_ptr<Connection>;

    struct Subscription {
        std::weak_ptr<Connection> connection;
        std::array<uint8_t, 8> client; // address of the program (target of the notification frames)
        std::array<uint8_t, 8> device; // address the program subscribed at (source of the frames)
        uint32_t length;
        std::chrono::milliseconds cycle;
        Clock::time_point due;
    };

    const Options opt;
    const Clock::time_point started;

    std::mutex control; // serializes Start/Stop/DropAll
    std::atomic<bool> listening{false};
    Socket listener = BAD_SOCKET;
    std::thread acceptor;

    mutable std::mutex mutex; // guards the members below
    std::list<SharedConnection> connections;
    std::set<uint16_t> used;
    std::set<uint16_t> refused;
    uint16_t nextPort;
    uint32_t nextHandle = 1;
    std::map<uint32_t, Subscription> subscriptions;

    std::atomic<bool> silent{false};
    std::atomic<uint16_t> adsState{5};
    std::atomic<uint32_t> requests{0};
    std::atomic<bool> quit{false};
    std::thread notifier;

    static void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
    static void Put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
    static uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
    static uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
    static void Append16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
    static void Append32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

    std::list<SharedConnection> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return connections;
    }

    void DropAllLocked() {
        std::list<SharedConnection> all;
        {
            std::lock_guard<std::mutex> lock(mutex);
            all.swap(connections);
        }
        for (auto& c : all) c->Close();
        for (auto& c : all) c->thread.join();
    }

    void Accept() {
        while (listening) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener, &set);
            timeval timeout{0, 100000};
            if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0) continue;
            const Socket s = accept(listener, nullptr, nullptr);
            if (s == BAD_SOCKET) continue;
            int yes = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
            auto c = std::make_shared<Connection>();
            c->socket = s;
            c->thread = std::thread([this, c] { Serve(c); }); // before it is listed, DropAll() joins it
            std::list<SharedConnection> finished;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto it = connections.begin(); it != connections.end();) {
                    if ((*it)->done) { finished.push_back(*it); it = connections.erase(it); } else ++it;
                }
                connections.push_back(c);
            }
            for (auto& f : finished) f->thread.join();
        }
    }

    static bool ReadAll(Connection& c, uint8_t* data, size_t length) {
        while (length) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(c.socket, &set);
            timeval timeout{0, 50000};
            const int ready = select(static_cast<int>(c.socket + 1), &set, nullptr, nullptr, &timeout);
            if (c.closing || ready < 0) return false;
            if (!ready) continue;
            const int n = recv(c.socket, reinterpret_cast<char*>(data), static_cast<int>(length), 0);
            if (n <= 0) return false;
            data += n;
            length -= static_cast<size_t>(n);
        }
        return true;
    }

    void Serve(const SharedConnection& c) {
        std::vector<uint8_t> payload;
        for (;;) {
            uint8_t header[6];
            if (!ReadAll(*c, header, sizeof(header))) break;
            const uint16_t command = Get16(header);
            const uint32_t length = Get32(header + 2);
            if (length > (1u << 20)) break;
            payload.resize(length);
            if (length && !ReadAll(*c, payload.data(), length)) break;
            if (silent) continue;
            if (command == CMD_PORT_CONNECT) {
                if (!ConnectPort(c, length >= 2 ? Get16(payload.data()) : 0)) break;
            } else if (command == CMD_PORT_CLOSE) {
                break; // the router closes the connection, that is the confirmation
            } else if (command == CMD_ADS && length >= 32) {
                Answer(c, payload);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (c->port) used.erase(c->port);
            for (auto it = subscriptions.begin(); it != subscriptions.end();) {
                if (it->second.connection.lock() == c) it = subscriptions.erase(it); else ++it;
            }
        }
        c->Release();
        c->done = true;
    }

    bool ConnectPort(const SharedConnection& c, uint16_t wanted) {
        uint16_t port = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (c->port) return false;
            if (wanted) {
                if (used.count(wanted) || refused.count(wanted)) return false; // close without answer
                port = wanted;
            } else {
                while (used.count(nextPort) || refused.count(nextPort)) ++nextPort;
                port = nextPort++;
            }
            used.insert(port);
            c->port = port;
        }
        uint8_t frame[14];
        Put16(frame, CMD_PORT_CONNECT);
        Put32(frame + 2, 8);
        std::memcpy(frame + 6, opt.netId.data(), 6);
        Put16(frame + 12, port);
        c->Send(frame, sizeof(frame));
        return true;
    }

    void Answer(const SharedConnection& c, const std::vector<uint8_t>& request) {
        const uint8_t* h = request.data();
        const uint16_t targetPort = Get16(h + 6);
        const uint16_t cmd = Get16(h + 16);
        const uint8_t* d = h + 32;
        const size_t n = std::min<size_t>(Get32(h + 20), request.size() - 32);
        std::vector<uint8_t> out;
        uint32_t amsError = 0;

        if (targetPort == 851 && adsState != 5) {
            amsError = 0x6; // target port not found: the PLC is not running
        } else {
            switch (cmd) {
            case 1: { // ReadDeviceInfo
                Append32(out, 0);
                out.push_back(3);
                out.push_back(1);
                Append16(out, 4026);
                char name[16] = "FakeRouter";
                out.insert(out.end(), name, name + sizeof(name));
                break;
            }
            case 2: // Read
                if (n < 12) { Append32(out, 0x706); break; }
                if (Get32(d) == COUNTER_GROUP && Get32(d + 4) == COUNTER_OFFSET) {
                    const uint32_t size = std::min<uint32_t>(Get32(d + 8), 4);
                    Append32(out, 0);
                    Append32(out, size);
                    uint8_t value[4];
                    Put32(value, Counter());
                    out.insert(out.end(), value, value + size);
                } else {
                    Append32(out, 0x702); // invalid index group
                    Append32(out, 0);
                }
                break;
            case 3: // Write
                Append32(out, 0);
                break;
            case 4: // ReadState
                Append32(out, 0);
                Append16(out, adsState);
                Append16(out, 0);
                break;
            case 5: // WriteControl
                if (n >= 2) { // reset comes back in run, reconfig in config
                    const uint16_t wanted = Get16(d);
                    adsState = wanted == 2 ? 5 : wanted == 16 ? 15 : wanted;
                }
                Append32(out, 0);
                break;
            case 6: { // AddDeviceNotification
                if (n < 24) { Append32(out, 0x706); Append32(out, 0); break; }
                if (Get32(d) != COUNTER_GROUP || Get32(d + 4) != COUNTER_OFFSET) {
                    Append32(out, 0x702);
                    Append32(out, 0);
                    break;
                }
                Subscription s;
                s.connection = c;
                std::memcpy(s.client.data(), h + 8, 8);
                std::memcpy(s.device.data(), h, 8);
                s.length = std::min<uint32_t>(Get32(d + 8), 4);
                s.cycle = std::chrono::milliseconds(std::max<uint32_t>(Get32(d + 20) / 10000, 10));
                s.due = Clock::now();
                uint32_t handle;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    handle = nextHandle++;
                    subscriptions.emplace(handle, s);
                }
                Append32(out, 0);
                Append32(out, handle);
                break;
            }
            case 7: { // DelDeviceNotification
                bool found = false;
                if (n >= 4) {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto it = subscriptions.find(Get32(d));
                    if (it != subscriptions.end() && it->second.connection.lock() == c) {
                        subscriptions.erase(it);
                        found = true;
                    }
                }
                Append32(out, found ? 0 : 0x714); // invalid notification handle
                break;
            }
            case 9: { // ReadWrite
                if (n < 16) { Append32(out, 0x706); Append32(out, 0); break; }
                const uint32_t group = Get32(d);
                const uint32_t readLength = Get32(d + 8);
                const std::string name(reinterpret_cast<const char*>(d + 16), std::min<size_t>(Get32(d + 12), n - 16));
                if (group != 0xF009) { Append32(out, 0x702); Append32(out, 0); break; }
                std::string upper = name;
                std::transform(upper.begin(), upper.end(), upper.begin(), [](char ch) { return char(std::toupper(static_cast<unsigned char>(ch))); });
                if (upper.c_str() != std::string("MAIN.NCOUNTER")) { Append32(out, 0x710); Append32(out, 0); break; }
                const std::string symbol = "MAIN.nCounter", type = "UDINT";
                std::vector<uint8_t> e; // AdsSymbolEntry
                Append32(e, 0);
                Append32(e, COUNTER_GROUP);
                Append32(e, COUNTER_OFFSET);
                Append32(e, 4);  // size
                Append32(e, 19); // ADST_UINT32
                Append32(e, 0);  // flags
                Append16(e, uint16_t(symbol.size()));
                Append16(e, uint16_t(type.size()));
                Append16(e, 0);
                e.insert(e.end(), symbol.begin(), symbol.end());
                e.push_back(0);
                e.insert(e.end(), type.begin(), type.end());
                e.push_back(0);
                e.push_back(0);
                Put32(e.data(), uint32_t(e.size()));
                if (readLength < e.size()) { Append32(out, 0x705); Append32(out, 0); break; } // invalid size
                Append32(out, 0);
                Append32(out, uint32_t(e.size()));
                out.insert(out.end(), e.begin(), e.end());
                break;
            }
            default:
                Append32(out, 0x701); // service not supported
            }
        }
        ++requests;

        std::vector<uint8_t> frame(6 + 32);
        Put16(frame.data(), CMD_ADS);
        Put32(frame.data() + 2, uint32_t(32 + out.size()));
        uint8_t* r = frame.data() + 6;
        std::memcpy(r, h + 8, 8);  // target: the requester
        std::memcpy(r + 8, h, 8);  // source: the device it asked
        Put16(r + 16, cmd);
        Put16(r + 18, 0x0005);     // response, ADS command
        Put32(r + 20, uint32_t(out.size()));
        Put32(r + 24, amsError);
        std::memcpy(r + 28, h + 28, 4); // invoke id
        frame.insert(frame.end(), out.begin(), out.end());
        c->Send(frame.data(), frame.size());
    }

    void Notify() {
        while (!quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (silent) continue;
            const auto now = Clock::now();
            std::vector<std::pair<SharedConnection, std::vector<uint8_t> > > due;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& e : subscriptions) {
                    auto& s = e.second;
                    if (now < s.due) continue;
                    s.due += s.cycle;
                    if (s.due < now) s.due = now + s.cycle;
                    auto c = s.connection.lock();
                    if (c) due.emplace_back(c, NotificationFrame(e.first, s));
                }
            }
            for (auto& d : due) d.first->Send(d.second.data(), d.second.size());
        }
    }

    std::vector<uint8_t> NotificationFrame(uint32_t handle, const Subscription& s) const {
        std::vector<uint8_t> data; // AdsNotificationStream with one stamp and one sample
        const uint32_t size = s.length;
        Append32(data, 4 + 8 + 4 + 4 + 4 + size); // length of the rest: stamps, time, samples, handle, size, data
        Append32(data, 1);                    // stamps
        const uint64_t filetime = (uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch()).count()) +
                                   11644473600ull) * 10000000ull;
        Append32(data, uint32_t(filetime));
        Append32(data, uint32_t(filetime >> 32));
        Append32(data, 1); // samples
        Append32(data, handle);
        Append32(data, size);
        uint8_t value[4];
        Put32(value, Counter());
        data.insert(data.end(), value, value + size);

        std::vector<uint8_t> frame(6 + 32);
        Put16(frame.data(), CMD_ADS);
        Put32(frame.data() + 2, uint32_t(32 + data.size()));
        uint8_t* r = frame.data() + 6;
        std::memcpy(r, s.client.data(), 8);
        std::memcpy(r + 8, s.device.data(), 8);
        Put16(r + 16, 8);      // DeviceNotification
        Put16(r + 18, 0x0004); // request, ADS command
        Put32(r + 20, uint32_t(data.size()));
        frame.insert(frame.end(), data.begin(), data.end());
        return frame;
    }
};
