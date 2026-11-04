// Route way reconnecting through the C-API against the fake router in the same process, no
// TwinCAT needed: router gone, silent, restarted, target not ready, requests waiting across the outage, deleting
// the route or closing the port during the outage.
// Usage: route-reconnect-fake [port]   (default 48930)

#include "AdsLib.h"

#include "../fake-router/FakeRouter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
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

const AmsNetId fakeNetId{192, 168, 0, 99, 1, 1};
const AmsAddr plc{fakeNetId, 851};

std::atomic<int> samples{0};
std::atomic<uint32_t> lastHandle{0};

void onCounter(const AmsAddr*, const AdsNotificationHeader* header, uint32_t) {
    lastHandle = header->hNotification;
    ++samples;
}

bool SamplesFlow(int more = 3, int ms = 2000) {
    const int before = samples;
    return waitFor([&] { return samples >= before + more; }, ms);
}

// The library notices a lost connection within about 100 ms (Select() in slices); a request sent before that goes
// out on the old connection and runs into its timeout, as with the TcAdsDll.
void LetLossBeNoticed() { std::this_thread::sleep_for(milliseconds(300)); }

bool SamplesStopped() {
    std::this_thread::sleep_for(milliseconds(200));
    const int before = samples;
    std::this_thread::sleep_for(milliseconds(300));
    return samples == before;
}

long Read(long port, uint32_t* value = nullptr) {
    uint32_t v = 0, n = 0;
    const long status = AdsSyncReadReqEx2(port, &plc, FakeRouter::COUNTER_GROUP, FakeRouter::COUNTER_OFFSET,
                                          sizeof(v), &v, &n);
    if (value) *value = v;
    return status;
}

/** Read that fails after its timeout (not at once) with ADSERR_CLIENT_SYNCTIMEOUT, like the TcAdsDll */
void ExpectTimeout(long port, const std::string& when) {
    const auto start = Clock::now();
    const long status = Read(port);
    const long ms = msSince(start);
    check(status == ADSERR_CLIENT_SYNCTIMEOUT && ms >= 900 && ms < 2500,
          "read " + when + ": " + hex(status) + " after " + std::to_string(ms) + " ms (0x745 after 1000 ms)");
}

void ExpectRead(long port, const std::string& when, int ms = 3000) {
    long status = -1;
    const auto start = Clock::now();
    waitFor([&] { return (status = Read(port)) == 0; }, ms);
    check(!status, "read " + when + ": " + hex(status) + " after " + std::to_string(msSince(start)) + " ms");
}

} // namespace

int main(int argc, char** argv) {
    FakeRouter::Options options;
    options.listenPort = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 48930);
    FakeRouter fake(options);
    if (!fake.Start()) { // e.g. still taken by a run that just ended (TIME_WAIT on Windows, shared under WSL)
        std::printf("cannot listen on 127.0.0.1:%u\n", options.listenPort);
        return 1;
    }
    fake.SetAdsState(5);
    const auto host = "127.0.0.1:" + std::to_string(options.listenPort);

    AdsSetLocalAddress(AmsNetId{192, 168, 0, 50, 1, 1});
    long status = AdsAddRoute(fakeNetId, host.c_str());
    check(!status, "AdsAddRoute " + hex(status));
    const long port = AdsPortOpenRouteEx();
    check(port != 0, "AdsPortOpenRouteEx");
    AdsSyncSetTimeoutEx(port, 1000);
    ExpectRead(port, "at the start");

    AdsNotificationAttrib attrib{};
    attrib.cbLength = 4;
    attrib.nTransMode = ADSTRANS_SERVERCYCLE;
    attrib.nCycleTime = 100000; // 10 ms
    uint32_t handle = 0;
    status = AdsSyncAddDeviceNotificationReqEx(port, &plc, FakeRouter::COUNTER_GROUP, FakeRouter::COUNTER_OFFSET,
                                               &attrib, onCounter, 0, &handle);
    check(!status, "AddDeviceNotification " + hex(status) + ", handle " + std::to_string(handle));
    check(SamplesFlow() && lastHandle == handle, "notifications arrive with our handle");

    // one request per port at a time: a second thread waits for the slot instead of failing (as notifications
    // are created again on the program's port while it may send itself)
    {
        std::atomic<int> bad{0};
        auto reader = [&] {
            for (int i = 0; i < 200; ++i) {
                if (Read(port)) ++bad;
            }
        };
        std::thread a(reader), b(reader);
        a.join();
        b.join();
        check(bad == 0, "two threads reading over the same port at once: " + std::to_string(bad) + " of 400 failed");
    }

    std::printf("-- router gone (not listening)\n");
    fake.Stop(false);
    check(SamplesStopped(), "notifications stop");
    ExpectTimeout(port, "while the router is gone");
    ExpectTimeout(port, "again while the router is gone");
    fake.Start();
    ExpectRead(port, "after the router is back");
    check(SamplesFlow() && lastHandle == handle, "notifications arrive again, same handle");
    check(fake.Notifications() == 1, "fake router has exactly 1 notification");

    std::printf("-- request waiting across the outage\n");
    fake.Stop(false);
    LetLossBeNoticed();
    AdsSyncSetTimeoutEx(port, 5000);
    const auto start = Clock::now();
    auto pending = std::async(std::launch::async, [&] { return Read(port); });
    std::this_thread::sleep_for(milliseconds(1500));
    fake.Start();
    status = pending.get();
    const long waited = msSince(start);
    check(!status && waited >= 1400 && waited < 5000,
          "read started during the outage succeeds after it: " + hex(status) + " after " + std::to_string(waited) +
              " ms");
    AdsSyncSetTimeoutEx(port, 1000);
    check(SamplesFlow() && lastHandle == handle, "notifications arrive again, same handle");

    std::printf("-- router silent (connection stays, nothing answered)\n");
    const auto connections = fake.Connections();
    fake.SetSilent(true);
    check(SamplesStopped(), "notifications stop");
    ExpectTimeout(port, "while the router is silent");
    fake.SetSilent(false);
    ExpectRead(port, "after the router speaks again");
    check(SamplesFlow() && lastHandle == handle, "notifications arrive again, same handle");
    check(fake.Connections() == connections, "still the same connection");

    std::printf("-- router restarted (forgets ports and notifications)\n");
    fake.Restart(false);
    ExpectRead(port, "after the restart");
    check(waitFor([&] { return fake.Notifications() == 1; }, 3000), "notification created again on the restarted router");
    check(SamplesFlow() && lastHandle == handle, "notifications arrive again, same handle");

    std::printf("-- target not ready after reconnecting (PLC stopped)\n");
    fake.SetAdsState(6);
    fake.DropAll();
    std::this_thread::sleep_for(milliseconds(2500));
    check(fake.Notifications() == 0, "notification not created while the PLC answers 0x6");
    check(Read(port) == 0x6, "read gets the target's error 0x6");
    fake.SetAdsState(5);
    check(waitFor([&] { return fake.Notifications() == 1; }, 3000), "created once the PLC runs (retried every second)");
    check(SamplesFlow() && lastHandle == handle, "notifications arrive again, same handle");

    std::printf("-- deleting during the outage\n");
    fake.Stop(false);
    LetLossBeNoticed();
    auto t = Clock::now();
    status = AdsSyncDelDeviceNotificationReqEx(port, &plc, handle);
    check(!status && msSince(t) < 500,
          "DelDeviceNotification during the outage " + hex(status) + " in " + std::to_string(msSince(t)) + " ms");
    t = Clock::now();
    AdsDelRoute(fakeNetId);
    check(msSince(t) < 1500, "AdsDelRoute during the outage returns in " + std::to_string(msSince(t)) + " ms");
    fake.Start();
    std::this_thread::sleep_for(milliseconds(1500));
    check(fake.Connections() == 0, "no reconnecting after AdsDelRoute");
    status = Read(port);
    check(status == GLOBALERR_MISSING_ROUTE, "read without the route: GLOBALERR_MISSING_ROUTE " + hex(status));

    std::printf("-- closing the port during the outage\n");
    status = AdsAddRoute(fakeNetId, host.c_str());
    check(!status, "AdsAddRoute again " + hex(status));
    status = AdsSyncAddDeviceNotificationReqEx(port, &plc, FakeRouter::COUNTER_GROUP, FakeRouter::COUNTER_OFFSET,
                                               &attrib, onCounter, 0, &handle);
    check(!status, "AddDeviceNotification " + hex(status));
    fake.Stop(false);
    LetLossBeNoticed();
    t = Clock::now();
    status = AdsPortCloseEx(port);
    check(!status && msSince(t) < 500, "AdsPortCloseEx during the outage " + hex(status) + " in " +
                                           std::to_string(msSince(t)) + " ms");
    fake.Start();
    check(waitFor([&] { return fake.Connections() == 1; }, 3000), "the route reconnects without a port");
    std::this_thread::sleep_for(milliseconds(1500));
    check(fake.Notifications() == 0, "the closed port's notification is not created again");
    t = Clock::now();
    AdsDelRoute(fakeNetId);
    check(msSince(t) < 500, "AdsDelRoute in " + std::to_string(msSince(t)) + " ms");

    fake.Stop(false);
    std::printf("%s, %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
