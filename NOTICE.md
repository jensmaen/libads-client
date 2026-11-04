# Notice

libads-client is MIT licensed (`LICENSE`). Parts of it are taken from **Beckhoff ADS**,
https://github.com/Beckhoff/ADS, commit `24e598a113edfdc0d34414bfefb847930d94cb87` (2026-09-17), also MIT licensed;
its license is in `LICENSE.Beckhoff`. Every taken file keeps its SPDX header and Beckhoff's copyright line.

This project is not affiliated with or endorsed by Beckhoff Automation. Beckhoff, TwinCAT and ADS are trademarks of
Beckhoff Automation GmbH & Co. KG.

## Files taken from Beckhoff ADS

Paths on the left in `AdsLib/` of Beckhoff ADS, on the right in `src/`.

| Beckhoff ADS | libads-client |
|---|---|
| `AdsDef.h`, `AdsDef.cpp`, `AmsHeader.h`, `Frame.*`, `Log.*`, `wrap_endian.h`, `wrap_socket.h` | `core/` (same names) |
| `standalone/AmsNetId.cpp` | `core/AmsNetId.cpp` |
| `standalone/AdsDef.h` | `core/StandaloneAdsDef.h` |
| `Sockets.*`, `AmsConnection.h`, `Router.h`, `NotificationDispatcher.h`, `AdsNotification.h`, `RingBuffer.h`, `Semaphore.h` | `connection/` (same names) |
| `standalone/AmsConnection.cpp`, `standalone/NotificationDispatcher.cpp` | `connection/` (same names) |
| `AmsPort.h`, `AmsRouter.h`, `standalone/AmsPort.cpp`, `standalone/AmsRouter.cpp` | `route/` (same names) |
| `AdsLib.cpp` (`AddRemoteRoute`, `GetRemoteAddress`, `AdsAddRoute`, `AdsDelRoute`, `AdsSetLocalAddress`) | `route/AdsLib.cpp` |
| `AdsLib.h`, `AdsDevice.*`, `AdsVariable.h`, `AdsNotificationOOI.h`, `AdsException.h` | `client/` (same names) |
| `standalone/AdsLib.h`, `standalone/AdsLib.cpp` | `client/StandaloneAdsLib.h`, `client/StandaloneAdsLib.cpp` |

Not taken: `TwinCAT/*`, `bhf/*`, the special accesses (`SymbolAccess`, `RegistryAccess`, `RouterAccess`, …),
Beckhoff's build files, tests and tools.

## Main changes to those files

- The switch to Beckhoff's `TcAdsDll` (`USE_TWINCAT_ROUTER`) is gone; the library always talks TCP itself.
- **Local way:** `AdsPortOpenEx()` opens a port at the local TwinCAT router (127.0.0.1:48898), as with the
  `TcAdsDll`; the route way has its own `AdsPortOpenRouteEx()`. Every C function is forwarded to the part that owns
  the port. `AmsTcpHeader` carries the router command, `Router` got hooks for router commands, lost and restored
  connections.
- **TcAdsDll compatibility:** types `ads_i32`/`ads_ui32` …, `TCADSDLL_API`, the notification header in TwinCAT order,
  callbacks as in the DLL, router events, the DLL's 23 functions; wrappers set `AdsGetLastError()`.
- **Routes:** `AddRemoteRoute()` and `GetRemoteAddress()` return error codes instead of throwing.
- **Secure ADS:** `AmsConnection` holds an exchangeable transport (TCP or TLS) instead of a socket; new functions
  `AddRemoteRouteSecure()`, `AddLocalRouteSecure()` and their C forms.
- **Reconnecting (route way):** `AmsConnection` can reconnect through a connector; requests during an outage wait
  until their deadline; `Sockets` connect and wait in slices so that they can be cancelled. Notifications are kept
  with own handles and created again on the target after reconnecting.
- A second request on the same port waits for the free slot instead of failing at once.
- The parts were split into `local/` and `route/`, each of which can be switched off; switched-off functions return
  `ADSERR_DEVICE_SRVNOTSUPP`.
- Small fixes: missing `<stdexcept>` in `AdsDevice.cpp`; `AmsConnection.h` includes only what it needs.

## Own code

Copyright Jens Fresenborg, MIT: `connection/AdsBackend.h`, `connection/AmsTransport.*`, `connection/Subscriptions.*`,
`local/LocalRouter.*`, `local/TcAdsDef.h`, `local/TcAdsAPI.h` (written from the public documentation, not copied from
a TwinCAT installation), `route/TlsSettings.h`, `route/TlsTransport.*`, the CMake files, tests and examples.

## Third-party software

**Mbed TLS** 3.6 (Apache-2.0, https://github.com/Mbed-TLS/mbedtls), used for Secure ADS only. It is not part of this
repository: CMake takes an installed copy or downloads the release named in `cmake/FetchMbedTLS.cmake`. As the library
is static, Mbed TLS ends up in your program; include its license when you pass the program on.
