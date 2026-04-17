#pragma once

#ifdef _WINDOWS64

#include <string>

namespace Win64LceLiveP2P
{
	// -------------------------------------------------------------------------
	// State
	// -------------------------------------------------------------------------

	enum class EP2PState
	{
		Idle,          // No socket open; call HostOpen() to start.
		Discovering,   // Discovery in progress (UPnP → STUN) on a background thread.
		Ready,         // External endpoint known; socket is live and kept warm.
		Failed,        // All discovery methods failed; see P2PSnapshot::errorMessage.
	};

	// How the external endpoint was obtained once state == Ready.
	enum class EConnMethod
	{
		None,   // Not yet known.
		UPnP,   // UPnP IGD port mapping — joiner can connect directly.
		STUN,   // STUN-derived endpoint — requires UDP hole punching.
	};

	struct P2PSnapshot
	{
		EP2PState    state;
		EConnMethod  connMethod;    // None until Ready.
		std::string  externalIp;    // Empty until Ready.
		int          externalPort;  // 0 until Ready.
		int          localPort;     // UDP port we bound on this machine.
		bool         tcpPortMapped; // true if UPnP also mapped the TCP game port.
		std::wstring statusMessage; // Human-readable; always set.
		std::wstring errorMessage;  // Non-empty only on Failed.
	};

	// -------------------------------------------------------------------------
	// API
	// -------------------------------------------------------------------------

	// Open the long-lived host UDP socket and begin STUN discovery.
	// Returns true if discovery was successfully kicked off.
	// Non-blocking; transition to Discovering happens immediately.
	// Call P2PTick() regularly to integrate the result.
	// Returns false if already open/discovering/ready (call HostClose first).
	bool HostOpen();

	// Close the host socket and reset to Idle.
	// Blocks briefly to join the discovery thread if it is still running.
	void HostClose();

	// Drive the state machine: integrate completed STUN results, send keepalives.
	// Call once per frame from the game loop, same cadence as Win64LceLive::Tick().
	void P2PTick();

	// Thread-safe snapshot of current state.
	// Calls P2PTick() internally so you can call this without a separate Tick call.
	P2PSnapshot GetP2PSnapshot();
}

#endif // _WINDOWS64
