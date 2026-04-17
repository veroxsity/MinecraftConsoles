#pragma once

#ifdef _WINDOWS64

#include <string>
#include "Windows64_LceLiveP2P.h"

namespace Win64LceLiveSignaling
{
	// -------------------------------------------------------------------------
	// State
	// -------------------------------------------------------------------------

	enum class ESignalingState
	{
		Idle,
		Connecting,    // Worker thread in progress; WebSocket not yet open.
		Connected,     // WebSocket open; own candidate sent; waiting for peer.
		PeerKnown,     // Peer candidate received; ready for hole punching.
		Failed,        // Unrecoverable error; see SignalingSnapshot::errorMessage.
		Closed,        // Cleanly closed.
	};

	struct SignalingSnapshot
	{
		ESignalingState state;
		std::string     sessionId;          // UUID; set once HostConnect() is called.
		std::string     peerIp;             // Set once PeerKnown.
		int             peerPort;           // Set once PeerKnown. 0 until then.
		bool            peerNeedsHolePunch; // true = STUN; false = UPnP direct connect.
		std::wstring    statusMessage;      // Human-readable; always set.
		std::wstring    errorMessage;       // Non-empty only on Failed.
	};

	// -------------------------------------------------------------------------
	// API
	// -------------------------------------------------------------------------

	// Host: generate a session UUID, connect to the signaling server, publish
	// our P2P endpoint. Non-blocking — transition to Connecting is immediate.
	// Returns false if already active (call Close() first).
	bool HostConnect(const std::string& externalIp, int externalPort,
	                 Win64LceLiveP2P::EConnMethod method);

	// Joiner: connect to an existing signaling session, exchange endpoints.
	// sessionId comes from the game invite. Non-blocking.
	// Returns false if already active.
	bool JoinerConnect(const std::string& sessionId,
	                   const std::string& externalIp, int externalPort,
	                   Win64LceLiveP2P::EConnMethod method);

	// Joiner pre-connect: store a session ID so that the frame-loop Tick
	// can call JoinerConnect automatically once P2P discovery finishes.
	// Call this immediately after accepting an invite that carries a session ID.
	void PrepareJoin(const std::string& sessionId);

	// Returns the pending joiner session ID set by PrepareJoin(), or empty if
	// none is pending.  Cleared automatically when JoinerConnect() is called.
	std::string GetPendingJoinerSessionId();

	// Drive the state machine. Call once per frame from the game loop,
	// same cadence as Win64LceLiveP2P::P2PTick().
	void Tick();

	// Thread-safe snapshot of current state.
	// Calls Tick() internally.
	SignalingSnapshot GetSnapshot();

	// Close the signaling connection and reset to Idle.
	// Blocks briefly to join the worker thread.
	void Close();

} // namespace Win64LceLiveSignaling

#endif // _WINDOWS64
