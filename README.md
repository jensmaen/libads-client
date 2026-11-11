# libads-client for TwinCAT

A lean C++ ADS client library for Windows and Linux. Programs use it to talk to TwinCAT systems **without the
`TcAdsDll`** or any other Beckhoff component, neither for building nor for passing the program on.

It speaks TCP itself, in two ways:

- **Local way**, the replacement for the `TcAdsDll`: through the TwinCAT router on the same computer
  (127.0.0.1:48898). Reaches everything the router has a route to. Code written for the `TcAdsDll` builds unchanged:
  the headers `TcAdsAPI.h` and `TcAdsDef.h` carry the DLL's names and types. This part is its own library,
  [libads](https://gitlab.com/fresenborg-development/libads); libads-client builds on it and brings it along.
- **Route way**, without TwinCAT on the own computer: directly to the target over an ADS route, like the standalone
  part of [Beckhoff ADS](https://github.com/Beckhoff/ADS). Adds routes on the target itself, speaks
  **Secure ADS** (TLS, self-signed certificates, pre-shared key or own CA) and **reconnects** by itself after the
  target or the network was gone.

On top of the C functions sit the C++ classes of Beckhoff ADS (`AdsDevice`, `AdsVariable`, `AdsNotification`;
the latter releases its symbol handle right after creating the notification).

Status: 0.3, the API may change until 1.0. Tested with TwinCAT 3.1.4026 on Windows 11 (XAR in kernel mode) and
TwinCAT for Linux; compilers clang-cl (MSVC ABI), GCC 14 and llvm-mingw.

## Which way to take

| | Local way | Route way |
|---|---|---|
| Open a port | `AdsPortOpenEx()` | `AdsPortOpenRouteEx()` |
| Needs on the own computer | TwinCAT (router) | nothing |
| Route | the TwinCAT router's routes | own route per target (`AdsAddRoute()`), target needs a route back |
| Secure ADS | the router's business | built in (TCP 8016) |
| Connection lost | as with the `TcAdsDll` (see below) | reconnects every second, notifications are created again |

Both ways can be used in the same program; the C functions take ports of both. A program that needs only the local
way from C can use libads alone.

## Using it in a CMake project

Requires CMake 3.25 and a C++14 compiler. The library is static. It needs libads in exactly its own version (it
uses inner headers of libads): a libads target the project added before, an installed package, or else it downloads
the matching tag.

**FetchContent** (simplest):

```cmake
include(FetchContent)
FetchContent_Declare(libads-client
    GIT_REPOSITORY https://gitlab.com/fresenborg-development/libads-client.git
    GIT_TAG v0.3.0)
FetchContent_MakeAvailable(libads-client)
target_link_libraries(myprogram PRIVATE libads-client::ads-client)
```

**Installed package:**

```sh
cmake -S libads-client -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/install
```

```cmake
find_package(libads-client 0.3 REQUIRED COMPONENTS secure)   # the component is optional
target_link_libraries(myprogram PRIVATE libads-client::ads-client)
```

With libads installed into the same prefix (otherwise libads is downloaded and installed along).

**Source copy or Git submodule:** `add_subdirectory(libads-client)` and the same `target_link_libraries`.

As part of another project (FetchContent, `add_subdirectory`), the library builds neither its tests nor install rules.
[`examples/consumer`](examples/consumer) shows all three ways.

### Options

| Option | Default | Meaning |
|---|---|---|
| `LIBADS_CLIENT_SECURE` | `ON` | Secure ADS in the route way, needs Mbed TLS 3.6 |
| `LIBADS_CLIENT_FETCH_MBEDTLS` | `ON` | download Mbed TLS if it is not installed |
| `LIBADS_CLIENT_FETCH_LIBADS` | `ON` | download libads if it is not found |
| `LIBADS_CLIENT_BUILD_TESTS` | top level only | test programs |
| `LIBADS_CLIENT_BUILD_EXAMPLES` | top level only | example programs (both ways; Win32 on Windows, ncurses elsewhere) |
| `LIBADS_CLIENT_INSTALL` | top level only | install rules and CMake package |

### Mbed TLS

Secure ADS needs **Mbed TLS 3.6** (not 4.x: TwinCAT negotiates DHE-RSA cipher suites, which Mbed TLS 4 no longer
has). CMake first looks for an installed one (`find_package(MbedTLS 3.6)`, e.g. from vcpkg or a distribution package).
If there is none, it downloads the release named in [`cmake/FetchMbedTLS.cmake`](cmake/FetchMbedTLS.cmake), checks its
SHA-256 and builds it along. With `-DLIBADS_CLIENT_FETCH_MBEDTLS=OFF` nothing is downloaded; without Secure ADS
(`-DLIBADS_CLIENT_SECURE=OFF`) Mbed TLS is not needed at all.

As everything is linked statically, Mbed TLS ends up in your program: after a security release of Mbed TLS, rebuild
your program (with an updated Mbed TLS, or with a libads-client release that names it).

## Examples

### Local way, as with the TcAdsDll

```cpp
#include "TcAdsAPI.h"

long port = AdsPortOpenEx();                  // local TwinCAT router
AmsAddr plc{};
AdsGetLocalAddressEx(port, &plc);             // own AmsNetId: the local PLC
plc.port = AMSPORT_R0_PLC_TC3;                // 851

uint16_t adsState = 0, deviceState = 0;
long status = AdsSyncReadStateReqEx(port, &plc, &adsState, &deviceState);

AdsPortCloseEx(port);
```

With the C++ classes, `AdsDevice{target, 851, AdsRouteAdded{}}` uses a route added like this (also a secure one);
`AdsDevice{"192.168.0.10", target, 851}` adds a plain route itself and removes it again.

### C++ classes

```cpp
#include "AdsDevice.h"
#include "AdsVariable.h"

AdsDevice plc{AmsNetId{"192.168.0.10.1.1"}, 851};                        // through the local router
// AdsDevice plc{"192.168.0.10", AmsNetId{"192.168.0.10.1.1"}, 851};     // route way, adds the route itself

AdsVariable<int32_t> value{plc, "MAIN.nValue"};
int32_t v = value;       // read by symbol name
value = v + 1;           // write
```

The classes throw `AdsException` on errors; the C functions return ADS error codes.

### All variables of a PLC

`AdsSymbolTable` uploads the symbols and data types of a PLC once and expands every symbol down to single values:
members of structs and function blocks, array elements with their real bounds (`aOffset[-2]`, `aMatrix[1,2]`,
`aNested[0][2]`), BIT members as byte and bit. `ReadElements()` reads any number of them with sum reads (at most 500
per request) and returns one result per element.

```cpp
#include "AdsDevice.h"
#include "AdsSymbols.h"

AdsDevice plc{AmsNetId{"192.168.0.10.1.1"}, 851};
AdsSymbolTable table{plc};                       // throws AdsException if the upload fails

std::vector<AdsElement> leaves;
table.ForEachLeaf([&](const AdsElement& e) { leaves.push_back(e); });
const auto results = ReadElements(plc, leaves);
for (size_t i = 0; i < leaves.size(); ++i) {
    // leaves[i].path, .typeName, .kind, .size; results[i].error, results[i].data (raw bytes, little endian)
}
```

A single element by its path, without regard to case:

```cpp
AdsElement pos;
if (table.FindElement("GVL.aMachines[2].stAxis.fPos", pos)) {   // also "a[1,2]" and "a[0][2]"
    const auto value = ReadElements(plc, table, {pos});         // throws 0x711 if the PLC has a new program
}
```

For a tree view, `table.Root(symbol)` and `table.Children(element)` go one level at a time. Names, types and comments
are UTF-8. The library does not turn values into text: `AdsElement::kind` and `type` (enum values, array bounds) tell
how to read the bytes, and a `STRING` with `{attribute 'TcEncoding' := 'UTF-8'}` carries that attribute in
`AdsElement::attributes` (otherwise it is in the PLC's code page, usually 1252). Properties are listed only if they
have `{attribute 'monitoring' := 'call'}`; they are read through a handle by their path. Pointers, references and
interfaces are not followed; their value is the address. When the PLC gets a new program (download or online change),
the addresses of the table may be out of date. A download changes the symbol version, an online change does not, but
it counts up `TwinCAT_SystemInfoVarList._AppInfo.OnlineChangeCnt`; the table keeps both. `table.IsCurrent(plc)` tells,
and `ReadElements(plc, table, elements)` reads both along in every request and throws
`ADSERR_DEVICE_SYMBOLVERSIONINVALID` (`0x711`) instead of returning data of another program. Upload a new table then.

[`examples/symbol-browser`](examples/symbol-browser) is a Windows program (Win32 API) built on this: it adds a route
if asked (plain, self-signed or pre-shared key), connects, shows all variables as a tree with type, size and value,
and watches values with notifications (right click). It only reads.

[`examples/symbol-browser-tui`](examples/symbol-browser-tui) does the same in a terminal (ncurses; built everywhere
but Windows, tested on Linux), also over ssh right on a controller without a desktop (TwinCAT for Linux,
TwinCAT/BSD). It connects as its command line says and asks for passwords before it starts:

    symbol-browser-tui                                   # the PLC of this system, local TwinCAT router
    symbol-browser-tui --psk 192.168.1.20 5.80.201.232.1.1    # Secure ADS with a pre-shared key
    symbol-browser-tui --ssc 192.168.1.20 --add-route Administrator   # adds the route first, asks for its AmsNetId

Keys: arrows move, Enter or Right/Left open and close, `/` filters, `b` shows the variables of Beckhoff's libraries,
`n` watches a value with a notification, `p` pauses reading, `u` uploads the symbols again after a new PLC program,
`q` quits; the mouse works too. Building it needs ncurses with wide characters (Debian/Ubuntu: `libncurses-dev`);
without it CMake skips the program. Both examples take their value formatting from
[`examples/common/SymbolText.h`](examples/common/SymbolText.h).

### Route way

The target needs a route to this computer. The library can add it on the target itself, with a user of the target
system, or you add it in TwinCAT on the target.

```cpp
#include "AdsLib.h"

const AmsNetId target{"192.168.0.10.1.1"};
AdsSetLocalAddress(AmsNetId{"192.168.0.20.1.1"});    // own AmsNetId, as named in the target's route

// once: route on the target to us (UDP 48899; the password goes over the network in plain text)
bhf::ads::AddRemoteRoute("192.168.0.10", AmsNetId{"192.168.0.20.1.1"}, "192.168.0.20", "my-pc",
                         "Administrator", "password");

AdsAddRoute(target, "192.168.0.10");                 // route of this program to the target
long port = AdsPortOpenRouteEx();
AmsAddr plc{target, 851};
// ... AdsSyncReadReqEx2(port, &plc, ...) as on the local way
AdsPortCloseEx(port);
```

### Secure ADS (route way)

Three modes, as in TwinCAT:

```cpp
// Self-signed certificates (SSC): add the route on the target once, inside TLS (password encrypted).
// The own certificate is created on first use in the folder (NULL: %ProgramData%\libads-client or
// /etc/libads-client); the target's certificate is stored there and checked from then on.
AdsAddRemoteRouteSecure("192.168.0.10", AmsNetId{"192.168.0.20.1.1"}, "Administrator", "password",
                        nullptr, nullptr, nullptr);
AdsAddRouteSecure(target, "192.168.0.10", nullptr);

// Pre-shared key (PSK): identity and password of a <Psk> entry in the target's StaticRoutes.xml.
AdsAddRoutePsk(target, "192.168.0.10", "my-identity", "psk-password");

// Own CA (SCA): PEM files; the target needs the same CA in its TLS settings.
AdsAddRouteSca(target, "192.168.0.10", "ca.pem", "my-cert.pem", "my-key.pem", nullptr);
```

After that, `AdsPortOpenRouteEx()` and all other functions work as without TLS.

## Behaviour when connections fail

- **Local way:** like the `TcAdsDll`. If TwinCAT restarts while the connection to the router stays (the router
  reports "started"), the library creates the port's notifications again on the target; the program keeps its
  handles. If the connection to the router is lost, the port stays open but every request returns
  `ROUTERERR_NOTREGISTERED` (`0x507`) until the program closes it and opens a new one.
- **Route way:** the connection is restored in the background, immediately and then every second. Requests during
  the outage fail with `ADSERR_CLIENT_SYNCTIMEOUT` (`0x745`) after their timeout; notifications are created again on
  the target after reconnecting, with the same handles for the program.
- **Target that does not answer:** once a request to a target ran into its timeout, deleting a notification there
  does not wait again until the target sends something: the notification is forgotten at once and the call returns
  `ADSERR_CLIENT_SYNCTIMEOUT`. Closing a port with many notifications, or destroying many `AdsNotification`s, so
  takes at most one timeout. A target that answers again (any frame, also a notification) is asked as before.

## Limits

- Client only: the program does not answer ADS requests, and there is no own AMS router.
- The local way needs TwinCAT's XAR in kernel mode on Windows; the router of the user-mode XAR refuses TCP
  connections. The route way works there.
- Not binary compatible with the `TcAdsDll`: there is no `TcAdsDll.dll` replacement, programs are rebuilt.
- Of the sum commands only reading is included (`ReadElements()`); writing and notifications in sums, and the
  special accesses of Beckhoff ADS (registry, file … access), are not included yet.

## Tests

`ctest` in the build folder runs the tests that need neither TwinCAT nor a network: a check of the route way through
the C-API of libads, a test that takes apart symbol uploads built after TwinCAT's format, and two tests of the route
way against a fake TwinCAT router in the same process (about 15 s with `ctest -j4`); libads tests its local router
the same way. The library was also
tested by hand against real TwinCAT systems (Windows, TwinCAT for Linux), including restarts, network outages and
all Secure ADS modes.

## License

MIT, see [`LICENSE`](LICENSE). Parts are taken from Beckhoff ADS (MIT); [`NOTICE.md`](NOTICE.md) lists them and the
changes, and names the third-party software; libads has its own. This project is not affiliated with or endorsed by Beckhoff Automation.
Beckhoff, TwinCAT and ADS are trademarks of Beckhoff Automation GmbH & Co. KG.
