#include "stdafx.h"

#ifdef _WINDOWS64

// Winsock2 must come after stdafx.h (which defines WIN32_LEAN_AND_MEAN before
// windows.h, so the old winsock.h v1 is never pulled in — no conflict).
#include <winsock2.h>
#include <ws2tcpip.h>

// UPnP IGD support via Windows native COM interfaces (no third-party lib needed).
// natupnp.h is in the Windows 8.1+ SDK; comdef.h provides _bstr_t/_variant_t helpers.
#include <natupnp.h>
#include <comdef.h>

#include "Windows64_LceLiveP2P.h"
#include "Windows64_Log.h"
#include "Network/WinsockNetLayer.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

// ============================================================================
// Internal implementation
// ============================================================================

namespace
{
	// -------------------------------------------------------------------------
	// STUN constants (RFC 5389)
	// -------------------------------------------------------------------------

	static const unsigned long  STUN_MAGIC_COOKIE         = 0x2112A442UL;
	static const unsigned short STUN_BINDING_REQUEST      = 0x0001;
	static const unsigned short STUN_BINDING_RESPONSE     = 0x0101;
	static const unsigned short STUN_ATTR_XOR_MAPPED_ADDR = 0x0020;
	static const unsigned short STUN_ATTR_MAPPED_ADDR     = 0x0001;

	static const char* STUN_HOST_PRIMARY  = "stun.l.google.com";
	static const char* STUN_HOST_FALLBACK = "stun1.l.google.com";
	static const int   STUN_PORT          = 19302;
	static const DWORD STUN_TIMEOUT_MS    = 5000;

	// Re-send a keepalive STUN binding request every N ms to keep the NAT
	// mapping alive.
	static const ULONGLONG KEEPALIVE_INTERVAL_MS = 20000ULL;

	// Bring the public EConnMethod into this anonymous namespace for convenience.
	using EConnMethod = Win64LceLiveP2P::EConnMethod;

	// -------------------------------------------------------------------------
	// STUN message helpers
	// -------------------------------------------------------------------------

	struct StunRequest
	{
		unsigned char bytes[20];     // Full 20-byte binding request
		unsigned char txId[12];      // Transaction ID (last 12 bytes)
	};

	struct StunResult
	{
		bool           success;
		std::string    externalIp;
		unsigned short externalPort;
		std::string    errorMessage;
	};

	// Build a 20-byte STUN Binding Request with a fresh random transaction ID.
	StunRequest BuildStunRequest()
	{
		StunRequest req = {};

		for (int i = 0; i < 12; ++i)
			req.txId[i] = static_cast<unsigned char>(rand() & 0xFF);

		unsigned char* p = req.bytes;
		// Message Type: Binding Request (big-endian)
		p[0] = 0x00; p[1] = 0x01;
		// Message Length: 0 (no attributes in a request)
		p[2] = 0x00; p[3] = 0x00;
		// Magic Cookie: 0x2112A442
		p[4] = 0x21; p[5] = 0x12; p[6] = 0xA4; p[7] = 0x42;
		// Transaction ID
		memcpy(p + 8, req.txId, 12);

		return req;
	}

	// Parse a STUN Binding Success Response.
	// Looks for XOR-MAPPED-ADDRESS first, falls back to MAPPED-ADDRESS.
	// Returns true and fills outIp/outPort on success.
	bool ParseStunResponse(
		const unsigned char* data, int length,
		std::string* outIp, unsigned short* outPort)
	{
		if (length < 20)
			return false;

		// Magic cookie check
		if (data[4] != 0x21 || data[5] != 0x12 || data[6] != 0xA4 || data[7] != 0x42)
			return false;

		// Must be a Binding Success Response
		const unsigned short msgType =
			(static_cast<unsigned short>(data[0]) << 8) | data[1];
		if (msgType != STUN_BINDING_RESPONSE)
			return false;

		const unsigned short msgLen =
			(static_cast<unsigned short>(data[2]) << 8) | data[3];
		if (static_cast<int>(msgLen) + 20 > length)
			return false;

		// Walk the attribute list
		int offset = 20;
		const int end = 20 + static_cast<int>(msgLen);

		while (offset + 4 <= end)
		{
			const unsigned short attrType =
				(static_cast<unsigned short>(data[offset]) << 8) | data[offset + 1];
			const unsigned short attrLen =
				(static_cast<unsigned short>(data[offset + 2]) << 8) | data[offset + 3];

			const int valueOffset = offset + 4;
			const int attrEnd = valueOffset + static_cast<int>(attrLen);

			// Bounds check before reading the attribute value
			if (attrEnd > length)
				break;

			if ((attrType == STUN_ATTR_XOR_MAPPED_ADDR || attrType == STUN_ATTR_MAPPED_ADDR)
				&& attrLen >= 8)
			{
				const unsigned char family = data[valueOffset + 1];
				if (family != 0x01) // IPv4 only
				{
					// Skip — IPv6 not supported here
				}
				else if (attrType == STUN_ATTR_XOR_MAPPED_ADDR)
				{
					// X-Port = port XOR (magic_cookie >> 16)
					// X-Address = address XOR magic_cookie
					// Both the packet value and the mask are in network byte order,
					// so we can XOR the big-endian values read directly from the bytes.
					const unsigned short xport =
						(static_cast<unsigned short>(data[valueOffset + 2]) << 8) |
						 data[valueOffset + 3];
					const unsigned short port =
						xport ^ static_cast<unsigned short>(STUN_MAGIC_COOKIE >> 16);

					const unsigned long xaddr =
						(static_cast<unsigned long>(data[valueOffset + 4]) << 24) |
						(static_cast<unsigned long>(data[valueOffset + 5]) << 16) |
						(static_cast<unsigned long>(data[valueOffset + 6]) << 8)  |
						 data[valueOffset + 7];
					const unsigned long addr = xaddr ^ STUN_MAGIC_COOKIE;

					// addr is in host byte order; convert to network for inet_ntop.
					const unsigned long addrNet = htonl(addr);
					char ipStr[INET_ADDRSTRLEN] = {};
					inet_ntop(AF_INET, &addrNet, ipStr, sizeof(ipStr));

					*outIp   = ipStr;
					*outPort = port;
					return true;
				}
				else // STUN_ATTR_MAPPED_ADDR (no XOR)
				{
					const unsigned short port =
						(static_cast<unsigned short>(data[valueOffset + 2]) << 8) |
						 data[valueOffset + 3];
					const unsigned long addr =
						(static_cast<unsigned long>(data[valueOffset + 4]) << 24) |
						(static_cast<unsigned long>(data[valueOffset + 5]) << 16) |
						(static_cast<unsigned long>(data[valueOffset + 6]) << 8)  |
						 data[valueOffset + 7];

					const unsigned long addrNet = htonl(addr);
					char ipStr[INET_ADDRSTRLEN] = {};
					inet_ntop(AF_INET, &addrNet, ipStr, sizeof(ipStr));

					*outIp   = ipStr;
					*outPort = port;
					return true;
				}
			}

			// Advance past this attribute (padded to 4-byte boundary)
			const int padded = (static_cast<int>(attrLen) + 3) & ~3;
			offset += 4 + padded;
		}

		return false;
	}

	// -------------------------------------------------------------------------
	// UPnP IGD helpers
	// -------------------------------------------------------------------------

	struct UPnPResult
	{
		bool        success;
		std::string externalIp;
		int         externalPort;   // same as localPort we passed in
		int         localPort;      // what we registered
		std::string error;
	};

	// Attempts to add a UPnP port mapping (UDP or TCP) via the Windows IUPnPNAT
	// COM interface.  Handles its own CoInitializeEx/CoUninitialize.
	// Blocks for up to a few seconds while UPnP discovery runs.
	// Set tcp=false for UDP (P2P socket), tcp=true for TCP (game server port).
	UPnPResult TryUPnPMapping(int localPort, bool tcp)
	{
		UPnPResult result = {};
		result.localPort = localPort;

		// COM must be initialised on this thread (STA for UPnP callbacks).
		HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const bool coInitialized = SUCCEEDED(hr) || (hr == RPC_E_CHANGED_MODE);

		IUPnPNAT* pNAT = nullptr;
		hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL,
		                      __uuidof(IUPnPNAT), reinterpret_cast<void**>(&pNAT));
		if (FAILED(hr))
		{
			result.error = "UPnP: CoCreateInstance(UPnPNAT) failed hr=0x"
			             + std::to_string(static_cast<unsigned long>(hr));
			if (coInitialized) CoUninitialize();
			return result;
		}

		// get_StaticPortMappingCollection blocks until UPnP discovery completes
		// (~1-3 s on a cooperative router, full timeout if no IGD present).
		IStaticPortMappingCollection* pMappings = nullptr;
		hr = pNAT->get_StaticPortMappingCollection(&pMappings);
		pNAT->Release();

		if (FAILED(hr) || pMappings == nullptr)
		{
			result.error = "UPnP: no IGD found (router may not support UPnP)";
			if (coInitialized) CoUninitialize();
			return result;
		}

		// Determine our local IP address (pick first IPv4 from hostname resolution).
		char localIp[INET_ADDRSTRLEN] = {};
		{
			char hostname[256] = {};
			gethostname(hostname, sizeof(hostname));
			addrinfo hints = {};
			hints.ai_family = AF_INET;
			addrinfo* info  = nullptr;
			if (getaddrinfo(hostname, nullptr, &hints, &info) == 0 && info != nullptr)
			{
				inet_ntop(AF_INET,
				          &reinterpret_cast<sockaddr_in*>(info->ai_addr)->sin_addr,
				          localIp, sizeof(localIp));
				freeaddrinfo(info);
			}
		}

		if (localIp[0] == '\0')
		{
			pMappings->Release();
			result.error = "UPnP: could not determine local IPv4 address";
			if (coInitialized) CoUninitialize();
			return result;
		}

		const wchar_t* protocol    = tcp ? L"TCP" : L"UDP";
		const wchar_t* description = tcp ? L"LceLive Game" : L"LceLive P2P";

		// Remove any stale LceLive mapping on this port (best-effort).
		pMappings->Remove(localPort, _bstr_t(protocol));

		// Convert local IP to wide for the COM API.
		wchar_t localIpW[INET_ADDRSTRLEN] = {};
		MultiByteToWideChar(CP_ACP, 0, localIp, -1, localIpW, INET_ADDRSTRLEN);

		IStaticPortMapping* pMapping = nullptr;
		hr = pMappings->Add(
			localPort,                   // external port
			_bstr_t(protocol),           // protocol
			localPort,                   // internal port
			_bstr_t(localIpW),           // internal client (our LAN IP)
			VARIANT_TRUE,                // enabled
			_bstr_t(description),        // description
			&pMapping
		);
		pMappings->Release();

		if (FAILED(hr) || pMapping == nullptr)
		{
			result.error = "UPnP: AddPortMapping failed hr=0x"
			             + std::to_string(static_cast<unsigned long>(hr));
			if (coInitialized) CoUninitialize();
			return result;
		}

		// Read back the external IP the router assigned.
		BSTR extIpBstr = nullptr;
		pMapping->get_ExternalIPAddress(&extIpBstr);
		if (extIpBstr != nullptr)
		{
			char buf[64] = {};
			WideCharToMultiByte(CP_UTF8, 0, extIpBstr, -1, buf, sizeof(buf), nullptr, nullptr);
			result.externalIp = buf;
			SysFreeString(extIpBstr);
		}
		pMapping->Release();

		if (coInitialized) CoUninitialize();

		// Sanity-check: router must return a non-empty, non-private external IP.
		// If the router returns 0.0.0.0 or a private range we can't use it.
		if (result.externalIp.empty() ||
		    result.externalIp == "0.0.0.0" ||
		    result.externalIp.substr(0, 3) == "10." ||
		    result.externalIp.substr(0, 8) == "192.168." ||
		    result.externalIp.substr(0, 7) == "172.16." ||
		    result.externalIp.substr(0, 7) == "172.17." ||
		    result.externalIp.substr(0, 7) == "172.18." ||
		    result.externalIp.substr(0, 7) == "172.19." ||
		    result.externalIp.substr(0, 7) == "172.20." ||
		    result.externalIp.substr(0, 7) == "172.21." ||
		    result.externalIp.substr(0, 7) == "172.22." ||
		    result.externalIp.substr(0, 7) == "172.23." ||
		    result.externalIp.substr(0, 7) == "172.24." ||
		    result.externalIp.substr(0, 7) == "172.25." ||
		    result.externalIp.substr(0, 7) == "172.26." ||
		    result.externalIp.substr(0, 7) == "172.27." ||
		    result.externalIp.substr(0, 7) == "172.28." ||
		    result.externalIp.substr(0, 7) == "172.29." ||
		    result.externalIp.substr(0, 7) == "172.30." ||
		    result.externalIp.substr(0, 7) == "172.31.")
		{
			result.error = "UPnP: router returned unusable external IP ("
			             + result.externalIp + ")";
			return result;
		}

		result.success      = true;
		result.externalPort = localPort;
		return result;
	}

	// Remove a previously added UPnP port mapping.
	// Call from HostClose() on any thread (handles its own CoInit/Uninit).
	// tcp=false removes a UDP mapping, tcp=true removes a TCP mapping.
	void RemoveUPnPMapping(int port, bool tcp)
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		const bool coInitialized = SUCCEEDED(hr) || (hr == RPC_E_CHANGED_MODE);

		IUPnPNAT* pNAT = nullptr;
		hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL,
		                      __uuidof(IUPnPNAT), reinterpret_cast<void**>(&pNAT));
		if (SUCCEEDED(hr) && pNAT != nullptr)
		{
			IStaticPortMappingCollection* pMappings = nullptr;
			if (SUCCEEDED(pNAT->get_StaticPortMappingCollection(&pMappings)) && pMappings != nullptr)
			{
				pMappings->Remove(port, _bstr_t(tcp ? L"TCP" : L"UDP"));
				pMappings->Release();
			}
			pNAT->Release();
		}

		if (coInitialized) CoUninitialize();
	}

	// -------------------------------------------------------------------------
	// Runtime state
	// -------------------------------------------------------------------------

	struct WorkerResult
	{
		bool         success;
		EConnMethod  method;
		std::string  externalIp;
		int          externalPort;
		std::string  errorMessage;
	};

	struct P2PState
	{
		bool             initialized;
		CRITICAL_SECTION lock;

		Win64LceLiveP2P::EP2PState state;
		EConnMethod      connMethod;    // set once worker succeeds

		// The long-lived UDP socket. INVALID_SOCKET when not open.
		// Owned by the main thread once discovery completes.
		// During discovery, owned by StunWorkerProc.
		SOCKET           udpSocket;
		int              localPort;     // Local port we bound (host order)

		// UPnP: we keep track of mapped ports so HostClose can remove them.
		bool             upnpMappingActive;     // UDP P2P port
		int              upnpMappedPort;
		bool             tcpUpnpMappingActive;  // TCP game port
		int              tcpUpnpMappedPort;

		// Resolved STUN server address — cached to avoid re-DNS on keepalives.
		sockaddr_in      stunServerAddr;
		bool             stunServerAddrValid;

		// Discovery results (written by worker, read by main thread after join)
		WorkerResult     workerResult;
		std::string      externalIp;
		int              externalPort;

		// Worker thread
		HANDLE           workerThread;
		bool             workerDone;

		std::string      lastError;
		ULONGLONG        nextKeepaliveAt;
	};

	static P2PState g_p2p     = {};
	static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

	BOOL CALLBACK InitP2PState(PINIT_ONCE, PVOID, PVOID*)
	{
		// Initialize Winsock once for the lifetime of the process.
		// WinHTTP already does this internally, but we do it explicitly so raw
		// Winsock calls (WSASocket, getaddrinfo, etc.) are available.
		WSADATA wsaData = {};
		WSAStartup(MAKEWORD(2, 2), &wsaData);

		InitializeCriticalSection(&g_p2p.lock);
		g_p2p.state       = Win64LceLiveP2P::EP2PState::Idle;
		g_p2p.udpSocket   = INVALID_SOCKET;
		g_p2p.initialized = true;
		return TRUE;
	}

	void EnsureInitialized()
	{
		InitOnceExecuteOnce(&g_initOnce, &InitP2PState, nullptr, nullptr);
	}

	// -------------------------------------------------------------------------
	// Resolve the STUN server address. Returns true on success.
	// -------------------------------------------------------------------------
	bool ResolveStunServer(const char* host, sockaddr_in* outAddr)
	{
		addrinfo hints = {};
		hints.ai_family   = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;

		char portStr[16] = {};
		sprintf_s(portStr, "%d", STUN_PORT);

		addrinfo* info = nullptr;
		if (getaddrinfo(host, portStr, &hints, &info) != 0 || info == nullptr)
			return false;

		*outAddr = *reinterpret_cast<const sockaddr_in*>(info->ai_addr);
		freeaddrinfo(info);
		return true;
	}

	// -------------------------------------------------------------------------
	// Discovery worker thread
	//
	// Attempt order:
	//   1. UPnP IGD port mapping  — no hole-punching needed, works on ~60% of home routers
	//   2. STUN external endpoint — requires hole-punching, works on ~95% of the rest
	//
	// In both cases the long-lived UDP socket stays open for the life of the
	// host session (keepalives via STUN, actual data via KCP later).
	// -------------------------------------------------------------------------

	DWORD WINAPI DiscoveryWorkerProc(LPVOID)
	{
		// ---- Open and bind the long-lived UDP socket ----
		SOCKET sock = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, 0);
		if (sock == INVALID_SOCKET)
		{
			WorkerResult r = {};
			r.errorMessage = "P2P: failed to create UDP socket (WSA "
			               + std::to_string(WSAGetLastError()) + ")";
			EnterCriticalSection(&g_p2p.lock);
			g_p2p.workerResult = r;
			g_p2p.workerDone   = true;
			LeaveCriticalSection(&g_p2p.lock);
			return 0;
		}

		sockaddr_in bindAddr = {};
		bindAddr.sin_family      = AF_INET;
		bindAddr.sin_addr.s_addr = INADDR_ANY;
		bindAddr.sin_port        = 0;
		if (::bind(sock, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0)
		{
			closesocket(sock);
			WorkerResult r = {};
			r.errorMessage = "P2P: failed to bind UDP socket";
			EnterCriticalSection(&g_p2p.lock);
			g_p2p.workerResult = r;
			g_p2p.workerDone   = true;
			LeaveCriticalSection(&g_p2p.lock);
			return 0;
		}

		sockaddr_in local = {};
		int localLen = sizeof(local);
		getsockname(sock, reinterpret_cast<sockaddr*>(&local), &localLen);
		const int localPort = ntohs(local.sin_port);

		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char*>(&STUN_TIMEOUT_MS), sizeof(STUN_TIMEOUT_MS));

		// ---- Step 1a: UPnP IGD — UDP (P2P socket) ----
		LCELOG("P2P", "trying UPnP IGD UDP (local port %d)", localPort);
		const UPnPResult upnp = TryUPnPMapping(localPort, false /*udp*/);

		// ---- Step 1b: UPnP IGD — TCP (game server port) ----
		// Always attempt regardless of UDP outcome: the router maps UDP and TCP
		// independently. A successful TCP mapping means joiners can reach the game
		// server directly over the internet without port-forwarding.
		const int tcpGamePort = WinsockNetLayer::GetHostPort();
		UPnPResult tcpUpnp = {};
		if (tcpGamePort > 0)
		{
			LCELOG("P2P", "trying UPnP IGD TCP (game port %d)", tcpGamePort);
			tcpUpnp = TryUPnPMapping(tcpGamePort, true /*tcp*/);
			if (tcpUpnp.success)
				LCELOG("P2P", "UPnP TCP game port %d mapped", tcpGamePort);
			else
				LCELOG("P2P", "UPnP TCP failed (%s)", tcpUpnp.error.c_str());
		}

		if (upnp.success)
		{
			LCELOG("P2P", "UPnP UDP mapped — external %s:%d  local port %d",
				upnp.externalIp.c_str(), upnp.externalPort, localPort);

			WorkerResult r = {};
			r.success      = true;
			r.method       = EConnMethod::UPnP;
			r.externalIp   = upnp.externalIp;
			r.externalPort = upnp.externalPort;

			EnterCriticalSection(&g_p2p.lock);
			g_p2p.udpSocket              = sock;
			g_p2p.localPort              = localPort;
			g_p2p.stunServerAddrValid    = false;  // keepalives still done via STUN below
			g_p2p.upnpMappingActive      = true;
			g_p2p.upnpMappedPort         = localPort;
			g_p2p.tcpUpnpMappingActive   = tcpUpnp.success;
			g_p2p.tcpUpnpMappedPort      = tcpUpnp.success ? tcpGamePort : 0;
			g_p2p.workerResult           = r;
			g_p2p.workerDone             = true;
			LeaveCriticalSection(&g_p2p.lock);
			return 0;
		}

		LCELOG("P2P", "UPnP UDP failed (%s) — falling back to STUN", upnp.error.c_str());

		// ---- Step 2: STUN ----
		const char* hosts[2] = { STUN_HOST_PRIMARY, STUN_HOST_FALLBACK };
		WorkerResult result   = {};
		sockaddr_in  serverAddr     = {};
		bool         serverAddrValid = false;

		for (int h = 0; h < 2 && !result.success; ++h)
		{
			const char* stunHost = hosts[h];

			if (!ResolveStunServer(stunHost, &serverAddr))
			{
				result.errorMessage = std::string("P2P: DNS failed for ") + stunHost;
				LCELOG("P2P", "STUN DNS lookup failed for %s", stunHost);
				continue;
			}

			serverAddrValid = true;

			// Try up to 2 transmissions per server
			for (int attempt = 0; attempt < 2 && !result.success; ++attempt)
			{
				const StunRequest req = BuildStunRequest();

				const int sent = sendto(
					sock,
					reinterpret_cast<const char*>(req.bytes), sizeof(req.bytes),
					0,
					reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr));

				if (sent != static_cast<int>(sizeof(req.bytes)))
				{
					result.errorMessage = "P2P: STUN sendto failed";
					continue;
				}

				unsigned char buf[512] = {};
				sockaddr_in   fromAddr = {};
				int           fromLen  = sizeof(fromAddr);

				const int received = recvfrom(
					sock,
					reinterpret_cast<char*>(buf), sizeof(buf),
					0,
					reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

				if (received < 20)
				{
					result.errorMessage = "P2P: STUN timeout or empty response";
					continue;
				}

				std::string    ip   = {};
				unsigned short port = 0;
				if (ParseStunResponse(buf, received, &ip, &port))
				{
					result.success      = true;
					result.method       = EConnMethod::STUN;
					result.externalIp   = ip;
					result.externalPort = port;
				}
				else
				{
					result.errorMessage = "P2P: STUN response parse failed";
				}
			}

			if (!result.success)
				LCELOG("P2P", "STUN failed for %s — %s", stunHost, result.errorMessage.c_str());
		}

		EnterCriticalSection(&g_p2p.lock);
		if (result.success)
		{
			g_p2p.udpSocket             = sock;
			g_p2p.localPort             = localPort;
			g_p2p.stunServerAddr        = serverAddr;
			g_p2p.stunServerAddrValid   = serverAddrValid;
			// TCP UPnP was already attempted at the top (Step 1b) — record result.
			g_p2p.tcpUpnpMappingActive  = tcpUpnp.success;
			g_p2p.tcpUpnpMappedPort     = tcpUpnp.success ? tcpGamePort : 0;
		}
		else
		{
			closesocket(sock);
		}
		g_p2p.workerResult = result;
		g_p2p.workerDone   = true;
		LeaveCriticalSection(&g_p2p.lock);

		return 0;
	}

	// -------------------------------------------------------------------------
	// Keepalive — sends a STUN binding request to keep the NAT mapping warm.
	// Called from P2PTick() outside the critical section.
	// Uses the cached server address; no DNS lookup.
	// -------------------------------------------------------------------------

	void SendKeepalive(SOCKET sock, const sockaddr_in& serverAddr)
	{
		const StunRequest req = BuildStunRequest();
		sendto(
			sock,
			reinterpret_cast<const char*>(req.bytes), sizeof(req.bytes),
			0,
			reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr));

		LCELOG("P2P", "keepalive sent");
	}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Win64LceLiveP2P
{
	bool HostOpen()
	{
		EnsureInitialized();

		EnterCriticalSection(&g_p2p.lock);

		if (g_p2p.state != EP2PState::Idle)
		{
			LeaveCriticalSection(&g_p2p.lock);
			return false; // Already running — caller should HostClose() first.
		}

		g_p2p.state                 = EP2PState::Discovering;
		g_p2p.workerDone            = false;
		g_p2p.connMethod            = EConnMethod::None;
		g_p2p.lastError.clear();
		g_p2p.externalIp.clear();
		g_p2p.externalPort          = 0;
		g_p2p.localPort             = 0;
		g_p2p.upnpMappingActive     = false;
		g_p2p.upnpMappedPort        = 0;
		g_p2p.tcpUpnpMappingActive  = false;
		g_p2p.tcpUpnpMappedPort     = 0;

		g_p2p.workerThread = CreateThread(nullptr, 0, &DiscoveryWorkerProc, nullptr, 0, nullptr);
		if (g_p2p.workerThread == nullptr)
		{
			g_p2p.state     = EP2PState::Failed;
			g_p2p.lastError = "P2P: failed to create discovery thread.";
			LeaveCriticalSection(&g_p2p.lock);
			return false;
		}

		LeaveCriticalSection(&g_p2p.lock);
		LCELOG("P2P", "discovery started (UPnP -> STUN fallback)");
		return true;
	}

	void HostClose()
	{
		EnsureInitialized();

		// Read state we need without blocking the worker.
		HANDLE threadToWait       = nullptr;
		bool   removeUpnp         = false;
		int    upnpPortToRemove   = 0;
		bool   removeTcpUpnp      = false;
		int    tcpUpnpPortToRemove = 0;

		EnterCriticalSection(&g_p2p.lock);
		threadToWait        = g_p2p.workerThread;
		removeUpnp          = g_p2p.upnpMappingActive;
		upnpPortToRemove    = g_p2p.upnpMappedPort;
		removeTcpUpnp       = g_p2p.tcpUpnpMappingActive;
		tcpUpnpPortToRemove = g_p2p.tcpUpnpMappedPort;
		LeaveCriticalSection(&g_p2p.lock);

		// Wait for in-flight discovery thread BEFORE taking the lock again, to
		// avoid deadlocking (the worker also takes the lock at the end of its run).
		if (threadToWait != nullptr)
		{
			WaitForSingleObject(threadToWait, 8000);
			CloseHandle(threadToWait);
		}

		// Remove UPnP mappings while we still have the info (before we clear state).
		if (removeUpnp && upnpPortToRemove != 0)
		{
			LCELOG("P2P", "removing UPnP UDP mapping for port %d", upnpPortToRemove);
			RemoveUPnPMapping(upnpPortToRemove, false /*udp*/);
		}
		if (removeTcpUpnp && tcpUpnpPortToRemove != 0)
		{
			LCELOG("P2P", "removing UPnP TCP mapping for port %d", tcpUpnpPortToRemove);
			RemoveUPnPMapping(tcpUpnpPortToRemove, true /*tcp*/);
		}

		EnterCriticalSection(&g_p2p.lock);
		g_p2p.workerThread = nullptr;

		if (g_p2p.udpSocket != INVALID_SOCKET)
		{
			closesocket(g_p2p.udpSocket);
			g_p2p.udpSocket = INVALID_SOCKET;
		}

		g_p2p.state                = EP2PState::Idle;
		g_p2p.connMethod           = EConnMethod::None;
		g_p2p.localPort            = 0;
		g_p2p.externalIp.clear();
		g_p2p.externalPort         = 0;
		g_p2p.stunServerAddrValid  = false;
		g_p2p.lastError.clear();
		g_p2p.workerDone           = false;
		g_p2p.upnpMappingActive    = false;
		g_p2p.upnpMappedPort       = 0;
		g_p2p.tcpUpnpMappingActive = false;
		g_p2p.tcpUpnpMappedPort    = 0;

		LeaveCriticalSection(&g_p2p.lock);
		LCELOG("P2P", "host socket closed");
	}

	void P2PTick()
	{
		EnsureInitialized();

		EnterCriticalSection(&g_p2p.lock);

		// Integrate a completed discovery
		if (g_p2p.state == EP2PState::Discovering && g_p2p.workerDone)
		{
			HANDLE t = g_p2p.workerThread;
			g_p2p.workerThread = nullptr;
			LeaveCriticalSection(&g_p2p.lock);

			if (t != nullptr)
			{
				WaitForSingleObject(t, INFINITE);
				CloseHandle(t);
			}

			EnterCriticalSection(&g_p2p.lock);
			const WorkerResult& r = g_p2p.workerResult;
			if (r.success)
			{
				g_p2p.state        = EP2PState::Ready;
				g_p2p.connMethod   = r.method;
				g_p2p.externalIp   = r.externalIp;
				g_p2p.externalPort = r.externalPort;
				g_p2p.nextKeepaliveAt = GetTickCount64() + KEEPALIVE_INTERVAL_MS;

				const char* methodName = (r.method == EConnMethod::UPnP) ? "UPnP" : "STUN";
				LCELOG("P2P", "ready via %s — external %s:%d  local port %d",
					methodName, r.externalIp.c_str(), r.externalPort, g_p2p.localPort);
			}
			else
			{
				g_p2p.state     = EP2PState::Failed;
				g_p2p.lastError = r.errorMessage;
				LCELOG("P2P", "all discovery methods failed — %s", r.errorMessage.c_str());
			}
		}

		// Check whether a STUN keepalive is due.
		// UPnP sessions also send keepalives to keep the STUN mapping warm for
		// potential fallback, but only if we have a STUN server address cached.
		bool doKeepalive       = false;
		SOCKET keepaliveSock   = INVALID_SOCKET;
		sockaddr_in keepaliveAddr = {};

		if (g_p2p.state == EP2PState::Ready &&
		    g_p2p.stunServerAddrValid &&
		    GetTickCount64() >= g_p2p.nextKeepaliveAt)
		{
			doKeepalive           = true;
			keepaliveSock         = g_p2p.udpSocket;
			keepaliveAddr         = g_p2p.stunServerAddr;
			g_p2p.nextKeepaliveAt = GetTickCount64() + KEEPALIVE_INTERVAL_MS;
		}

		LeaveCriticalSection(&g_p2p.lock);

		if (doKeepalive && keepaliveSock != INVALID_SOCKET)
			SendKeepalive(keepaliveSock, keepaliveAddr);
	}

	P2PSnapshot GetP2PSnapshot()
	{
		EnsureInitialized();
		P2PTick();

		P2PSnapshot snap = {};

		EnterCriticalSection(&g_p2p.lock);
		snap.state          = g_p2p.state;
		snap.connMethod     = g_p2p.connMethod;
		snap.externalIp     = g_p2p.externalIp;
		snap.externalPort   = g_p2p.externalPort;
		snap.localPort      = g_p2p.localPort;
		snap.tcpPortMapped  = g_p2p.tcpUpnpMappingActive;

		switch (g_p2p.state)
		{
		case EP2PState::Idle:
			snap.statusMessage = L"P2P: idle.";
			break;

		case EP2PState::Discovering:
			snap.statusMessage = L"P2P: discovering external endpoint (UPnP \u2192 STUN)...";
			break;

		case EP2PState::Ready:
		{
			const wchar_t* method = (g_p2p.connMethod == EConnMethod::UPnP) ? L"UPnP" : L"STUN";
			wchar_t buf[256] = {};
			swprintf_s(buf,
				L"P2P ready via %s. External %hs:%d  (local port %d)",
				method,
				g_p2p.externalIp.c_str(), g_p2p.externalPort, g_p2p.localPort);
			snap.statusMessage = buf;
			break;
		}

		case EP2PState::Failed:
			snap.statusMessage = L"P2P: discovery failed (UPnP + STUN). "
				L"Manual port forwarding may be required.";
			snap.errorMessage  = std::wstring(
				g_p2p.lastError.begin(), g_p2p.lastError.end());
			break;
		}

		LeaveCriticalSection(&g_p2p.lock);
		return snap;
	}

} // namespace Win64LceLiveP2P

#endif // _WINDOWS64
