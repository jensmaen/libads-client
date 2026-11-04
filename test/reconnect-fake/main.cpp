// Reconnecting AmsConnection against the fake router in the same process, no TwinCAT needed. Uses the connection
// layer directly, below LocalRouter and AmsRouter.
// Usage: reconnect-fake [first port]   (default 48910; uses this port and the next one)

#include "AmsConnection.h"
#include "Sockets.h"

#include "../fake-router/FakeRouter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

std::string hex(long v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%lX", v);
    return b;
}

long msSince(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<milliseconds>(Clock::now() - t).count());
}

bool waitFor(const std::function<bool()>& done, int ms) {
    const auto end = Clock::now() + milliseconds(ms);
    while (!done()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(milliseconds(10));
    }
    return true;
}

const AmsNetId ownNetId{192, 168, 0, 50, 1, 1};
const AmsNetId fakeNetId{192, 168, 0, 99, 1, 1};
const uint16_t ownPort = 30000;

struct TestRouter : Router {
    std::atomic<int> lost{0}, reconnected{0}, gaveUp{0};
    long GetLocalAddress(uint16_t, AmsAddr* pAddr) override {
        pAddr->netId = ownNetId;
        pAddr->port = ownPort;
        return 0;
    }
    void OnConnectionLost(AmsConnection&) override { ++lost; }
    void OnReconnected(AmsConnection&) override { ++reconnected; }
    void OnGaveUp(AmsConnection&) override { ++gaveUp; }
};

long readState(AmsConnection& c, uint32_t tmms, uint16_t* adsState = nullptr) {
    const AmsAddr target{fakeNetId, 10000};
    uint8_t buffer[4]{};
    uint32_t bytesRead = 0;
    AmsRequest request{target, ownPort, AoEHeader::READ_STATE, sizeof(buffer), buffer, &bytesRead};
    const long status = c.AdsRequest(request, tmms);
    if (adsState) *adsState = static_cast<uint16_t>(buffer[0] | buffer[1] << 8);
    return status;
}

std::unique_ptr<AmsTransport> connectTo(const std::string& host, const std::atomic<bool>& abort) {
    auto addresses = bhf::ads::GetListOfAddresses(host, "48898");
    return std::unique_ptr<AmsTransport>(new TcpTransport{addresses.get(), &abort});
}

// A peer that accepts the connection (through the backlog) and then never reads, answers or closes.
struct DeadPeer {
#ifdef _WIN32
    SOCKET s = INVALID_SOCKET;
#else
    int s = -1;
#endif
    explicit DeadPeer(uint16_t port) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        listen(s, 4);
    }
    ~DeadPeer() {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
    }
};

} // namespace

int main(int argc, char** argv) {
    const uint16_t port = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 48910);
    const std::string host = "127.0.0.1:" + std::to_string(port);
    FakeRouter::Options options;
    options.listenPort = port;
    options.netId = {{192, 168, 0, 99, 1, 1}};
    FakeRouter fake(options);
    if (!fake.Start()) {
        std::printf("cannot listen on %s\n", host.c_str());
        return 1;
    }

    TestRouter router;
    std::string target = host; // the connector of c connects here
    std::mutex targetMutex;
    auto c = std::unique_ptr<AmsConnection>(new AmsConnection{router, [&](const std::atomic<bool>& abort) {
        std::string to;
        {
            std::lock_guard<std::mutex> lock(targetMutex);
            to = target;
        }
        return connectTo(to, abort);
    }});
    uint16_t adsState = 0;
    long status = readState(*c, 1000, &adsState);
    check(!status && adsState == 5, "ReadState on a reconnecting connection (" + hex(status) + ")");

    std::printf("-- connection dropped, the router keeps listening\n");
    fake.DropAll();
    check(waitFor([&] { return router.reconnected == 1; }, 3000), "lost and connected again");
    check(router.lost == 1, "OnConnectionLost once");
    status = readState(*c, 1000);
    check(!status, "ReadState after reconnecting (" + hex(status) + ")");

    std::printf("-- router gone\n");
    fake.Stop();
    check(waitFor([&] { return router.lost == 2; }, 3000), "loss noticed");
    check(!c->IsConnected(), "IsConnected() false while down");
    auto t = Clock::now();
    status = readState(*c, 1000);
    long ms = msSince(t);
    check(status == ADSERR_CLIENT_SYNCTIMEOUT && ms >= 900 && ms < 1500,
          "request while down: 0x745 after its timeout (" + hex(status) + ", " + std::to_string(ms) + " ms)");

    std::printf("-- router back while a request waits\n");
    t = Clock::now();
    auto pending = std::async(std::launch::async, [&] { return readState(*c, 8000); });
    std::this_thread::sleep_for(milliseconds(1500));
    fake.Start();
    status = pending.get();
    ms = msSince(t);
    check(!status && ms < 5000, "waiting request goes through once connected (" + hex(status) + ", " +
                                    std::to_string(ms) + " ms)");
    check(router.reconnected == 2, "OnReconnected again");

    std::printf("-- silent peer\n");
    fake.SetSilent(true);
    status = readState(*c, 1000);
    check(status == ADSERR_CLIENT_SYNCTIMEOUT, "no answer: 0x745 (" + hex(status) + ")");
    check(c->IsConnected() && router.lost == 2, "silence alone is no loss");
    fake.SetSilent(false);
    status = readState(*c, 1000);
    check(!status, "answers again (" + hex(status) + ")");

    std::printf("-- connection dropped while a request is pending\n");
    fake.SetSilent(true);
    t = Clock::now();
    pending = std::async(std::launch::async, [&] { return readState(*c, 2000); });
    std::this_thread::sleep_for(milliseconds(300));
    fake.DropAll();
    fake.SetSilent(false);
    status = pending.get();
    ms = msSince(t);
    check(status == ADSERR_CLIENT_SYNCTIMEOUT && ms >= 1900,
          "pending request: 0x745 at its timeout (" + hex(status) + ", " + std::to_string(ms) + " ms)");
    check(waitFor([&] { return router.reconnected == 3; }, 3000), "connected again");
    status = readState(*c, 1000);
    check(!status, "ReadState afterwards (" + hex(status) + ")");

    std::printf("-- destroyed while connecting to a silent host\n");
    {
        std::lock_guard<std::mutex> lock(targetMutex);
        target = "192.0.2.1:48898"; // TEST-NET-1 never answers: connect() would take 21 s or more
    }
    fake.DropAll();
    check(waitFor([&] { return router.lost == 4; }, 3000), "loss noticed");
    std::this_thread::sleep_for(milliseconds(500)); // the first attempt runs now
    t = Clock::now();
    c.reset();
    ms = msSince(t);
    check(ms < 500, "destructor aborts the connect (" + std::to_string(ms) + " ms)");

    std::printf("-- destroyed while the router is gone\n");
    {
        TestRouter router4;
        auto c4 = std::unique_ptr<AmsConnection>(
            new AmsConnection{router4, [&](const std::atomic<bool>& abort) { return connectTo(host, abort); }});
        fake.Stop();
        check(waitFor([&] { return router4.lost == 1; }, 3000), "loss noticed");
        std::this_thread::sleep_for(milliseconds(300));
        t = Clock::now();
        c4.reset();
        ms = msSince(t);
        check(ms < 500, "destructor returns (" + std::to_string(ms) + " ms)");
        fake.Start();
    }

    std::printf("-- failing at once while down (local way, 0x507)\n");
    TestRouter router2;
    std::atomic<bool> giveUp{false};
    auto c2 = std::unique_ptr<AmsConnection>(new AmsConnection{
        router2,
        [&](const std::atomic<bool>& abort) {
            if (giveUp) throw AmsConnection::GiveUp("test");
            return connectTo(host, abort);
        },
        ROUTERERR_NOTREGISTERED});
    status = readState(*c2, 1000);
    check(!status, "ReadState (" + hex(status) + ")");
    fake.Stop();
    check(waitFor([&] { return router2.lost == 1; }, 3000), "loss noticed");
    t = Clock::now();
    status = readState(*c2, 1000);
    ms = msSince(t);
    check(status == ROUTERERR_NOTREGISTERED && ms < 100,
          "request while down: 0x507 at once (" + hex(status) + ", " + std::to_string(ms) + " ms)");
    fake.Start();
    check(waitFor([&] { return router2.reconnected == 1; }, 3000), "connected again");
    status = readState(*c2, 1000);
    check(!status, "ReadState afterwards (" + hex(status) + ")");

    std::printf("-- connector gives up\n");
    giveUp = true;
    fake.DropAll();
    check(waitFor([&] { return router2.gaveUp == 1; }, 3000), "OnGaveUp");
    check(!c2->IsConnected() && router2.reconnected == 1, "stays down");
    status = readState(*c2, 1000);
    check(status == ROUTERERR_NOTREGISTERED, "request afterwards: 0x507 (" + hex(status) + ")");
    t = Clock::now();
    c2.reset();
    check(msSince(t) < 500, "destructor returns");

    std::printf("-- destroyed while the peer never answers nor closes\n");
    {
        DeadPeer dead(static_cast<uint16_t>(port + 1));
        TestRouter router3;
        const std::string deadHost = "127.0.0.1:" + std::to_string(port + 1);
        auto c3 = std::unique_ptr<AmsConnection>(
            new AmsConnection{router3, [&](const std::atomic<bool>& abort) { return connectTo(deadHost, abort); }});
        std::this_thread::sleep_for(milliseconds(300));
        t = Clock::now();
        c3.reset();
        ms = msSince(t);
        check(ms < 1000, "destructor returns (" + std::to_string(ms) + " ms)");
    }

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
