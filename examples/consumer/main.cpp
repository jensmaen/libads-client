// Opens a port through the local TwinCAT router and one for the route way, as code written for the TcAdsDll would.
// Without TwinCAT the local port fails cleanly; the program only shows that headers and library fit together.

#include "TcAdsAPI.h"

#include <cstdio>

int main() {
    const long local = AdsPortOpenEx();
    AmsAddr addr{};
    if (local) {
        const long status = AdsGetLocalAddressEx(local, &addr);
        std::printf("local port %ld, status 0x%lX, AmsNetId %u.%u.%u.%u.%u.%u\n", local, status, addr.netId.b[0],
                    addr.netId.b[1], addr.netId.b[2], addr.netId.b[3], addr.netId.b[4], addr.netId.b[5]);
        AdsPortCloseEx(local);
    } else {
        std::printf("local port: none (no TwinCAT router, or built without the local part)\n");
    }

    const long route = AdsPortOpenRouteEx();
    std::printf("route port %ld\n", route);
    if (route) AdsPortCloseEx(route);

    // Secure ADS: only linked in, not called (needs a target). Returns ADSERR_DEVICE_SRVNOTSUPP without Secure ADS.
    long (*secure)(AmsNetId, const char*, const char*) = AdsAddRouteSecure;
    std::printf("Secure ADS function at %p\n", reinterpret_cast<void*>(secure));
    return 0;
}
