#pragma once

#ifdef _WINDOWS64

#include <string>

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

	void Tick();
	Snapshot GetSnapshot();
	bool StartDeviceLink();
	bool SignOut();
}

#endif
