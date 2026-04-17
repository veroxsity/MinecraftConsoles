#pragma once

#ifdef _WINDOWS64

#include <string>
#include <vector>

namespace Win64LceLive
{
	enum class EClientState
	{
		SignedOut,
		StartingLink,
		LinkPending,
		Polling,
		SignedIn,
	};

	struct Snapshot
	{
		EClientState state;
		bool requestInFlight;
		bool hasError;
		std::wstring accountDisplayName;
		std::wstring accountUsername;
		std::wstring accountId;
		std::wstring verificationUri;
		std::wstring verificationUriComplete;
		std::wstring userCode;
		std::wstring statusMessage;
		std::wstring errorMessage;
	};

	struct TicketResult
	{
		bool success;
		std::string ticket;  // ASCII/URL-safe; empty on failure
		std::string error;   // human-readable; empty on success
	};

	// Social feature types
	struct SocialEntry
	{
		std::string accountId;
		std::string username;
		std::string displayName;
	};

	struct FriendsListResult
	{
		bool success;
		std::vector<SocialEntry> friends;
		std::string error;
	};

	struct PendingRequestsResult
	{
		bool success;
		std::vector<SocialEntry> incoming;
		std::vector<SocialEntry> outgoing;
		std::string error;
	};

	struct SocialActionResult
	{
		bool success;
		std::string error;
	};

	struct GameInviteEntry
	{
		std::string inviteId;
		std::string senderAccountId;
		std::string senderUsername;
		std::string senderDisplayName;
		std::string recipientAccountId;
		std::string recipientUsername;
		std::string recipientDisplayName;
		std::string hostIp;
		int hostPort;
		std::string hostName;
		std::string status;
		bool sessionActive;
		std::string createdUtc;
		std::string expiresUtc;
		std::string signalingSessionId; // empty if host didn't provide one (Phase 4b+)
	};

	struct GameInvitesResult
	{
		bool success;
		std::vector<GameInviteEntry> incoming;
		std::vector<GameInviteEntry> outgoing;
		std::string error;
	};

	struct GameInviteActionResult
	{
		bool success;
		std::string inviteId;
		std::string hostIp;
		int hostPort;
		std::string hostName;
		std::string signalingSessionId; // empty if not a Phase 4b session
		std::string error;
	};

	void Tick();
	Snapshot GetSnapshot();
	bool StartDeviceLink();
	bool SignOut();

	// Returns the current access token if signed in, or an empty string.
	// Synchronous — reads from cached state, never blocks.
	std::string GetAccessToken();

	// Requests a short-lived join ticket from the LceLive API.
	// Synchronous blocking call (~localhost RTT). Call from a non-UI thread.
	TicketResult RequestJoinTicketSync();

	// Validates a join ticket presented by a remote client by calling the LceLive API.
	// Synchronous blocking call. Returns true if the ticket is valid and not yet used.
	// Populates out-params with the verified account identity on success.
	bool ValidateJoinTicketSync(const std::string& ticket,
	                            std::string* outAccountId,
	                            std::string* outUsername,
	                            std::string* outDisplayName);

	// Social sync functions — all blocking, call from UI thread only when brief (~localhost RTT).
	FriendsListResult      GetFriendsSync();
	PendingRequestsResult  GetPendingRequestsSync();
	SocialActionResult     SendFriendRequestSync(const std::string& username);
	SocialActionResult     AcceptFriendRequestSync(const std::string& fromAccountId);
	SocialActionResult     DeclineFriendRequestSync(const std::string& fromAccountId);
	SocialActionResult     RemoveFriendSync(const std::string& accountId);

	GameInvitesResult      GetGameInvitesSync();
	SocialActionResult     SendGameInviteSync(const std::string& recipientAccountId, const std::string& hostIp, int hostPort, const std::string& hostName, const std::string& signalingSessionId);
	GameInviteActionResult AcceptGameInviteSync(const std::string& inviteId);
	SocialActionResult     DeclineGameInviteSync(const std::string& inviteId);
	SocialActionResult     DeactivateGameInvitesSync();
}

#endif
