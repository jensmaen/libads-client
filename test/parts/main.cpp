// The library as built: the route way opens its ports through the C-API of libads; without Secure ADS its functions
// stay and fail with ADSERR_DEVICE_SRVNOTSUPP. Needs neither TwinCAT nor a network, so it runs in CTest.
// The local router is not tried here: without TwinCAT it cannot open a port.

#include "AdsLib.h"

// Set by CMake from LIBADS_CLIENT_SECURE; without CMake off unless defined.
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
    std::printf("parts: secure %d\n", LIBADS_CLIENT_TEST_SECURE);

    const long route = AdsPortOpenRouteEx();
    check(route != 0, "route way: AdsPortOpenRouteEx opens a port", route);
    if (route) {
        AmsAddr addr{};
        const long status = AdsGetLocalAddressEx(route, &addr);
        check(status == 0, "the C-API of libads knows the route port", status);
        check(AdsPortCloseEx(route) == 0, "AdsPortCloseEx", 0);
    }

#if !LIBADS_CLIENT_TEST_SECURE
    // Rejected by the router before any lookup.
    long s = AdsAddRouteSecure(target, "127.0.0.1", ".");
    check(s == ADSERR_DEVICE_SRVNOTSUPP, "Secure ADS off: AdsAddRouteSecure", s);
    s = AdsAddRoutePsk(target, "127.0.0.1", "identity", "password");
    check(s == ADSERR_DEVICE_SRVNOTSUPP, "Secure ADS off: AdsAddRoutePsk", s);
#endif

    std::printf("%s, %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
