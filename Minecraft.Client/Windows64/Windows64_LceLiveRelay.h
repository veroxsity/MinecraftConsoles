#pragma once

#ifdef _WINDOWS64

#include <string>

// ============================================================================
// Win64LceLiveRelay  —  TCP-over-WebSocket relay client
//
// Routes Minecraft game traffic through the LCELive relay server when direct
// TCP is blocked (symmetric NAT, campus/hotel WiFi, etc.).
//
// Host side:
//   Call HostOpen(sessionId, tcpPort) right after signaling HostConnect().
//   The relay client connects to the relay WebSocket as "host", then connects
//   a TCP socket to 127.0.0.1:tcpPort (the running game server).
//   All data is forwarded bidirectionally between the two.
//
// Joiner side:
//   Call JoinerOpen(sessionId) before JoinGameFromInviteInfo().
//   Returns a local TCP port.  Tell the game to connect to 127.0.0.1:<port>.
//   The relay client accepts that connection, connects to the relay WebSocket
//   as "joiner", and forwards all data bidirectionally.
// ============================================================================

namespace Win64LceLiveRelay
{
    enum class ERelayState
    {
        Idle,         // Not started.
        Connecting,   // Opening WebSocket + TCP connections.
        Relaying,     // Fully active; data is flowing.
        Failed,       // Fatal error — see snapshot.errorMessage.
        Closed,       // Cleanly torn down.
    };

    struct RelaySnapshot
    {
        ERelayState  state;
        std::wstring statusMessage;
        std::wstring errorMessage;
    };

    // ---- Host side ----------------------------------------------------------

    // Open the host relay for the given signaling session.
    // tcpGamePort: the local TCP port the game server is listening on (usually 25565).
    // Returns false if a relay is already active (call Close() first).
    bool HostOpen(const std::string& sessionId, int tcpGamePort);

    // ---- Joiner side --------------------------------------------------------

    // Open the joiner relay for the given signaling session.
    // Returns the local TCP port the game should connect to, or 0 on error.
    int JoinerOpen(const std::string& sessionId);

    // ---- Common -------------------------------------------------------------

    // Tear down any active relay connections and reset to Idle.
    void Close();

    // Thread-safe snapshot of current relay state.
    RelaySnapshot GetSnapshot();
}

// Set by the joiner when it tries direct TCP but has a relay proxy port ready as
// fallback.  The main Tick monitors WinsockNetLayer::GetJoinState(); if the direct
// attempt fails it retries automatically via this port (mirrors Xbox Live's TURN
// fallback when direct UDP hole-punch fails).  Cleared after the fallback fires.
extern int g_LceLiveRelayFallbackPort;

#endif // _WINDOWS64
