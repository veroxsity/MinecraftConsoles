#include "stdafx.h"

#ifdef _WINDOWS64

// Winsock2 must appear before any Windows.h inclusion; the PCH has already
// pulled in windows.h, so undef any byte-order macros it may have injected
// (they conflict with the BIGENDIAN/LITTLEENDIAN enum in Definitions.h).
#ifdef BIGENDIAN
#undef BIGENDIAN
#endif
#ifdef LITTLEENDIAN
#undef LITTLEENDIAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <objbase.h>

#include "Windows64_LceLiveRelay.h"
#include "Windows64_LceLive.h"
#include "Windows64_Log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Winhttp.lib")

// Global: joiner sets this to the relay proxy port when it tries direct TCP first.
// The main Tick automatically retries via relay if the direct attempt fails.
int g_LceLiveRelayFallbackPort = 0;

// ============================================================================
// Internal implementation
// ============================================================================

namespace
{
    // -------------------------------------------------------------------------
    // URL / string helpers (mirrored from signaling)
    // -------------------------------------------------------------------------

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) return L"";
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 0) return L"";
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    std::string GetBaseUrl()
    {
        char envValue[512] = {};
        if (GetEnvironmentVariableA("LCELIVE_API_BASE_URL", envValue, sizeof(envValue)) > 0)
            return std::string(envValue);

        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string props(exePath);
        const size_t lastSlash = props.find_last_of("\\/");
        if (lastSlash != std::string::npos) props = props.substr(0, lastSlash + 1);
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

    std::string UrlEncode(const std::string& s)
    {
        std::string out;
        for (unsigned char c : s)
        {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                out += static_cast<char>(c);
            else { char hex[4] = {}; snprintf(hex, sizeof(hex), "%%%02X", c); out += hex; }
        }
        return out;
    }

    // -------------------------------------------------------------------------
    // Recv helpers
    // -------------------------------------------------------------------------

    // Reliable recv: keeps reading until 'len' bytes arrive or an error occurs.
    bool RecvExact(SOCKET sock, void* buf, int len)
    {
        char* p = static_cast<char*>(buf);
        int remaining = len;
        while (remaining > 0)
        {
            const int r = recv(sock, p, remaining, 0);
            if (r <= 0) return false;
            p += r;
            remaining -= r;
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // WebSocket connection helper — opens a WinHTTP WS to the relay endpoint.
    // Returns the HINTERNET handle on success, nullptr on failure.
    // Caller owns the handle and must close it.
    // Also returns hSession/hConnect so caller can clean them up.
    // -------------------------------------------------------------------------

    HINTERNET OpenRelayWebSocket(
        const std::string& sessionId,
        bool isHost,
        const std::string& accessToken,
        const std::string& baseUrl,
        HINTERNET* outSession,
        HINTERNET* outConnect)
    {
        *outSession = nullptr;
        *outConnect = nullptr;

        const std::wstring baseUrlW = Utf8ToWide(baseUrl);

        std::vector<wchar_t> hostBuf(256, 0);
        std::vector<wchar_t> pathBuf(2048, 0);

        URL_COMPONENTSW comp = {};
        comp.dwStructSize     = sizeof(comp);
        comp.lpszHostName     = hostBuf.data();
        comp.dwHostNameLength = static_cast<DWORD>(hostBuf.size());
        comp.lpszUrlPath      = pathBuf.data();
        comp.dwUrlPathLength  = static_cast<DWORD>(pathBuf.size());

        if (!WinHttpCrackUrl(baseUrlW.c_str(), 0, 0, &comp))
            return nullptr;

        const bool secure = (comp.nScheme == INTERNET_SCHEME_HTTPS);
        const std::wstring hostW(comp.lpszHostName, comp.dwHostNameLength);
        const std::wstring basePath = comp.lpszUrlPath
            ? std::wstring(comp.lpszUrlPath, comp.dwUrlPathLength) : L"";

        const std::wstring wsPath = basePath + L"/api/relay/ws"
            + L"?sessionId=" + Utf8ToWide(UrlEncode(sessionId))
            + L"&role="      + (isHost ? L"host" : L"joiner");

        LCELOG("RELAY", "connecting %s%ls",
            secure ? "wss://" : "ws://", (hostW + wsPath).c_str());

        HINTERNET hSession = WinHttpOpen(L"MCLCE-LceLive/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return nullptr;

        WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 30000);

        HINTERNET hConnect = WinHttpConnect(hSession, hostW.c_str(), comp.nPort, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return nullptr; }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wsPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return nullptr; }

        WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

        if (!accessToken.empty())
        {
            const std::wstring auth = L"Authorization: Bearer " + Utf8ToWide(accessToken);
            WinHttpAddRequestHeaders(hRequest, auth.c_str(), static_cast<DWORD>(auth.size()),
                WINHTTP_ADDREQ_FLAG_ADD);
        }

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return nullptr;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr))
        {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return nullptr;
        }

        HINTERNET hWs = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
        WinHttpCloseHandle(hRequest);

        if (!hWs) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return nullptr; }

        *outSession = hSession;
        *outConnect = hConnect;
        return hWs;
    }

    // -------------------------------------------------------------------------
    // Per-direction forwarding thread params
    // -------------------------------------------------------------------------

    struct ForwardWsToTcpParams
    {
        HINTERNET wsHandle;
        SOCKET    tcpSocket;
        std::atomic<bool>* stop;
    };

    struct ForwardTcpToWsParams
    {
        SOCKET    tcpSocket;
        HINTERNET wsHandle;
        CRITICAL_SECTION* wsSendLock;
        std::atomic<bool>* stop;
    };

    // -------------------------------------------------------------------------
    // WS → TCP forwarding thread
    // -------------------------------------------------------------------------

    DWORD WINAPI ForwardWsToTcpProc(LPVOID param)
    {
        auto* p  = static_cast<ForwardWsToTcpParams*>(param);
        std::vector<BYTE> buf(65536);

        while (!p->stop->load())
        {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;

            const DWORD err = WinHttpWebSocketReceive(
                p->wsHandle,
                buf.data(),
                static_cast<DWORD>(buf.size()),
                &bytesRead,
                &bufType);

            if (err != ERROR_SUCCESS)
                break;

            // Only forward binary frames; ignore text (control) frames.
            if (bufType != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE &&
                bufType != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                continue;

            if (bytesRead == 0)
                continue;

            // Write all bytes to the TCP socket.
            const char* src = reinterpret_cast<const char*>(buf.data());
            DWORD remaining = bytesRead;
            while (remaining > 0 && !p->stop->load())
            {
                const int sent = send(p->tcpSocket, src, static_cast<int>(remaining), 0);
                if (sent <= 0) { p->stop->store(true); break; }
                src       += sent;
                remaining -= static_cast<DWORD>(sent);
            }
        }

        p->stop->store(true);
        delete p;
        return 0;
    }

    // -------------------------------------------------------------------------
    // TCP → WS forwarding thread
    // -------------------------------------------------------------------------

    DWORD WINAPI ForwardTcpToWsProc(LPVOID param)
    {
        auto* p  = static_cast<ForwardTcpToWsParams*>(param);
        std::vector<char> buf(65536);

        while (!p->stop->load())
        {
            const int received = recv(p->tcpSocket, buf.data(), static_cast<int>(buf.size()), 0);
            if (received <= 0)
                break;

            EnterCriticalSection(p->wsSendLock);
            const DWORD err = WinHttpWebSocketSend(
                p->wsHandle,
                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                buf.data(),
                static_cast<DWORD>(received));
            LeaveCriticalSection(p->wsSendLock);

            if (err != ERROR_SUCCESS)
                break;
        }

        p->stop->store(true);
        delete p;
        return 0;
    }

    // -------------------------------------------------------------------------
    // Global relay state
    // -------------------------------------------------------------------------

    struct RelayState
    {
        bool             initialized = false;
        CRITICAL_SECTION lock;

        Win64LceLiveRelay::ERelayState state = Win64LceLiveRelay::ERelayState::Idle;
        std::string      lastError;

        // Active handles — closed in Close().
        HINTERNET  wsHandle  = nullptr;
        HINTERNET  wsSession = nullptr;
        HINTERNET  wsConnect = nullptr;
        SOCKET     tcpSocket = INVALID_SOCKET;
        SOCKET     listenSocket = INVALID_SOCKET;  // Joiner only.

        // Forwarding threads.
        HANDLE     wsToTcpThread = nullptr;
        HANDLE     tcpToWsThread = nullptr;
        std::atomic<bool> stopForwarding{false};

        CRITICAL_SECTION wsSendLock;
    };

    static RelayState g_relay;
    static INIT_ONCE  g_initOnce = INIT_ONCE_STATIC_INIT;

    BOOL CALLBACK InitRelayState(PINIT_ONCE, PVOID, PVOID*)
    {
        InitializeCriticalSection(&g_relay.lock);
        InitializeCriticalSection(&g_relay.wsSendLock);
        g_relay.state       = Win64LceLiveRelay::ERelayState::Idle;
        g_relay.initialized = true;
        return TRUE;
    }

    void EnsureInitialized()
    {
        InitOnceExecuteOnce(&g_initOnce, &InitRelayState, nullptr, nullptr);
    }

    void CloseHandlesLocked()
    {
        g_relay.stopForwarding.store(true);

        if (g_relay.wsHandle != nullptr)
        {
            WinHttpWebSocketClose(g_relay.wsHandle,
                WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(g_relay.wsHandle);
            g_relay.wsHandle = nullptr;
        }
        if (g_relay.wsConnect != nullptr) { WinHttpCloseHandle(g_relay.wsConnect); g_relay.wsConnect = nullptr; }
        if (g_relay.wsSession != nullptr) { WinHttpCloseHandle(g_relay.wsSession); g_relay.wsSession = nullptr; }
        if (g_relay.tcpSocket != INVALID_SOCKET)
        {
            closesocket(g_relay.tcpSocket);
            g_relay.tcpSocket = INVALID_SOCKET;
        }
        if (g_relay.listenSocket != INVALID_SOCKET)
        {
            closesocket(g_relay.listenSocket);
            g_relay.listenSocket = INVALID_SOCKET;
        }

        // Wait for forwarding threads (they read from now-closed sockets, so they'll exit).
        auto waitThread = [](HANDLE& h) {
            if (h != nullptr)
            {
                WaitForSingleObject(h, 3000);
                CloseHandle(h);
                h = nullptr;
            }
        };
        LeaveCriticalSection(&g_relay.lock);   // unlock while waiting (threads may need the lock)
        waitThread(g_relay.wsToTcpThread);
        waitThread(g_relay.tcpToWsThread);
        EnterCriticalSection(&g_relay.lock);
    }

    // -------------------------------------------------------------------------
    // Start the two forwarding threads once both WS and TCP are ready.
    // -------------------------------------------------------------------------

    void StartForwarding()
    {
        g_relay.stopForwarding.store(false);

        auto* wsToTcpP          = new ForwardWsToTcpParams();
        wsToTcpP->wsHandle      = g_relay.wsHandle;
        wsToTcpP->tcpSocket     = g_relay.tcpSocket;
        wsToTcpP->stop          = &g_relay.stopForwarding;

        auto* tcpToWsP          = new ForwardTcpToWsParams();
        tcpToWsP->tcpSocket     = g_relay.tcpSocket;
        tcpToWsP->wsHandle      = g_relay.wsHandle;
        tcpToWsP->wsSendLock    = &g_relay.wsSendLock;
        tcpToWsP->stop          = &g_relay.stopForwarding;

        g_relay.wsToTcpThread = CreateThread(nullptr, 0, ForwardWsToTcpProc, wsToTcpP, 0, nullptr);
        g_relay.tcpToWsThread = CreateThread(nullptr, 0, ForwardTcpToWsProc, tcpToWsP, 0, nullptr);

        g_relay.state = Win64LceLiveRelay::ERelayState::Relaying;
        LCELOG("RELAY", "forwarding active — data flowing");
    }

    // -------------------------------------------------------------------------
    // Worker thread context
    // -------------------------------------------------------------------------

    struct WorkerContext
    {
        bool        isHost;
        std::string sessionId;
        int         tcpPort;           // Host: game server port. Joiner: listen port (filled by worker).
        std::string accessToken;
        std::string baseUrl;
    };

    // -------------------------------------------------------------------------
    // Host worker: WS open → wait for joiner's first packet → lazy TCP connect
    //
    // The TCP connection to the local game server is opened LAZILY — only once
    // the first binary frame arrives from the relay server (the joiner's JOIN
    // packet).  Connecting eagerly (before the joiner arrives) leaves a TCP
    // socket idle for up to ~20 s, long enough for the game server to time it
    // out and close it, causing the join to silently fail.
    // -------------------------------------------------------------------------

    DWORD WINAPI HostWorkerProc(LPVOID param)
    {
        auto* ctx = static_cast<WorkerContext*>(param);

        // 1. Open WebSocket to the relay server.
        HINTERNET hSession = nullptr, hConnect = nullptr;
        HINTERNET hWs = OpenRelayWebSocket(ctx->sessionId, true,
            ctx->accessToken, ctx->baseUrl, &hSession, &hConnect);

        if (!hWs)
        {
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = Win64LceLiveRelay::ERelayState::Failed;
            g_relay.lastError = "Relay (host): failed to open WebSocket";
            LeaveCriticalSection(&g_relay.lock);
            LCELOG("RELAY", "host WS open failed");
            delete ctx;
            return 0;
        }

        // Publish the WS handle immediately so Close() can abort the blocking
        // receive below (closing the handle makes WinHttpWebSocketReceive return).
        EnterCriticalSection(&g_relay.lock);
        g_relay.wsHandle  = hWs;
        g_relay.wsSession = hSession;
        g_relay.wsConnect = hConnect;
        LeaveCriticalSection(&g_relay.lock);

        LCELOG("RELAY", "host WS open — session %s, game port %d — waiting for joiner",
            ctx->sessionId.c_str(), ctx->tcpPort);

        // 2. Wait for the relay server to signal that the joiner's WS has connected.
        //    The server sends a TEXT frame "peer_connected" the instant the joiner
        //    arrives, BEFORE any game data flows.  We do NOT open the TCP socket to
        //    the game server until we receive this signal: opening it eagerly leaves
        //    an idle socket the game server can time out in the ~15-20 s before the
        //    joiner arrives.
        //    The server also sends this before its own ForwardLoopAsync, so the signal
        //    is guaranteed to arrive before any binary game-data frames.  However, we
        //    buffer any binary data that sneaks in early and forward it after TCP opens.
        std::vector<BYTE> recvBuf(65536);
        std::vector<BYTE> earlyBinary; // binary frames received before the signal (rare)
        bool signalReceived = false;

        while (!signalReceived)
        {
            DWORD bytesRead = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType =
                WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;

            const DWORD err = WinHttpWebSocketReceive(
                hWs, recvBuf.data(), static_cast<DWORD>(recvBuf.size()),
                &bytesRead, &bufType);

            if (err != ERROR_SUCCESS)
            {
                // WS closed — session ended or Close() was called.
                delete ctx;
                return 0;
            }

            if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && bytesRead > 0)
            {
                const std::string text(reinterpret_cast<const char*>(recvBuf.data()), bytesRead);
                if (text == "peer_connected")
                    signalReceived = true;
                // Unknown text frames are ignored.
            }
            else if (bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                     bufType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            {
                // Unexpected binary data before the signal — buffer it.
                earlyBinary.insert(earlyBinary.end(),
                    recvBuf.begin(), recvBuf.begin() + bytesRead);
                // A complete binary message means the joiner is definitely here.
                if (bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
                    signalReceived = true;
            }
        }

        // 3. NOW connect TCP to the game server — the connection is fresh and the
        //    game server's first packet will arrive immediately (no idle-timeout risk).
        LCELOG("RELAY", "host peer_connected signal — connecting to game port %d", ctx->tcpPort);

        SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcpSock == INVALID_SOCKET)
        {
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = Win64LceLiveRelay::ERelayState::Failed;
            g_relay.lastError = "Relay (host): failed to create TCP socket";
            LeaveCriticalSection(&g_relay.lock);
            delete ctx;
            return 0;
        }

        sockaddr_in sa = {};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port        = htons(static_cast<u_short>(ctx->tcpPort));

        // Retry briefly in case the game server isn't listening yet.
        bool connected = false;
        for (int attempt = 0; attempt < 8 && !connected; ++attempt)
        {
            if (connect(tcpSock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0)
                connected = true;
            else
                Sleep(250);
        }

        if (!connected)
        {
            closesocket(tcpSock);
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = Win64LceLiveRelay::ERelayState::Failed;
            g_relay.lastError = "Relay (host): could not connect to local game server";
            LeaveCriticalSection(&g_relay.lock);
            LCELOG("RELAY", "host TCP connect to 127.0.0.1:%d failed", ctx->tcpPort);
            delete ctx;
            return 0;
        }

        // 4. Forward any binary data buffered before the signal (normally none).
        if (!earlyBinary.empty())
        {
            const char* src    = reinterpret_cast<const char*>(earlyBinary.data());
            int         remain = static_cast<int>(earlyBinary.size());
            while (remain > 0)
            {
                const int sent = send(tcpSock, src, remain, 0);
                if (sent <= 0)
                {
                    closesocket(tcpSock);
                    LCELOG("RELAY", "host TCP send of early-buffered data failed");
                    delete ctx;
                    return 0;
                }
                src    += sent;
                remain -= sent;
            }
        }

        LCELOG("RELAY", "host WS+TCP ready — session %s, game port %d",
            ctx->sessionId.c_str(), ctx->tcpPort);

        // 5. Start the forwarding threads for the rest of the session.
        EnterCriticalSection(&g_relay.lock);
        g_relay.tcpSocket = tcpSock;   // wsHandle already stored in step 1
        StartForwarding();
        LeaveCriticalSection(&g_relay.lock);

        delete ctx;
        return 0;
    }

    // -------------------------------------------------------------------------
    // Joiner worker: listen on local port, WS→accept connection from game
    // -------------------------------------------------------------------------

    DWORD WINAPI JoinerWorkerProc(LPVOID param)
    {
        auto* ctx = static_cast<WorkerContext*>(param);

        // 1. Bind a local TCP listen socket on a random OS-assigned port.
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET)
        {
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = Win64LceLiveRelay::ERelayState::Failed;
            g_relay.lastError = "Relay (joiner): failed to create listen socket";
            LeaveCriticalSection(&g_relay.lock);
            delete ctx;
            return 0;
        }

        sockaddr_in bindAddr = {};
        bindAddr.sin_family      = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bindAddr.sin_port        = 0;  // OS picks the port.

        if (::bind(listenSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0 ||
            listen(listenSock, 1) != 0)
        {
            closesocket(listenSock);
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = Win64LceLiveRelay::ERelayState::Failed;
            g_relay.lastError = "Relay (joiner): bind/listen failed";
            LeaveCriticalSection(&g_relay.lock);
            delete ctx;
            return 0;
        }

        // Read back the assigned port and publish it.
        sockaddr_in actual = {};
        int actualLen = sizeof(actual);
        getsockname(listenSock, reinterpret_cast<sockaddr*>(&actual), &actualLen);
        const int localPort = ntohs(actual.sin_port);

        EnterCriticalSection(&g_relay.lock);
        g_relay.listenSocket = listenSock;
        // Store port in lastError field as a temporary channel (read by JoinerOpen).
        // We piggyback ctx->tcpPort for the return value.
        LeaveCriticalSection(&g_relay.lock);

        ctx->tcpPort = localPort;

        LCELOG("RELAY", "joiner proxy listening on 127.0.0.1:%d", localPort);

        // 2. Connect relay WebSocket (in parallel with waiting for the game to connect).
        HINTERNET hSession = nullptr, hConnect = nullptr;
        HINTERNET hWs = OpenRelayWebSocket(ctx->sessionId, false,
            ctx->accessToken, ctx->baseUrl, &hSession, &hConnect);

        if (!hWs)
        {
            closesocket(listenSock);
            EnterCriticalSection(&g_relay.lock);
            g_relay.listenSocket = INVALID_SOCKET;
            g_relay.state        = Win64LceLiveRelay::ERelayState::Failed;
            g_relay.lastError    = "Relay (joiner): failed to open WebSocket";
            LeaveCriticalSection(&g_relay.lock);
            LCELOG("RELAY", "joiner WS open failed");
            delete ctx;
            return 0;
        }

        // 3. Accept the local TCP connection from the game.
        // Set a generous timeout on the listen socket (game may take a few seconds to call BeginJoinGame).
        DWORD timeout = 30000;
        setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        const SOCKET gameSock = accept(listenSock, nullptr, nullptr);
        closesocket(listenSock);

        EnterCriticalSection(&g_relay.lock);
        g_relay.listenSocket = INVALID_SOCKET;
        LeaveCriticalSection(&g_relay.lock);

        if (gameSock == INVALID_SOCKET)
        {
            WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(hWs); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = Win64LceLiveRelay::ERelayState::Failed;
            g_relay.lastError = "Relay (joiner): game did not connect to proxy port";
            LeaveCriticalSection(&g_relay.lock);
            LCELOG("RELAY", "joiner accept() timed out — game never connected to proxy port");
            delete ctx;
            return 0;
        }

        LCELOG("RELAY", "joiner WS+TCP ready — forwarding started");

        EnterCriticalSection(&g_relay.lock);
        g_relay.wsHandle  = hWs;
        g_relay.wsSession = hSession;
        g_relay.wsConnect = hConnect;
        g_relay.tcpSocket = gameSock;
        StartForwarding();
        LeaveCriticalSection(&g_relay.lock);

        delete ctx;
        return 0;
    }

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Win64LceLiveRelay
{
    bool HostOpen(const std::string& sessionId, int tcpGamePort)
    {
        EnsureInitialized();

        EnterCriticalSection(&g_relay.lock);
        if (g_relay.state != ERelayState::Idle)
        {
            LeaveCriticalSection(&g_relay.lock);
            return false;  // Already active.
        }
        g_relay.state = ERelayState::Connecting;
        g_relay.lastError.clear();
        LeaveCriticalSection(&g_relay.lock);

        auto* ctx       = new WorkerContext();
        ctx->isHost      = true;
        ctx->sessionId   = sessionId;
        ctx->tcpPort     = tcpGamePort;
        ctx->accessToken = Win64LceLive::GetAccessToken();
        ctx->baseUrl     = GetBaseUrl();

        HANDLE t = CreateThread(nullptr, 0, HostWorkerProc, ctx, 0, nullptr);
        if (!t)
        {
            delete ctx;
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = ERelayState::Failed;
            g_relay.lastError = "Relay: failed to create host worker thread";
            LeaveCriticalSection(&g_relay.lock);
            return false;
        }
        CloseHandle(t);  // Thread is detached; lifetime managed by its own logic.

        LCELOG("RELAY", "host open — session %s, game port %d", sessionId.c_str(), tcpGamePort);
        return true;
    }

    int JoinerOpen(const std::string& sessionId)
    {
        EnsureInitialized();

        EnterCriticalSection(&g_relay.lock);
        if (g_relay.state != ERelayState::Idle)
        {
            LeaveCriticalSection(&g_relay.lock);
            return 0;
        }
        g_relay.state = ERelayState::Connecting;
        g_relay.lastError.clear();
        LeaveCriticalSection(&g_relay.lock);

        // We run the joiner worker SYNCHRONOUSLY for the bind/listen phase so we
        // can return the local port to the caller before the game calls JoinGame.
        // The rest (WS connect + accept) runs on the worker thread.
        //
        // To do this we do the bind ourselves here, publish the port, then hand off.

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET)
        {
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = ERelayState::Failed;
            g_relay.lastError = "Relay (joiner): socket() failed";
            LeaveCriticalSection(&g_relay.lock);
            return 0;
        }

        sockaddr_in bindAddr = {};
        bindAddr.sin_family      = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bindAddr.sin_port        = 0;

        if (::bind(listenSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0 ||
            listen(listenSock, 1) != 0)
        {
            closesocket(listenSock);
            EnterCriticalSection(&g_relay.lock);
            g_relay.state     = ERelayState::Failed;
            g_relay.lastError = "Relay (joiner): bind/listen failed";
            LeaveCriticalSection(&g_relay.lock);
            return 0;
        }

        sockaddr_in actual = {};
        int actualLen = sizeof(actual);
        getsockname(listenSock, reinterpret_cast<sockaddr*>(&actual), &actualLen);
        const int localPort = ntohs(actual.sin_port);

        EnterCriticalSection(&g_relay.lock);
        g_relay.listenSocket = listenSock;
        LeaveCriticalSection(&g_relay.lock);

        LCELOG("RELAY", "joiner open — proxy port %d, session %s", localPort, sessionId.c_str());

        // Hand the already-bound listen socket to the worker thread via ctx.
        auto* ctx       = new WorkerContext();
        ctx->isHost      = false;
        ctx->sessionId   = sessionId;
        ctx->tcpPort     = localPort;
        ctx->accessToken = Win64LceLive::GetAccessToken();
        ctx->baseUrl     = GetBaseUrl();

        // Spawn a simplified version of JoinerWorkerProc that skips the bind step
        // (we already did it and stored listenSock in g_relay.listenSocket).
        // We use a lambda-like approach via a separate proc.

        struct AcceptCtx {
            WorkerContext* ctx;
            SOCKET         listenSock;
        };

        auto* actx        = new AcceptCtx();
        actx->ctx         = ctx;
        actx->listenSock  = listenSock;

        HANDLE t = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
            auto* actx = static_cast<AcceptCtx*>(param);
            WorkerContext* ctx = actx->ctx;
            SOCKET listenSock  = actx->listenSock;
            delete actx;

            // Connect relay WebSocket.
            HINTERNET hSession = nullptr, hConnect = nullptr;
            HINTERNET hWs = OpenRelayWebSocket(ctx->sessionId, false,
                ctx->accessToken, ctx->baseUrl, &hSession, &hConnect);

            if (!hWs)
            {
                closesocket(listenSock);
                EnterCriticalSection(&g_relay.lock);
                g_relay.listenSocket = INVALID_SOCKET;
                g_relay.state        = ERelayState::Failed;
                g_relay.lastError    = "Relay (joiner): WebSocket open failed";
                LeaveCriticalSection(&g_relay.lock);
                LCELOG("RELAY", "joiner WS failed");
                delete ctx;
                return 0;
            }

            // Accept the game's connection.
            DWORD timeout = 30000;
            setsockopt(listenSock, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout));

            const SOCKET gameSock = accept(listenSock, nullptr, nullptr);
            closesocket(listenSock);
            EnterCriticalSection(&g_relay.lock);
            g_relay.listenSocket = INVALID_SOCKET;
            LeaveCriticalSection(&g_relay.lock);

            if (gameSock == INVALID_SOCKET)
            {
                WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
                WinHttpCloseHandle(hWs); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
                EnterCriticalSection(&g_relay.lock);
                g_relay.state     = ERelayState::Failed;
                g_relay.lastError = "Relay (joiner): game never connected to proxy";
                LeaveCriticalSection(&g_relay.lock);
                LCELOG("RELAY", "joiner accept() timed out");
                delete ctx;
                return 0;
            }

            LCELOG("RELAY", "joiner WS+TCP ready — forwarding");

            EnterCriticalSection(&g_relay.lock);
            g_relay.wsHandle  = hWs;
            g_relay.wsSession = hSession;
            g_relay.wsConnect = hConnect;
            g_relay.tcpSocket = gameSock;
            StartForwarding();
            LeaveCriticalSection(&g_relay.lock);

            delete ctx;
            return 0;
        }, actx, 0, nullptr);

        if (!t)
        {
            delete actx;  // Also deletes ctx
            closesocket(listenSock);
            EnterCriticalSection(&g_relay.lock);
            g_relay.listenSocket = INVALID_SOCKET;
            g_relay.state        = ERelayState::Failed;
            g_relay.lastError    = "Relay: failed to create joiner worker thread";
            LeaveCriticalSection(&g_relay.lock);
            return 0;
        }
        CloseHandle(t);

        return localPort;
    }

    void Close()
    {
        EnsureInitialized();

        EnterCriticalSection(&g_relay.lock);
        CloseHandlesLocked();
        g_relay.state = ERelayState::Idle;
        g_relay.lastError.clear();
        LeaveCriticalSection(&g_relay.lock);

        LCELOG("RELAY", "closed");
    }

    RelaySnapshot GetSnapshot()
    {
        EnsureInitialized();

        RelaySnapshot snap = {};

        EnterCriticalSection(&g_relay.lock);
        snap.state = g_relay.state;

        switch (g_relay.state)
        {
        case ERelayState::Idle:
            snap.statusMessage = L"Relay: idle.";
            break;
        case ERelayState::Connecting:
            snap.statusMessage = L"Relay: connecting to relay server...";
            break;
        case ERelayState::Relaying:
            snap.statusMessage = L"Relay: active (routing via server).";
            break;
        case ERelayState::Failed:
            snap.statusMessage = L"Relay: failed.";
            snap.errorMessage  = std::wstring(g_relay.lastError.begin(), g_relay.lastError.end());
            break;
        case ERelayState::Closed:
            snap.statusMessage = L"Relay: closed.";
            break;
        }

        LeaveCriticalSection(&g_relay.lock);
        return snap;
    }

} // namespace Win64LceLiveRelay

#endif // _WINDOWS64
