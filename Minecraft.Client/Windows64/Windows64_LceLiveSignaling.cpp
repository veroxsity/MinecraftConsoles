#include "stdafx.h"

#ifdef _WINDOWS64

#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <objbase.h>    // CoCreateGuid

#include "Windows64_LceLiveSignaling.h"
#include "Windows64_LceLive.h"
#include "Windows64_LceLiveP2P.h"
#include "Windows64_Log.h"
#include "../../../Minecraft.Server/vendor/nlohmann/json.hpp"

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "Winhttp.lib")

// ============================================================================
// Internal implementation
// ============================================================================

namespace
{
	using Json = nlohmann::json;

	// -------------------------------------------------------------------------
	// Helpers
	// -------------------------------------------------------------------------

	std::wstring Utf8ToWideLocal(const std::string& s)
	{
		if (s.empty()) return L"";
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
		if (n <= 0) return L"";
		std::wstring out(static_cast<size_t>(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
		if (!out.empty() && out.back() == L'\0') out.pop_back();
		return out;
	}

	std::string WideToUtf8Local(const std::wstring& s)
	{
		if (s.empty()) return "";
		const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (n <= 0) return "";
		std::string out(static_cast<size_t>(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, nullptr, nullptr);
		if (!out.empty() && out.back() == '\0') out.pop_back();
		return out;
	}

	// Generate a lowercase hyphenated UUID string (e.g. "550e8400-e29b-41d4-a716-446655440000").
	std::string GenerateUuid()
	{
		GUID guid = {};
		CoCreateGuid(&guid);

		char buf[40] = {};
		snprintf(buf, sizeof(buf),
			"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			guid.Data1, guid.Data2, guid.Data3,
			guid.Data4[0], guid.Data4[1],
			guid.Data4[2], guid.Data4[3], guid.Data4[4],
			guid.Data4[5], guid.Data4[6], guid.Data4[7]);
		return buf;
	}

	// Read the API base URL the same way Windows64_LceLive.cpp does:
	// LCELIVE_API_BASE_URL env var → lcelive.properties → localhost:5187.
	std::string GetBaseUrl()
	{
		char envValue[512] = {};
		const DWORD len = GetEnvironmentVariableA("LCELIVE_API_BASE_URL", envValue,
		                                           static_cast<DWORD>(sizeof(envValue)));
		if (len > 0 && len < sizeof(envValue))
			return std::string(envValue);

		// Try lcelive.properties next to the .exe
		char exePath[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		std::string props(exePath);
		const size_t lastSlash = props.find_last_of("\\/");
		if (lastSlash != std::string::npos)
			props = props.substr(0, lastSlash + 1);
		props += "lcelive.properties";

		FILE* f = nullptr;
		if (fopen_s(&f, props.c_str(), "rb") == 0 && f != nullptr)
		{
			char line[512] = {};
			while (fgets(line, sizeof(line), f) != nullptr)
			{
				std::string s(line);
				while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
				if (s.substr(0, 12) == "api_base_url")
				{
					const size_t eq = s.find('=');
					if (eq != std::string::npos) { fclose(f); return s.substr(eq + 1); }
				}
			}
			fclose(f);
		}

		return "http://localhost:5187";
	}

	// URL-percent-encode a string (for safe use in query parameters).
	std::string UrlEncode(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() * 3);
		for (unsigned char c : s)
		{
			if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				out += static_cast<char>(c);
			else
			{
				char hex[4] = {};
				snprintf(hex, sizeof(hex), "%%%02X", c);
				out += hex;
			}
		}
		return out;
	}

	// Build the JSON candidate message we send over the signaling channel.
	std::string BuildCandidateJson(const std::string& ip, int port,
	                                Win64LceLiveP2P::EConnMethod method)
	{
		const char* methodStr =
			(method == Win64LceLiveP2P::EConnMethod::UPnP) ? "upnp" : "stun";

		Json j;
		j["type"]   = "candidate";
		j["ip"]     = ip;
		j["port"]   = port;
		j["method"] = methodStr;
		return j.dump();
	}

	// -------------------------------------------------------------------------
	// Worker context — passed to the background thread
	// -------------------------------------------------------------------------

	struct WorkerContext
	{
		bool        isHost;
		std::string sessionId;
		std::string ourIp;
		int         ourPort;
		Win64LceLiveP2P::EConnMethod ourMethod;
		std::string accessToken;
		std::string baseUrl;

		// Written by worker to signal results back to main thread.
		volatile bool workerDone;
		bool          wsConnected;    // WebSocket opened successfully
		bool          peerReceived;   // Peer candidate decoded
		std::string   peerIp;
		int           peerPort;
		bool          peerNeedsHolePunch;
		std::string   errorMessage;

		// Handle the main thread can close to unblock a stuck WinHttpWebSocketReceive.
		volatile HINTERNET wsHandle;
	};

	// -------------------------------------------------------------------------
	// Runtime state
	// -------------------------------------------------------------------------

	struct SignalingState
	{
		bool             initialized;
		CRITICAL_SECTION lock;

		Win64LceLiveSignaling::ESignalingState state;
		std::string      sessionId;
		std::string      peerIp;
		int              peerPort;
		bool             peerNeedsHolePunch;
		std::string      lastError;

		// Set by PrepareJoin(); cleared when JoinerConnect() fires.
		std::string      pendingJoinerSessionId;

		HANDLE           workerThread;
		WorkerContext*   workerCtx;
	};

	static SignalingState  g_sig     = {};
	static INIT_ONCE       g_initOnce = INIT_ONCE_STATIC_INIT;

	BOOL CALLBACK InitSignalingState(PINIT_ONCE, PVOID, PVOID*)
	{
		InitializeCriticalSection(&g_sig.lock);
		g_sig.state       = Win64LceLiveSignaling::ESignalingState::Idle;
		g_sig.initialized = true;
		return TRUE;
	}

	void EnsureInitialized()
	{
		InitOnceExecuteOnce(&g_initOnce, &InitSignalingState, nullptr, nullptr);
	}

	// -------------------------------------------------------------------------
	// WebSocket worker thread
	// -------------------------------------------------------------------------

	DWORD WINAPI SignalingWorkerProc(LPVOID param)
	{
		WorkerContext* ctx = static_cast<WorkerContext*>(param);

		// ---- Parse the base URL ----
		const std::wstring baseUrlW = Utf8ToWideLocal(ctx->baseUrl);

		std::vector<wchar_t> hostBuf(256, 0);
		std::vector<wchar_t> pathBuf(2048, 0);

		URL_COMPONENTSW components = {};
		components.dwStructSize      = sizeof(components);
		components.lpszHostName      = hostBuf.data();
		components.dwHostNameLength  = static_cast<DWORD>(hostBuf.size());
		components.lpszUrlPath       = pathBuf.data();
		components.dwUrlPathLength   = static_cast<DWORD>(pathBuf.size());

		if (!WinHttpCrackUrl(baseUrlW.c_str(), static_cast<DWORD>(baseUrlW.size()), 0, &components))
		{
			ctx->errorMessage = "Signaling: WinHttpCrackUrl failed for base URL";
			ctx->workerDone   = true;
			return 0;
		}

		const bool secure = (components.nScheme == INTERNET_SCHEME_HTTPS);
		const std::wstring hostW(components.lpszHostName, components.dwHostNameLength);

		// Build the WebSocket path including query params.
		// Auth goes in the Authorization header (server prefers that over ?token=).
		const std::wstring basePath = components.lpszUrlPath
			? std::wstring(components.lpszUrlPath, components.dwUrlPathLength)
			: L"";

		const std::wstring wsPath = basePath + L"/api/signaling/ws"
			+ L"?sessionId=" + Utf8ToWideLocal(ctx->sessionId)
			+ L"&role="      + Utf8ToWideLocal(ctx->isHost ? "host" : "joiner");

		LCELOG("SIG", "connecting to %s%ls (role=%s sessionId=%s)",
			secure ? "wss://" : "ws://",
			(hostW + wsPath).c_str(),
			ctx->isHost ? "host" : "joiner",
			ctx->sessionId.c_str());

		// ---- Open WinHTTP session ----
		HINTERNET hSession = WinHttpOpen(
			L"MCLCE-LceLive/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0);

		if (hSession == nullptr)
		{
			ctx->errorMessage = "Signaling: WinHttpOpen failed";
			ctx->workerDone   = true;
			return 0;
		}

		WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 30000);

		HINTERNET hConnect = WinHttpConnect(hSession, hostW.c_str(), components.nPort, 0);
		if (hConnect == nullptr)
		{
			WinHttpCloseHandle(hSession);
			ctx->errorMessage = "Signaling: WinHttpConnect failed";
			ctx->workerDone   = true;
			return 0;
		}

		// ---- Open GET request (will be upgraded to WebSocket) ----
		const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
		HINTERNET hRequest = WinHttpOpenRequest(
			hConnect,
			L"GET",
			wsPath.c_str(),
			nullptr,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			flags);

		if (hRequest == nullptr)
		{
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			ctx->errorMessage = "Signaling: WinHttpOpenRequest failed";
			ctx->workerDone   = true;
			return 0;
		}

		// Mark this request for WebSocket upgrade BEFORE sending.
		WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

		// Add Authorization header.
		if (!ctx->accessToken.empty())
		{
			const std::wstring authHeader =
				L"Authorization: Bearer " + Utf8ToWideLocal(ctx->accessToken);
			WinHttpAddRequestHeaders(hRequest,
				authHeader.c_str(),
				static_cast<DWORD>(authHeader.size()),
				WINHTTP_ADDREQ_FLAG_ADD);
		}

		// Send the upgrade request.
		if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
		                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
		{
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			ctx->errorMessage = "Signaling: WinHttpSendRequest failed (WSA "
			                  + std::to_string(GetLastError()) + ")";
			ctx->workerDone   = true;
			return 0;
		}

		if (!WinHttpReceiveResponse(hRequest, nullptr))
		{
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			ctx->errorMessage = "Signaling: WinHttpReceiveResponse failed — server may be down";
			ctx->workerDone   = true;
			return 0;
		}

		// ---- Complete WebSocket upgrade ----
		HINTERNET hWs = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
		WinHttpCloseHandle(hRequest);

		if (hWs == nullptr)
		{
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			ctx->errorMessage = "Signaling: WebSocket upgrade failed — server returned non-101";
			ctx->workerDone   = true;
			return 0;
		}

		// Make the handle visible to the main thread so Close() can abort the receive.
		ctx->wsHandle    = hWs;
		ctx->wsConnected = true;

		LCELOG("SIG", "WebSocket connected (session %s)", ctx->sessionId.c_str());

		// ---- Send our P2P candidate ----
		const std::string candidateJson =
			BuildCandidateJson(ctx->ourIp, ctx->ourPort, ctx->ourMethod);

		DWORD sendResult = WinHttpWebSocketSend(
			hWs,
			WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
			const_cast<PVOID>(reinterpret_cast<const void*>(candidateJson.c_str())),
			static_cast<DWORD>(candidateJson.size()));

		if (sendResult != ERROR_SUCCESS)
		{
			WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
			WinHttpCloseHandle(hWs);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			ctx->wsHandle     = nullptr;
			ctx->errorMessage = "Signaling: failed to send candidate";
			ctx->workerDone   = true;
			return 0;
		}

		LCELOG("SIG", "candidate sent %s", candidateJson.c_str());

		// ---- Receive loop: wait for peer's candidate ----
		// We also pass through joiner_connected notifications (host only) so the
		// log shows when the joiner arrives.
		std::vector<BYTE> recvBuf(8192);
		bool done = false;

		while (!done)
		{
			DWORD                           bytesRead  = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE  bufType    = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;

			const DWORD recvErr = WinHttpWebSocketReceive(
				hWs,
				recvBuf.data(),
				static_cast<DWORD>(recvBuf.size() - 1),
				&bytesRead,
				&bufType);

			if (recvErr != ERROR_SUCCESS)
			{
				// Closed from main thread (Close() called) or network error.
				LCELOG("SIG", "receive ended (%lu)", recvErr);
				break;
			}

			if (bufType != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
			    bufType != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
				continue;  // binary or close frame — ignore

			recvBuf[bytesRead] = 0;
			const std::string msg(reinterpret_cast<char*>(recvBuf.data()), bytesRead);

			LCELOG("SIG", "recv %s", msg.c_str());

			try
			{
				const Json j = Json::parse(msg);
				const std::string type = j.value("type", "");

				if (type == "joiner_connected")
				{
					// Host-only: the joiner has arrived on the signaling channel.
					// Their candidate will follow shortly.
					LCELOG("SIG", "joiner connected on session %s", ctx->sessionId.c_str());
				}
				else if (type == "candidate")
				{
					const std::string peerIp     = j.value("ip", "");
					const int         peerPort   = j.value("port", 0);
					const std::string methodStr  = j.value("method", "stun");
					const bool        holePunch  = (methodStr == "stun");

					if (!peerIp.empty() && peerPort > 0)
					{
						ctx->peerIp              = peerIp;
						ctx->peerPort            = peerPort;
						ctx->peerNeedsHolePunch  = holePunch;
						ctx->peerReceived        = true;

						LCELOG("SIG", "peer endpoint %s:%d (method=%s)",
							peerIp.c_str(), peerPort, methodStr.c_str());

						done = true;  // Both candidates exchanged — we're done here.
					}
				}
			}
			catch (...) {}  // Malformed JSON — skip
		}

		// ---- Clean up ----
		WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
		WinHttpCloseHandle(hWs);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		ctx->wsHandle   = nullptr;
		ctx->workerDone = true;
		return 0;
	}

	// -------------------------------------------------------------------------
	// Common start helper (host & joiner share 90% of setup)
	// -------------------------------------------------------------------------

	bool StartWorker(bool isHost,
	                 const std::string& sessionId,
	                 const std::string& externalIp, int externalPort,
	                 Win64LceLiveP2P::EConnMethod method)
	{
		EnsureInitialized();

		EnterCriticalSection(&g_sig.lock);
		if (g_sig.state != Win64LceLiveSignaling::ESignalingState::Idle)
		{
			LeaveCriticalSection(&g_sig.lock);
			return false;
		}

		WorkerContext* ctx = new WorkerContext();
		ctx->isHost          = isHost;
		ctx->sessionId       = sessionId;
		ctx->ourIp           = externalIp;
		ctx->ourPort         = externalPort;
		ctx->ourMethod       = method;
		ctx->accessToken     = Win64LceLive::GetAccessToken();
		ctx->baseUrl         = GetBaseUrl();
		ctx->workerDone      = false;
		ctx->wsConnected     = false;
		ctx->peerReceived    = false;
		ctx->peerPort        = 0;
		ctx->peerNeedsHolePunch = false;
		ctx->wsHandle        = nullptr;

		g_sig.state     = Win64LceLiveSignaling::ESignalingState::Connecting;
		g_sig.sessionId = sessionId;
		g_sig.peerPort  = 0;
		g_sig.lastError.clear();
		g_sig.peerIp.clear();
		g_sig.workerCtx = ctx;

		g_sig.workerThread = CreateThread(nullptr, 0, &SignalingWorkerProc, ctx, 0, nullptr);
		if (g_sig.workerThread == nullptr)
		{
			g_sig.state     = Win64LceLiveSignaling::ESignalingState::Failed;
			g_sig.lastError = "Signaling: failed to create worker thread";
			delete ctx;
			g_sig.workerCtx = nullptr;
			LeaveCriticalSection(&g_sig.lock);
			return false;
		}

		LeaveCriticalSection(&g_sig.lock);
		return true;
	}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Win64LceLiveSignaling
{
	bool HostConnect(const std::string& externalIp, int externalPort,
	                 Win64LceLiveP2P::EConnMethod method)
	{
		const std::string sessionId = GenerateUuid();
		LCELOG("SIG", "hosting session %s  endpoint %s:%d",
			sessionId.c_str(), externalIp.c_str(), externalPort);
		return StartWorker(true, sessionId, externalIp, externalPort, method);
	}

	bool JoinerConnect(const std::string& sessionId,
	                   const std::string& externalIp, int externalPort,
	                   Win64LceLiveP2P::EConnMethod method)
	{
		// Clear the pending ID — we're acting on it now.
		EnterCriticalSection(&g_sig.lock);
		g_sig.pendingJoinerSessionId.clear();
		LeaveCriticalSection(&g_sig.lock);

		LCELOG("SIG", "joining session %s  our endpoint %s:%d",
			sessionId.c_str(), externalIp.c_str(), externalPort);
		return StartWorker(false, sessionId, externalIp, externalPort, method);
	}

	void PrepareJoin(const std::string& sessionId)
	{
		EnsureInitialized();
		EnterCriticalSection(&g_sig.lock);
		g_sig.pendingJoinerSessionId = sessionId;
		LeaveCriticalSection(&g_sig.lock);
		LCELOG("SIG", "joiner session ID stored (%s) — waiting for P2P", sessionId.c_str());
	}

	std::string GetPendingJoinerSessionId()
	{
		EnsureInitialized();
		EnterCriticalSection(&g_sig.lock);
		const std::string id = g_sig.pendingJoinerSessionId;
		LeaveCriticalSection(&g_sig.lock);
		return id;
	}

	void Tick()
	{
		EnsureInitialized();

		EnterCriticalSection(&g_sig.lock);

		if (g_sig.workerCtx == nullptr ||
		    (g_sig.state != ESignalingState::Connecting &&
		     g_sig.state != ESignalingState::Connected))
		{
			LeaveCriticalSection(&g_sig.lock);
			return;
		}

		WorkerContext* ctx = g_sig.workerCtx;

		// Promote Connecting → Connected once WebSocket is open.
		if (g_sig.state == ESignalingState::Connecting && ctx->wsConnected)
			g_sig.state = ESignalingState::Connected;

		// Integrate completed peer exchange.
		if (ctx->workerDone)
		{
			HANDLE t = g_sig.workerThread;
			g_sig.workerThread = nullptr;
			LeaveCriticalSection(&g_sig.lock);

			if (t != nullptr)
			{
				WaitForSingleObject(t, INFINITE);
				CloseHandle(t);
			}

			EnterCriticalSection(&g_sig.lock);
			if (ctx->peerReceived)
			{
				g_sig.state              = ESignalingState::PeerKnown;
				g_sig.peerIp             = ctx->peerIp;
				g_sig.peerPort           = ctx->peerPort;
				g_sig.peerNeedsHolePunch = ctx->peerNeedsHolePunch;
				LCELOG("SIG", "peer known %s:%d (holePunch=%d)",
					ctx->peerIp.c_str(), ctx->peerPort, ctx->peerNeedsHolePunch ? 1 : 0);
			}
			else
			{
				g_sig.state     = ESignalingState::Failed;
				g_sig.lastError = ctx->errorMessage.empty()
				                ? "Signaling: connection closed before peer candidate received"
				                : ctx->errorMessage;
				LCELOG("SIG", "failed — %s", g_sig.lastError.c_str());
			}

			delete ctx;
			g_sig.workerCtx = nullptr;
		}

		LeaveCriticalSection(&g_sig.lock);
	}

	SignalingSnapshot GetSnapshot()
	{
		EnsureInitialized();
		Tick();

		SignalingSnapshot snap = {};

		EnterCriticalSection(&g_sig.lock);
		snap.state              = g_sig.state;
		snap.sessionId          = g_sig.sessionId;
		snap.peerIp             = g_sig.peerIp;
		snap.peerPort           = g_sig.peerPort;
		snap.peerNeedsHolePunch = g_sig.peerNeedsHolePunch;

		switch (g_sig.state)
		{
		case ESignalingState::Idle:
			snap.statusMessage = L"Signaling: idle.";
			break;
		case ESignalingState::Connecting:
			snap.statusMessage = L"Signaling: connecting to relay...";
			break;
		case ESignalingState::Connected:
			snap.statusMessage = L"Signaling: waiting for peer...";
			break;
		case ESignalingState::PeerKnown:
		{
			wchar_t buf[256] = {};
			swprintf_s(buf, L"Signaling: peer at %hs:%d (%s)",
				g_sig.peerIp.c_str(), g_sig.peerPort,
				g_sig.peerNeedsHolePunch ? L"hole punch" : L"direct");
			snap.statusMessage = buf;
			break;
		}
		case ESignalingState::Failed:
			snap.statusMessage = L"Signaling: failed.";
			snap.errorMessage  = std::wstring(g_sig.lastError.begin(), g_sig.lastError.end());
			break;
		case ESignalingState::Closed:
			snap.statusMessage = L"Signaling: closed.";
			break;
		}

		LeaveCriticalSection(&g_sig.lock);
		return snap;
	}

	void Close()
	{
		EnsureInitialized();

		// Unblock any in-progress WinHttpWebSocketReceive by closing the handle.
		HINTERNET wsToClose = nullptr;
		EnterCriticalSection(&g_sig.lock);
		if (g_sig.workerCtx != nullptr)
			wsToClose = g_sig.workerCtx->wsHandle;
		LeaveCriticalSection(&g_sig.lock);

		if (wsToClose != nullptr)
		{
			// Sending a Close frame unblocks the receive on the worker thread.
			WinHttpWebSocketClose(wsToClose,
				WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
		}

		// Now wait for the worker to finish.
		HANDLE t = nullptr;
		EnterCriticalSection(&g_sig.lock);
		t = g_sig.workerThread;
		LeaveCriticalSection(&g_sig.lock);

		if (t != nullptr)
		{
			WaitForSingleObject(t, 5000);
			CloseHandle(t);
		}

		EnterCriticalSection(&g_sig.lock);
		if (g_sig.workerCtx != nullptr)
		{
			delete g_sig.workerCtx;
			g_sig.workerCtx = nullptr;
		}
		g_sig.workerThread           = nullptr;
		g_sig.state                  = ESignalingState::Closed;
		g_sig.sessionId.clear();
		g_sig.peerIp.clear();
		g_sig.peerPort               = 0;
		g_sig.peerNeedsHolePunch     = false;
		g_sig.lastError.clear();
		g_sig.pendingJoinerSessionId.clear();
		LeaveCriticalSection(&g_sig.lock);

		LCELOG("SIG", "closed");
	}

} // namespace Win64LceLiveSignaling

#endif // _WINDOWS64
