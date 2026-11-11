// Notifications with own handles, against the fake router in the same process, no TwinCAT needed:
// LocalRouter (libads) and AmsRouter (libads-client) share Subscriptions.
// Usage: subscriptions-fake [port]   (default 48920)

#include "FakeRouter.h"

// Which part runs: the local one with libads, the route one with libads-client; set by CMake, without CMake both.
#ifndef LIBADS_TEST_LOCAL
#define LIBADS_TEST_LOCAL 1
#endif
#ifndef LIBADS_TEST_ROUTE
#define LIBADS_TEST_ROUTE 1
#endif

#if LIBADS_TEST_LOCAL
#include "LocalRouter.h"
#endif
#if LIBADS_TEST_ROUTE
#include "AmsRouter.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

bool waitFor(const std::function<bool()>& done, int ms) {
    const auto end = Clock::now() + milliseconds(ms);
    while (!done()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(milliseconds(10));
    }
    return true;
}

const AmsNetId fakeNetId{192, 168, 0, 99, 1, 1};

// Samples received, per handle the callback saw
std::mutex seenMutex;
std::map<uint32_t, int> seen;

void onNotification(const AmsAddr*, const AdsNotificationHeader* header, uint32_t) {
    std::lock_guard<std::mutex> lock(seenMutex);
    ++seen[header->hNotification];
}

int Seen(uint32_t handle) {
    std::lock_guard<std::mutex> lock(seenMutex);
    const auto it = seen.find(handle);
    return it == seen.end() ? 0 : it->second;
}

/** Waits until handle gets at least `more` samples beyond the current count */
bool GetsSamples(uint32_t handle, int more = 3) {
    const int before = Seen(handle);
    return waitFor([&] { return Seen(handle) >= before + more; }, 2000);
}

/** Adds a notification on MAIN.nCounter of the fake router (or on another index group) like the C-API does */
long Add(AdsBackend& backend, uint16_t port, uint32_t* handle, uint32_t group = FakeRouter::COUNTER_GROUP) {
    const AmsAddr target{fakeNetId, 851};
    uint8_t buffer[sizeof(*handle)];
    AmsRequest request{target, port, AoEHeader::ADD_DEVICE_NOTIFICATION, sizeof(buffer), buffer, nullptr,
                       sizeof(AdsAddDeviceNotificationRequest)};
    request.frame.prepend(AdsAddDeviceNotificationRequest{group, FakeRouter::COUNTER_OFFSET, 4, ADSTRANS_SERVERCYCLE,
                                                          0, 100000 /* 10 ms */});
    auto notify = std::make_shared<Notification>(NotificationCallback{&onNotification}, 0, 4, target, port);
    return backend.AddNotification(request, handle, notify);
}

long Del(AdsBackend& backend, uint16_t port, uint32_t handle) {
    const AmsAddr target{fakeNetId, 851};
    return backend.DelNotification(port, &target, handle);
}

long Elapsed(Clock::time_point start) {
    return static_cast<long>(std::chrono::duration_cast<milliseconds>(Clock::now() - start).count());
}

/**
 * A target that stops answering, unnoticed by the connection: only the first delete waits for its timeout, the
 * others and closing the port forget their notifications at once. Once the target sends again, deletes reach it.
 */
void Silent(FakeRouter& fake, AdsBackend& backend, uint16_t port) {
    const uint32_t timeout = 300;
    backend.SetTimeout(port, timeout);

    uint32_t handles[4] = {};
    for (auto& h : handles) Add(backend, port, &h);
    fake.SetSilent(true);
    auto start = Clock::now();
    long first = Del(backend, port, handles[0]);
    long rest = 0;
    for (int i = 1; i < 4; ++i) rest |= Del(backend, port, handles[i]);
    long ms = Elapsed(start);
    check(first == ADSERR_CLIENT_SYNCTIMEOUT && rest == ADSERR_CLIENT_SYNCTIMEOUT,
          "silent target: 4 deletes " + hex(first) + "/" + hex(rest));
    check(ms < 2 * static_cast<long>(timeout), "silent target: 4 deletes took one timeout (" + std::to_string(ms) + " ms)");

    fake.SetSilent(false); // its forgotten notifications send samples again
    uint32_t h = 0;
    long status = Add(backend, port, &h);
    check(!status, "target answers again: AddNotification " + hex(status));
    const auto count = fake.Notifications();
    status = Del(backend, port, h);
    check(!status && fake.Notifications() == count - 1, "target answers again: delete reaches it " + hex(status));

    uint32_t more[5] = {};
    for (auto& m : more) Add(backend, port, &m);
    fake.SetSilent(true);
    start = Clock::now();
    status = Del(backend, port, more[0]);
    status = backend.ClosePort(port);
    ms = Elapsed(start);
    check(!status, "silent target: ClosePort " + hex(status));
    check(ms < 2 * static_cast<long>(timeout),
          "silent target: delete and ClosePort with 4 notifications took one timeout (" + std::to_string(ms) + " ms)");
    fake.SetSilent(false);
}

#if LIBADS_TEST_LOCAL
void Local(FakeRouter& fake, const std::string& host) {
    std::printf("-- local part (LocalRouter)\n");
    LocalRouter router(host);
    const auto port = router.OpenPort();
    check(port != 0, "OpenPort");

    uint32_t h1 = 0, h2 = 0;
    long status = Add(router, port, &h1);
    check(!status, "AddNotification " + hex(status) + ", handle " + std::to_string(h1));
    status = Add(router, port, &h2);
    check(!status && h2 != h1, "second AddNotification " + hex(status) + ", handle " + std::to_string(h2));
    check(fake.Notifications() == 2, "fake router has 2 notifications");
    check(GetsSamples(h1) && GetsSamples(h2), "callbacks see our handles");

    // TwinCAT restarted behind the connection: delete and create again
    const auto requests = fake.Requests();
    fake.SendRouterEvent(1);
    check(waitFor([&] { return fake.Requests() >= requests + 4; }, 3000),
          "router 'started': both deleted and created again (" + std::to_string(fake.Requests() - requests) +
              " requests)");
    check(fake.Notifications() == 2, "fake router still has 2 notifications");
    check(GetsSamples(h1) && GetsSamples(h2), "callbacks still see the same handles");

    status = Del(router, port, h1);
    check(!status, "DelNotification " + hex(status));
    check(fake.Notifications() == 1, "fake router has 1 notification");
    status = Del(router, port, h1);
    check(status == ADSERR_CLIENT_REMOVEHASH, "deleting it again: ADSERR_CLIENT_REMOVEHASH " + hex(status));

    uint32_t bad = 0;
    status = Add(router, port, &bad, 0x1234);
    check(status == 0x702, "AddNotification on an unknown index group: target's error " + hex(status));
    const auto quiet = fake.Requests();
    std::this_thread::sleep_for(milliseconds(1500));
    check(fake.Requests() == quiet, "failed notification is not created again");

    status = router.ClosePort(port);
    check(!status, "ClosePort " + hex(status));
    check(waitFor([&] { return fake.Notifications() == 0; }, 2000), "ClosePort deleted the rest on the target");

    // connection lost for good (local part, as with the TcAdsDll): handles stay, requests fail
    const auto port2 = router.OpenPort();
    uint32_t h3 = 0;
    status = Add(router, port2, &h3);
    check(!status, "AddNotification on a new port " + hex(status));
    fake.DropAll();
    check(waitFor([&] { return !router.IsConnected(port2); }, 3000), "connection to the router lost");
    std::this_thread::sleep_for(milliseconds(1500)); // worker releases the connection
    uint32_t h4 = 0;
    status = Add(router, port2, &h4);
    check(status == ROUTERERR_NOTREGISTERED, "AddNotification after the loss: ROUTERERR_NOTREGISTERED " + hex(status));
    status = Del(router, port2, h3);
    check(!status, "DelNotification after the loss " + hex(status));
    status = router.ClosePort(port2);
    check(!status, "ClosePort after the loss " + hex(status));

    Silent(fake, router, router.OpenPort());
}
#endif

#if LIBADS_TEST_ROUTE
void Route(FakeRouter& fake, const std::string& host) {
    std::printf("-- route part (AmsRouter)\n");
    AmsRouter router(AmsNetId{192, 168, 0, 50, 1, 1});
    const auto port = router.OpenPort();
    check(port != 0, "OpenPort");

    uint32_t h = 0;
    long status = Add(router, port, &h);
    check(status == GLOBALERR_MISSING_ROUTE, "AddNotification without a route: GLOBALERR_MISSING_ROUTE " + hex(status));

    status = router.AddRoute(fakeNetId, host);
    check(!status, "AddRoute " + hex(status));
    const auto before = fake.Notifications();
    status = Add(router, port, &h);
    check(!status, "AddNotification " + hex(status) + ", handle " + std::to_string(h));
    check(fake.Notifications() == before + 1, "fake router has the notification");
    check(GetsSamples(h), "callback sees our handle");

    status = Del(router, port, h);
    check(!status, "DelNotification " + hex(status));
    check(fake.Notifications() == before, "deleted on the fake router");
    status = Del(router, port, h);
    check(status == ADSERR_CLIENT_REMOVEHASH, "deleting it again: ADSERR_CLIENT_REMOVEHASH " + hex(status));

    uint32_t h2 = 0;
    status = Add(router, port, &h2);
    check(!status, "AddNotification again " + hex(status));
    router.DelRoute(fakeNetId); // destroys the connection under the notification
    check(true, "DelRoute with an active notification");
    status = Del(router, port, h2);
    check(!status, "DelNotification after DelRoute " + hex(status));

    uint32_t h3 = 0;
    status = router.AddRoute(fakeNetId, host);
    check(!status, "AddRoute again " + hex(status));
    status = Add(router, port, &h3);
    check(!status, "AddNotification " + hex(status));

    // connection lost: the route reconnects and creates the notification again, same handle
    fake.DropAll();
    check(waitFor([&] { return fake.Connections() == 0 || fake.Notifications() == before; }, 3000),
          "fake router dropped the connection");
    check(waitFor([&] { return fake.Notifications() == before + 1; }, 5000),
          "reconnected, notification created again on the target");
    check(GetsSamples(h3), "callback sees the same handle after reconnecting");
    // a request after reconnecting
    uint32_t value = 0, read = 0;
    const AmsAddr plc{fakeNetId, 851}; // AmsRequest keeps a reference to it
    AmsRequest request{plc, port, AoEHeader::READ, sizeof(value), &value, &read,
                       sizeof(AoERequestHeader)};
    request.frame.prepend(AoERequestHeader{FakeRouter::COUNTER_GROUP, FakeRouter::COUNTER_OFFSET, sizeof(value)});
    status = router.AdsRequest(request);
    check(!status && read == sizeof(value), "read after reconnecting " + hex(status));
    status = router.ClosePort(port);
    check(!status, "ClosePort " + hex(status));
    check(waitFor([&] { return fake.Notifications() == before; }, 2000), "ClosePort deleted it on the target");

    Silent(fake, router, router.OpenPort());
    router.DelRoute(fakeNetId);
}
#endif

} // namespace

int main(int argc, char** argv) {
    FakeRouter::Options options;
    options.listenPort = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 48920);
    FakeRouter fake(options);
    if (!fake.Start()) { // e.g. still taken by a run that just ended (TIME_WAIT on Windows, shared under WSL)
        std::printf("cannot listen on 127.0.0.1:%u\n", options.listenPort);
        return 1;
    }
    const auto host = "127.0.0.1:" + std::to_string(options.listenPort);
    fake.SetAdsState(5);
#if LIBADS_TEST_LOCAL
    Local(fake, host);
#endif
#if LIBADS_TEST_ROUTE
    fake.Restart();
    Route(fake, host);
#endif
    fake.Stop(false);
    std::printf("%s, %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
