// Fake TwinCAT router as a program (FakeRouter.h), controlled by commands on stdin.
// Usage: fake-router [listen port] [netid]     (default 48900, 192.168.0.99.1.1)
// Commands: status | drop | stop | start | restart | silent on|off | refuse <port> | allow <port> |
//           event <0|1|2> | state <ads state> | quit

#include "FakeRouter.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    FakeRouter::Options options;
    if (argc > 1) options.listenPort = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) {
        unsigned b[6];
        if (std::sscanf(argv[2], "%u.%u.%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
            std::fprintf(stderr, "usage: fake-router [listen port] [netid]\n");
            return 2;
        }
        for (int i = 0; i < 6; ++i) options.netId[i] = static_cast<uint8_t>(b[i]);
    }
    FakeRouter router(options);
    if (!router.Start()) {
        std::fprintf(stderr, "cannot listen on 127.0.0.1:%u\n", options.listenPort);
        return 1;
    }
    std::cout << router.Status() << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string command, argument;
        in >> command >> argument;
        if (command.empty()) continue;
        if (command == "quit") break;
        if (command == "drop") router.DropAll();
        else if (command == "stop") router.Stop();
        else if (command == "start") { if (!router.Start()) std::cout << "start failed" << std::endl; }
        else if (command == "restart") { if (!router.Restart()) std::cout << "start failed" << std::endl; }
        else if (command == "silent") router.SetSilent(argument != "off");
        else if (command == "refuse") router.RefusePort(static_cast<uint16_t>(std::atoi(argument.c_str())));
        else if (command == "allow") router.RefusePort(static_cast<uint16_t>(std::atoi(argument.c_str())), false);
        else if (command == "event") router.SendRouterEvent(static_cast<uint32_t>(std::atoi(argument.c_str())));
        else if (command == "state") router.SetAdsState(static_cast<uint16_t>(std::atoi(argument.c_str())));
        else if (command != "status") { std::cout << "unknown command: " << command << std::endl; continue; }
        std::cout << router.Status() << std::endl;
    }
    return 0;
}
