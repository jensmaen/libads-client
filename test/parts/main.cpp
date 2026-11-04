// The parts of the library as built: a part that is off keeps its functions, they fail with
// ADSERR_DEVICE_SRVNOTSUPP (port functions return 0). Needs neither TwinCAT nor a network, so it runs in CTest.
// The local part that is on is not tried here: without TwinCAT it cannot open a port.

#include "AdsLib.h"

// Set by CMake from the options; without CMake: both parts, Secure ADS off unless defined.
#ifndef LIBADS_CLIENT_TEST_LOCAL
#define LIBADS_CLIENT_TEST_LOCAL 1
#endif
#ifndef LIBADS_CLIENT_TEST_ROUTE
#define LIBADS_CLIENT_TEST_ROUTE 1
#endif
#ifndef LIBADS_CLIENT_TEST_SECURE
#define LIBADS_CLIENT_TEST_SECURE 0
#endif

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char* what, long value) {
    std::printf("%s %s (0x%lX)\n", ok ? "[ ok ]" : "[FAIL]", what, value);
    if (!ok) ++failures;
}

const AmsNetId target{192, 168, 0, 99, 1, 1};

} // namespace

int main() {
    std::printf("parts: local %d, route %d, secure %d\n", LIBADS_CLIENT_TEST_LOCAL, LIBADS_CLIENT_TEST_ROUTE,
                LIBADS_CLIENT_TEST_SECURE);

#if !LIBADS_CLIENT_TEST_LOCAL
    const long local = AdsPortOpenEx();
    check(local == 0, "local part off: AdsPortOpenEx returns 0", local);
#endif

#if LIBADS_CLIENT_TEST_ROUTE
    const long route = AdsPortOpenRouteEx();
    check(route != 0, "route part on: AdsPortOpenRouteEx opens a port", route);
    if (route) check(AdsPortCloseEx(route) == 0, "AdsPortCloseEx", 0);
#else
    long v = AdsPortOpenRouteEx();
    check(v == 0, "route part off: AdsPortOpenRouteEx returns 0", v);
    v = AdsAddRoute(target, "127.0.0.1");
    check(v == ADSERR_DEVICE_SRVNOTSUPP, "route part off: AdsAddRoute", v);
    AmsNetId remote{};
    v = bhf::ads::GetRemoteAddress("127.0.0.1", remote);
    check(v == ADSERR_DEVICE_SRVNOTSUPP, "route part off: GetRemoteAddress", v);
    v = bhf::ads::AddRemoteRoute("127.0.0.1", target, "192.0.2.1", "route", "user", "password");
    check(v == ADSERR_DEVICE_SRVNOTSUPP, "route part off: AddRemoteRoute", v);
    AdsDelRoute(target);
#endif

#if !LIBADS_CLIENT_TEST_SECURE
    // Without the route part through the stubs, with it through the router (rejected before any lookup).
    long s = AdsAddRouteSecure(target, "127.0.0.1", ".");
    check(s == ADSERR_DEVICE_SRVNOTSUPP, "Secure ADS off: AdsAddRouteSecure", s);
    s = AdsAddRoutePsk(target, "127.0.0.1", "identity", "password");
    check(s == ADSERR_DEVICE_SRVNOTSUPP, "Secure ADS off: AdsAddRoutePsk", s);
#endif

    std::printf("%s, %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
