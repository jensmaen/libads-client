# Notice

libads-client is MIT licensed (`LICENSE`). Parts of it are taken from **Beckhoff ADS**,
https://github.com/Beckhoff/ADS, commit `24e598a113edfdc0d34414bfefb847930d94cb87` (2026-09-17), also MIT licensed;
its license is in `LICENSE.Beckhoff`. Every taken file keeps its SPDX header and Beckhoff's copyright line.
libads-client is built on **libads** (https://gitlab.com/fresenborg-development/libads), which has its own notice.

This project is not affiliated with or endorsed by Beckhoff Automation. Beckhoff, TwinCAT and ADS are trademarks of
Beckhoff Automation GmbH & Co. KG.

## Files taken from Beckhoff ADS

Paths on the left in `AdsLib/` of Beckhoff ADS, on the right in `src/`.

| Beckhoff ADS | libads-client |
|---|---|
| `AmsPort.h`, `AmsRouter.h`, `standalone/AmsPort.cpp`, `standalone/AmsRouter.cpp` | `route/` (same names) |
| `AdsLib.cpp` (`AddRemoteRoute`, `GetRemoteAddress`, `AdsAddRoute`, `AdsDelRoute`, `AdsSetLocalAddress`), the route functions of `standalone/AdsLib.cpp` | `route/AdsLib.cpp` |
| `AdsLib.h` (without the port functions, which are in libads), `AdsDevice.*`, `AdsVariable.h`, `AdsNotificationOOI.h`, `AdsException.h` | `client/` (same names) |

Not taken: `TwinCAT/*`, `bhf/*`, the special accesses (`SymbolAccess`, `RegistryAccess`, `RouterAccess`, …),
Beckhoff's build files, tests and tools.

## Main changes to those files

- **Route way beside the local one:** the route way has its own `AdsPortOpenRouteEx()`; its router makes itself
  known to the C-API of libads on first use, which then forwards the calls for its ports.
- **Routes:** `AddRemoteRoute()` and `GetRemoteAddress()` return error codes instead of throwing.
- **Secure ADS:** routes over TLS (TCP 8016); new functions `AddRemoteRouteSecure()`, `AddLocalRouteSecure()` and
  their C forms.
- **Reconnecting:** every route reconnects after a loss, every second, until it is deleted; notifications are
  created again on the target after reconnecting.
- `AdsDevice` has two more constructors: through the local router, and over a route the program added before
  (`AdsRouteAdded`).
- `AdsNotification` releases its symbol handle right after creating the notification.
- Small fixes: missing `<stdexcept>` in `AdsDevice.cpp`.

## Own code

Copyright Jens Fresenborg, MIT: `client/AdsSymbols.*`, `route/TlsSettings.h`, `route/TlsTransport.*`, the CMake
files, tests and examples. `test/fake-router` and `test/subscriptions-fake` are copies of the same files in libads.

## Third-party software

**Mbed TLS** 3.6 (Apache-2.0, https://github.com/Mbed-TLS/mbedtls), used for Secure ADS only. It is not part of this
repository: CMake takes an installed copy or downloads the release named in `cmake/FetchMbedTLS.cmake`. As the library
is static, Mbed TLS ends up in your program; include its license when you pass the program on.
