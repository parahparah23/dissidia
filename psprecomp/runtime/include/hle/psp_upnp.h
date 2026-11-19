// psp_upnp.h: ask the home router to forward the ad-hoc UDP port (UPnP IGD), for direct internet play without the party server.
//
// LAN mode with Direct Peers needs the other player to reach this machine's UDP port. With UPnP on (Network page, or
// PSPRECOMP_ADHOC_UPNP=1) a worker thread finds the router (SSDP), asks it to forward the port to this machine
// (AddPortMapping, a one-hour lease renewed every 30 minutes) and reads the public address to give to friends. The mapping is
// removed when UPnP is turned off or the game quits. Nothing is sent to anyone but the router on the local network.
#pragma once
#include <cstdint>
#include <string>

void psp_upnp_set_enabled(bool on);   // saved by the settings menu; on: map the port (now, or once the ad-hoc socket exists)
bool psp_upnp_enabled();
void psp_upnp_port(uint16_t port);    // the ad-hoc layer's bound UDP port (0: none yet)
std::string psp_upnp_status();        // one line for the Network page ("Off", "Looking for the router...", "Open: 1.2.3.4:27312", ...)
std::string psp_upnp_public_address();  // "1.2.3.4:27312" while the port is open, else ""
void psp_upnp_shutdown();             // remove the mapping (exit)
