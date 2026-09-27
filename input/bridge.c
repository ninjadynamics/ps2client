/* Bridge input module for ps2client --input bridge (ps2link P4).
 *
 * A local program (an agent, a script, a test) drives the console by sending
 * full controller states as UDP datagrams to 127.0.0.1:INPUT_BRIDGE_PORT
 * (default 18198 = 0x4716; INPUT_BRIDGE_PORT overrides it, so tandem clients
 * each run their own bridge). The newest
 * valid state is reported on every poll; ps2client owns pacing, sequencing and
 * the console transport. Bridge datagram v1, 14 bytes, little-endian:
 *    0 magic "PKBR"   4 version (1)   5 flags (0)
 *    6 buttons (u16, PS2_PAD_* bits)
 *    8 lx, 9 ly, 10 rx, 11 ry (u8, 0x80 = centre)   12 l2, 13 r2 (u8)
 * A sender holds a state by repeating it; after BRIDGE_HOLD_MS of silence the
 * controls release, so a stopped or crashed agent cannot hold a button.
 * input/bridge.py is the reference sender and a maneuver-file player.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2link-input.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>

#define BRIDGE_DEFAULT_PORT 0x4716
#define BRIDGE_V1           1u
#define BRIDGE_V1_SIZE      14
#define BRIDGE_HOLD_MS      500u

static SOCKET bridge_socket = INVALID_SOCKET;
static ps2_input_state_t bridge_state;
static DWORD bridge_last_ms;
static int bridge_have_state;

static int bridge_start(void)
{
    WSADATA wsa;
    struct sockaddr_in addr;
    u_long nonblocking = 1;
    const char *env = getenv("INPUT_BRIDGE_PORT");
    const int port = env ? atoi(env) : BRIDGE_DEFAULT_PORT;

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "input-bridge: bad INPUT_BRIDGE_PORT '%s'\n", env);
        return -1;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
    bridge_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (bridge_socket == INVALID_SOCKET) {
        WSACleanup();
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)port);
    if (bind(bridge_socket, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        ioctlsocket(bridge_socket, FIONBIO, &nonblocking) != 0) {
        fprintf(stderr, "input-bridge: cannot listen on 127.0.0.1:%d\n", port);
        closesocket(bridge_socket);
        bridge_socket = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    bridge_have_state = 0;
    fprintf(stderr, "input-bridge: listening on 127.0.0.1:%d\n", port);
    return 0;
}

static int bridge_poll(ps2_input_state_t *state)
{
    unsigned char buf[64];
    int len;

    /* Drain the queue: only the newest state matters. */
    while ((len = recv(bridge_socket, (char *)buf, sizeof(buf), 0)) > 0) {
        if (len != BRIDGE_V1_SIZE || memcmp(buf, "PKBR", 4) != 0 || buf[4] != BRIDGE_V1)
            continue;
        bridge_state.buttons = (uint16_t)(buf[6] | (buf[7] << 8));
        bridge_state.lx = buf[8];
        bridge_state.ly = buf[9];
        bridge_state.rx = buf[10];
        bridge_state.ry = buf[11];
        bridge_state.l2 = buf[12];
        bridge_state.r2 = buf[13];
        bridge_last_ms = GetTickCount();
        bridge_have_state = 1;
    }
    if (bridge_have_state && GetTickCount() - bridge_last_ms <= BRIDGE_HOLD_MS)
        *state = bridge_state;
    return 0;   /* otherwise the caller's released state */
}

static void bridge_stop(void)
{
    closesocket(bridge_socket);
    bridge_socket = INVALID_SOCKET;
    WSACleanup();
}

static const ps2_input_module_v1_t bridge_module = {
    PS2_INPUT_MODULE_ABI_V1,
    sizeof(ps2_input_module_v1_t),
    "ps2client bridge (local UDP controller states for agents and scripts)",
    bridge_start,
    bridge_poll,
    bridge_stop,
};

__declspec(dllexport) const ps2_input_module_v1_t *ps2_input_module_get_v1(void)
{
    return &bridge_module;
}
#endif
