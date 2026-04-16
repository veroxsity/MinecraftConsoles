#include "stdafx.h"

#ifdef _WINDOWS64

#include "Windows64_LceLive.h"

#include "Windows64_Xuid.h"
#include "../../Minecraft.World/StringHelpers.h"
#include "../../Minecraft.Server/vendor/nlohmann/json.hpp"

#include <Windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	using Json = nlohmann::json;

	enum class ERequestType
	{
		None,
		StartLink,
		Poll,
		Refresh,
		Logout,
	};

	struct AuthSession
	{
		bool valid;
		std::string accountId;
		std::string username;
		std::string displayName;
		std::string accessToken;
		std::string refreshToken;
	};

	struct PendingLink
	{
		bool active;
		std::string deviceCode;
		std::string userCode;
		std::string verificationUri;
		std::string verificationUriComplete;
		ULONGLONG nextPollAt;
	};

	struct RequestContext
	{
		ERequestType type;
		std::string path;
		std::string body;
	};

	struct CompletedRequest
	{
		ERequestType type;
		bool transportOk;
		DWORD httpStatus;
		std::string responseBody;
	};

	struct RuntimeState
	{
		bool initialized;
		CRITICAL_SECTION lock;
		bool requestInFlight;
		bool completedReady;
		bool sessionRefreshInFlight;
		HANDLE workerThread;
		RequestContext request;
		CompletedRequest completed;
		AuthSession session;
		PendingLink pendingLink;
		std::string lastError;
	};

	RuntimeState g_state = {};
	INIT_ONCE g_initializeOnce = INIT_ONCE_STATIC_INIT;
	INIT_ONCE g_authBlobPathOnce = INIT_ONCE_STATIC_INIT;
	char g_authBlobPath[MAX_PATH] = {};

	bool BuildExeRelativePath(const char *fileName, char *outPath, size_t outPathSize)
	{
		if (fileName == nullptr || outPath == nullptr || outPathSize == 0)
			return false;

		outPath[0] = 0;

		char exePath[MAX_PATH] = {};
		DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		if (len == 0 || len >= MAX_PATH)
			return false;

		char *lastSlash = strrchr(exePath, '\\');
		if (lastSlash != nullptr)
			*(lastSlash + 1) = 0;

		if (strcpy_s(outPath, outPathSize, exePath) != 0)
			return false;
		if (strcat_s(outPath, outPathSize, fileName) != 0)
			return false;

		return true;
	}

	std::wstring Utf8ToWide(const std::string &text)
	{
		if (text.empty())
			return L"";

		const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
		if (length <= 0)
			return convStringToWstring(text);

		std::wstring result;
		result.resize(static_cast<size_t>(length));
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], length);
		return result;
	}

	std::string WideToUtf8(const std::wstring &text)
	{
		if (text.empty())
			return std::string();

		const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (length <= 0)
			return std::string(wstringtochararray(text));

		std::string result;
		result.resize(static_cast<size_t>(length));
		WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], length, nullptr, nullptr);
		return result;
	}

	std::string JsonStringOrEmpty(const Json &object, const char *key)
	{
		const Json::const_iterator it = object.find(key);
		if (it == object.end() || !it->is_string())
			return std::string();
		return it->get<std::string>();
	}

	std::string ParseErrorMessage(const std::string &responseBody, const std::string &fallback);

	void TrimTrailingSlashes(std::string *value)
	{
		if (value == nullptr)
			return;

		while (!value->empty() && (value->back() == '/' || value->back() == '\\' || value->back() == '\r' || value->back() == '\n' || value->back() == ' ' || value->back() == '\t'))
			value->pop_back();
	}

	std::string BuildDeviceId()
	{
		const unsigned long long xuid = static_cast<unsigned long long>(Win64Xuid::ResolvePersistentXuid());
		char buffer[64] = {};
		sprintf_s(buffer, "win64-%016llX", xuid);
		return buffer;
	}

	std::string BuildDeviceName()
	{
		wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
		DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
		if (GetComputerNameW(computerName, &size))
		{
			const std::wstring displayName = L"MCLCE Windows64 (" + std::wstring(computerName) + L")";
			return WideToUtf8(displayName);
		}

		return "MCLCE Windows64";
	}

	std::string GetApiBaseUrl()
	{
		char envValue[512] = {};
		const DWORD envLength = GetEnvironmentVariableA("LCELIVE_API_BASE_URL", envValue, static_cast<DWORD>(sizeof(envValue)));
		if (envLength > 0 && envLength < sizeof(envValue))
		{
			std::string value(envValue);
			TrimTrailingSlashes(&value);
			return value;
		}

		char configPath[MAX_PATH] = {};
		if (BuildExeRelativePath("lcelive.properties", configPath, sizeof(configPath)))
		{
			FILE *file = nullptr;
			if (fopen_s(&file, configPath, "rb") == 0 && file != nullptr)
			{
				char line[512] = {};
				while (fgets(line, sizeof(line), file) != nullptr)
				{
					std::string currentLine(line);
					while (!currentLine.empty() &&
						(currentLine[currentLine.size() - 1] == '\n' || currentLine[currentLine.size() - 1] == '\r'))
					{
						currentLine.erase(currentLine.size() - 1);
					}

					const size_t equalsIndex = currentLine.find('=');
					if (equalsIndex == std::string::npos)
						continue;

					if (currentLine.substr(0, equalsIndex) == "api_base_url")
					{
						fclose(file);
						std::string value = currentLine.substr(equalsIndex + 1);
						TrimTrailingSlashes(&value);
						return value;
					}
				}

				fclose(file);
			}
		}

		std::string fallback = "http://localhost:5187";
		TrimTrailingSlashes(&fallback);
		return fallback;
	}

	std::string BuildHttpFailureMessage(DWORD httpStatus, const std::string &responseBody, const std::string &fallback)
	{
		const std::string parsed = ParseErrorMessage(responseBody, std::string());
		if (!parsed.empty())
			return parsed;

		switch (httpStatus)
		{
		case 404:
			return "LCELive start request returned HTTP 404. Check the API base URL and port.";
		case 500:
			return "LCELive API returned HTTP 500 while creating a device link.";
		case 502:
		case 503:
		case 504:
			return "LCELive API is unavailable right now.";
		default:
			break;
		}

		char buffer[96] = {};
		sprintf_s(buffer, "LCELive start request failed with HTTP %lu.", static_cast<unsigned long>(httpStatus));
		return fallback.empty() ? std::string(buffer) : std::string(buffer);
	}

	bool ReadFileBytes(const char *path, std::vector<unsigned char> *outBytes)
	{
		if (path == nullptr || outBytes == nullptr)
			return false;

		FILE *file = nullptr;
		if (fopen_s(&file, path, "rb") != 0 || file == nullptr)
			return false;

		if (fseek(file, 0, SEEK_END) != 0)
		{
			fclose(file);
			return false;
		}

		const long fileSize = ftell(file);
		if (fileSize <= 0)
		{
			fclose(file);
			return false;
		}

		if (fseek(file, 0, SEEK_SET) != 0)
		{
			fclose(file);
			return false;
		}

		outBytes->resize(static_cast<size_t>(fileSize));
		const size_t readCount = fread(outBytes->data(), 1, outBytes->size(), file);
		fclose(file);
		return readCount == outBytes->size();
	}

	bool WriteFileBytes(const char *path, const unsigned char *data, size_t size)
	{
		if (path == nullptr || data == nullptr || size == 0)
			return false;

		FILE *file = nullptr;
		if (fopen_s(&file, path, "wb") != 0 || file == nullptr)
			return false;

		const size_t writeCount = fwrite(data, 1, size, file);
		fclose(file);
		return writeCount == size;
	}

	bool ProtectString(const std::string &plainText, std::vector<unsigned char> *outEncrypted)
	{
		if (outEncrypted == nullptr)
			return false;

		DATA_BLOB inputBlob = {};
		inputBlob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plainText.data()));
		inputBlob.cbData = static_cast<DWORD>(plainText.size());

		DATA_BLOB outputBlob = {};
		if (!CryptProtectData(&inputBlob, L"LCELive", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outputBlob))
			return false;

		outEncrypted->assign(outputBlob.pbData, outputBlob.pbData + outputBlob.cbData);
		LocalFree(outputBlob.pbData);
		return true;
	}

	bool UnprotectBytes(const std::vector<unsigned char> &encrypted, std::string *outPlainText)
	{
		if (outPlainText == nullptr || encrypted.empty())
			return false;

		DATA_BLOB inputBlob = {};
		inputBlob.pbData = const_cast<BYTE *>(encrypted.data());
		inputBlob.cbData = static_cast<DWORD>(encrypted.size());

		DATA_BLOB outputBlob = {};
		if (!CryptUnprotectData(&inputBlob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outputBlob))
			return false;

		outPlainText->assign(reinterpret_cast<const char *>(outputBlob.pbData), reinterpret_cast<const char *>(outputBlob.pbData) + outputBlob.cbData);
		LocalFree(outputBlob.pbData);
		return true;
	}

	const char *GetAuthBlobPath()
	{
		InitOnceExecuteOnce(&g_authBlobPathOnce,
			[](PINIT_ONCE, PVOID, PVOID *) -> BOOL
			{
				BuildExeRelativePath("lcelive_auth.dat", g_authBlobPath, sizeof(g_authBlobPath));
				return TRUE;
			},
			nullptr,
			nullptr);
		return g_authBlobPath;
	}

	void SaveAuthSessionLocked()
	{
		if (!g_state.session.valid)
		{
			DeleteFileA(GetAuthBlobPath());
			return;
		}

		Json sessionJson;
		sessionJson["version"] = 1;
		sessionJson["accountId"] = g_state.session.accountId;
		sessionJson["username"] = g_state.session.username;
		sessionJson["displayName"] = g_state.session.displayName;
		sessionJson["accessToken"] = g_state.session.accessToken;
		sessionJson["refreshToken"] = g_state.session.refreshToken;

		std::vector<unsigned char> encrypted;
		if (!ProtectString(sessionJson.dump(), &encrypted))
		{
			app.DebugPrintf("LCELive: failed to protect auth blob for local storage\n");
			return;
		}

		if (!WriteFileBytes(GetAuthBlobPath(), encrypted.data(), encrypted.size()))
			app.DebugPrintf("LCELive: failed to write auth blob to disk\n");
	}

	void ClearSessionLocked()
	{
		g_state.session = {};
		g_state.pendingLink = {};
		g_state.lastError.clear();
		DeleteFileA(GetAuthBlobPath());
	}

	void LoadPersistedSessionLocked()
	{
		std::vector<unsigned char> encrypted;
		if (!ReadFileBytes(GetAuthBlobPath(), &encrypted))
			return;

		std::string decrypted;
		if (!UnprotectBytes(encrypted, &decrypted))
		{
			app.DebugPrintf("LCELive: unable to decrypt stored auth state, clearing local blob\n");
			DeleteFileA(GetAuthBlobPath());
			return;
		}

		const Json sessionJson = Json::parse(decrypted, nullptr, false);
		if (!sessionJson.is_object())
		{
			app.DebugPrintf("LCELive: stored auth state is invalid JSON, clearing local blob\n");
			DeleteFileA(GetAuthBlobPath());
			return;
		}

		g_state.session.refreshToken = JsonStringOrEmpty(sessionJson, "refreshToken");
		if (g_state.session.refreshToken.empty())
		{
			DeleteFileA(GetAuthBlobPath());
			g_state.session = {};
			return;
		}

		g_state.session.valid = true;
		g_state.session.accountId = JsonStringOrEmpty(sessionJson, "accountId");
		g_state.session.username = JsonStringOrEmpty(sessionJson, "username");
		g_state.session.displayName = JsonStringOrEmpty(sessionJson, "displayName");
		g_state.session.accessToken = JsonStringOrEmpty(sessionJson, "accessToken");
	}

	bool CrackUrl(const std::wstring &baseUrl, URL_COMPONENTSW *outComponents, std::vector<wchar_t> *hostBuffer, std::vector<wchar_t> *pathBuffer)
	{
		if (outComponents == nullptr || hostBuffer == nullptr || pathBuffer == nullptr)
			return false;

		hostBuffer->assign(256, 0);
		pathBuffer->assign(2048, 0);

		ZeroMemory(outComponents, sizeof(*outComponents));
		outComponents->dwStructSize = sizeof(*outComponents);
		outComponents->lpszHostName = hostBuffer->data();
		outComponents->dwHostNameLength = static_cast<DWORD>(hostBuffer->size());
		outComponents->lpszUrlPath = pathBuffer->data();
		outComponents->dwUrlPathLength = static_cast<DWORD>(pathBuffer->size());

		return WinHttpCrackUrl(baseUrl.c_str(), static_cast<DWORD>(baseUrl.length()), 0, outComponents) == TRUE;
	}

	bool PerformJsonRequest(const RequestContext &request, DWORD *outStatus, std::string *outResponseBody)
	{
		if (outStatus == nullptr || outResponseBody == nullptr)
			return false;

		*outStatus = 0;
		outResponseBody->clear();

		const std::string baseUrlUtf8 = GetApiBaseUrl();
		const std::wstring baseUrl = Utf8ToWide(baseUrlUtf8);
		URL_COMPONENTSW components = {};
		std::vector<wchar_t> hostBuffer;
		std::vector<wchar_t> pathBuffer;
		if (!CrackUrl(baseUrl, &components, &hostBuffer, &pathBuffer))
		{
			app.DebugPrintf("LCELive: WinHttpCrackUrl failed for '%s'\n", baseUrlUtf8.c_str());
			return false;
		}

		std::wstring fullPath = components.lpszUrlPath != nullptr
			? std::wstring(components.lpszUrlPath, components.dwUrlPathLength)
			: std::wstring();
		if (!request.path.empty())
			fullPath += Utf8ToWide(request.path);
		if (fullPath.empty())
			fullPath = L"/";

		const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

		HINTERNET session = WinHttpOpen(L"MCLCE-LceLive/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (session == nullptr)
			return false;

		WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);

		const std::wstring hostWide(components.lpszHostName, components.dwHostNameLength);
		HINTERNET connection = WinHttpConnect(session, hostWide.c_str(), components.nPort, 0);
		if (connection == nullptr)
		{
			WinHttpCloseHandle(session);
			return false;
		}

		const wchar_t *verb = request.body.empty() ? L"GET" : L"POST";
		HINTERNET requestHandle = WinHttpOpenRequest(connection, verb, fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
		if (requestHandle == nullptr)
		{
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		const wchar_t *headers = request.body.empty()
			? L"Accept: application/json\r\n"
			: L"Content-Type: application/json\r\nAccept: application/json\r\n";
		LPVOID sendBuffer = WINHTTP_NO_REQUEST_DATA;
		DWORD sendSize = 0;
		if (!request.body.empty())
		{
			sendBuffer = const_cast<char *>(request.body.data());
			sendSize = static_cast<DWORD>(request.body.size());
		}

		const BOOL sendOk = WinHttpSendRequest(requestHandle, headers, -1L, sendBuffer, sendSize, sendSize, 0);
		if (!sendOk || !WinHttpReceiveResponse(requestHandle, nullptr))
		{
			WinHttpCloseHandle(requestHandle);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		DWORD statusCode = 0;
		DWORD statusCodeSize = sizeof(statusCode);
		if (!WinHttpQueryHeaders(requestHandle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX))
		{
			WinHttpCloseHandle(requestHandle);
			WinHttpCloseHandle(connection);
			WinHttpCloseHandle(session);
			return false;
		}

		std::string responseBody;
		for (;;)
		{
			DWORD bytesAvailable = 0;
			if (!WinHttpQueryDataAvailable(requestHandle, &bytesAvailable))
				break;
			if (bytesAvailable == 0)
				break;

			std::vector<char> buffer(bytesAvailable);
			DWORD bytesRead = 0;
			if (!WinHttpReadData(requestHandle, buffer.data(), bytesAvailable, &bytesRead))
				break;

			responseBody.append(buffer.data(), buffer.data() + bytesRead);
		}

		*outStatus = statusCode;
		*outResponseBody = responseBody;

		WinHttpCloseHandle(requestHandle);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return true;
	}

	std::string ParseErrorMessage(const std::string &responseBody, const std::string &fallback)
	{
		if (responseBody.empty())
			return fallback;

		const Json responseJson = Json::parse(responseBody, nullptr, false);
		if (!responseJson.is_object())
			return fallback;

		const std::string message = JsonStringOrEmpty(responseJson, "message");
		return message.empty() ? fallback : message;
	}

	DWORD WINAPI RequestThreadProc(LPVOID)
	{
		RequestContext request = {};
		EnterCriticalSection(&g_state.lock);
		request = g_state.request;
		LeaveCriticalSection(&g_state.lock);

		CompletedRequest completed = {};
		completed.type = request.type;
		completed.transportOk = PerformJsonRequest(request, &completed.httpStatus, &completed.responseBody);

		EnterCriticalSection(&g_state.lock);
		g_state.completed = completed;
		g_state.completedReady = true;
		g_state.requestInFlight = false;
		LeaveCriticalSection(&g_state.lock);
		return 0;
	}

	bool QueueRequestLocked(ERequestType type, const std::string &path, const std::string &body)
	{
		if (g_state.requestInFlight)
			return false;

		if (g_state.workerThread != nullptr)
		{
			WaitForSingleObject(g_state.workerThread, INFINITE);
			CloseHandle(g_state.workerThread);
			g_state.workerThread = nullptr;
		}

		g_state.request = {};
		g_state.request.type = type;
		g_state.request.path = path;
		g_state.request.body = body;
		g_state.requestInFlight = true;
		g_state.completedReady = false;
		g_state.workerThread = CreateThread(nullptr, 0, &RequestThreadProc, nullptr, 0, nullptr);
		if (g_state.workerThread == nullptr)
		{
			g_state.requestInFlight = false;
			g_state.lastError = "Unable to create LCELive worker thread.";
			return false;
		}

		return true;
	}

	void QueueSessionRefreshLocked()
	{
		if (!g_state.session.valid || g_state.session.refreshToken.empty() || g_state.requestInFlight)
			return;

		Json requestJson;
		requestJson["refreshToken"] = g_state.session.refreshToken;
		if (QueueRequestLocked(ERequestType::Refresh, "/api/auth/refresh", requestJson.dump()))
			g_state.sessionRefreshInFlight = true;
	}

	BOOL CALLBACK InitializeRuntimeState(PINIT_ONCE, PVOID, PVOID *)
	{
		InitializeCriticalSection(&g_state.lock);

		EnterCriticalSection(&g_state.lock);
		g_state.initialized = true;
		LoadPersistedSessionLocked();
		QueueSessionRefreshLocked();
		LeaveCriticalSection(&g_state.lock);

		return TRUE;
	}

	void EnsureInitialized()
	{
		InitOnceExecuteOnce(&g_initializeOnce, &InitializeRuntimeState, nullptr, nullptr);
	}

	void ApplyStartLinkResponseLocked(const CompletedRequest &completed)
	{
		if (!completed.transportOk)
		{
			g_state.lastError = "Unable to contact LCELive.";
			return;
		}

		if (completed.httpStatus < 200 || completed.httpStatus >= 300)
		{
			g_state.lastError = BuildHttpFailureMessage(completed.httpStatus, completed.responseBody, "LCELive rejected the device-link request.");
			return;
		}

		const Json responseJson = Json::parse(completed.responseBody, nullptr, false);
		if (!responseJson.is_object())
		{
			g_state.lastError = "LCELive returned an invalid device-link response.";
			return;
		}

		g_state.pendingLink = {};
		g_state.pendingLink.active = true;
		g_state.pendingLink.deviceCode = JsonStringOrEmpty(responseJson, "deviceCode");
		g_state.pendingLink.userCode = JsonStringOrEmpty(responseJson, "userCode");
		g_state.pendingLink.verificationUri = JsonStringOrEmpty(responseJson, "verificationUri");
		g_state.pendingLink.verificationUriComplete = JsonStringOrEmpty(responseJson, "verificationUriComplete");

		int intervalSeconds = 5;
		const Json::const_iterator intervalIt = responseJson.find("intervalSeconds");
		if (intervalIt != responseJson.end() && intervalIt->is_number_integer())
			intervalSeconds = intervalIt->get<int>();
		if (intervalSeconds < 1)
			intervalSeconds = 5;

		g_state.pendingLink.nextPollAt = GetTickCount64() + (static_cast<ULONGLONG>(intervalSeconds) * 1000ULL);
		g_state.lastError.clear();
	}

	void ApplyPollResponseLocked(const CompletedRequest &completed)
	{
		if (!g_state.pendingLink.active)
			return;

		if (!completed.transportOk)
		{
			g_state.pendingLink.active = false;
			g_state.lastError = "LCELive polling failed. Press A to request a new code.";
			return;
		}

		if (completed.httpStatus < 200 || completed.httpStatus >= 300)
		{
			g_state.pendingLink.active = false;
			g_state.lastError = ParseErrorMessage(completed.responseBody, "LCELive rejected the device poll.");
			return;
		}

		const Json responseJson = Json::parse(completed.responseBody, nullptr, false);
		if (!responseJson.is_object())
		{
			g_state.pendingLink.active = false;
			g_state.lastError = "LCELive returned an invalid poll response.";
			return;
		}

		const std::string status = JsonStringOrEmpty(responseJson, "status");
		const bool isLinked = responseJson.contains("isLinked") && responseJson["isLinked"].is_boolean() && responseJson["isLinked"].get<bool>();
		if (isLinked)
		{
			const Json::const_iterator accountIt = responseJson.find("account");
			if (accountIt == responseJson.end() || !accountIt->is_object())
			{
				g_state.pendingLink.active = false;
				g_state.lastError = "LCELive linked the device but omitted account details.";
				return;
			}

			g_state.session.valid = true;
			g_state.session.accountId = JsonStringOrEmpty(*accountIt, "accountId");
			g_state.session.username = JsonStringOrEmpty(*accountIt, "username");
			g_state.session.displayName = JsonStringOrEmpty(*accountIt, "displayName");
			g_state.session.accessToken = JsonStringOrEmpty(responseJson, "accessToken");
			g_state.session.refreshToken = JsonStringOrEmpty(responseJson, "refreshToken");
			g_state.pendingLink = {};
			g_state.lastError.clear();
			SaveAuthSessionLocked();
			return;
		}

		if (status == "pending")
		{
			g_state.pendingLink.nextPollAt = GetTickCount64() + 5000ULL;
			g_state.lastError.clear();
			return;
		}

		g_state.pendingLink.active = false;
		if (status == "expired")
			g_state.lastError = "That device code expired. Press A to request a new one.";
		else
			g_state.lastError = "LCELive returned an unexpected poll status.";
	}

	void ApplyRefreshResponseLocked(const CompletedRequest &completed)
	{
		g_state.sessionRefreshInFlight = false;
		if (!g_state.session.valid)
			return;

		if (!completed.transportOk)
		{
			g_state.lastError = "Unable to reach LCELive. Using the cached local sign-in for now.";
			return;
		}

		if (completed.httpStatus < 200 || completed.httpStatus >= 300)
		{
			ClearSessionLocked();
			g_state.lastError = "Saved LCELive sign-in expired. Press A to link this device again.";
			return;
		}

		const Json responseJson = Json::parse(completed.responseBody, nullptr, false);
		if (!responseJson.is_object())
		{
			g_state.lastError = "LCELive returned an invalid session refresh response.";
			return;
		}

		const Json::const_iterator accountIt = responseJson.find("account");
		if (accountIt == responseJson.end() || !accountIt->is_object())
		{
			ClearSessionLocked();
			g_state.lastError = "LCELive refresh omitted account details. Link this device again.";
			return;
		}

		const std::string refreshedAccessToken = JsonStringOrEmpty(responseJson, "accessToken");
		const std::string refreshedRefreshToken = JsonStringOrEmpty(responseJson, "refreshToken");
		if (refreshedAccessToken.empty() || refreshedRefreshToken.empty())
		{
			ClearSessionLocked();
			g_state.lastError = "LCELive refresh returned incomplete credentials. Link this device again.";
			return;
		}

		g_state.session.valid = true;
		g_state.session.accountId = JsonStringOrEmpty(*accountIt, "accountId");
		g_state.session.username = JsonStringOrEmpty(*accountIt, "username");
		g_state.session.displayName = JsonStringOrEmpty(*accountIt, "displayName");
		g_state.session.accessToken = refreshedAccessToken;
		g_state.session.refreshToken = refreshedRefreshToken;
		g_state.lastError.clear();
		SaveAuthSessionLocked();
	}

	void IntegrateCompletedRequestLocked()
	{
		if (!g_state.completedReady)
			return;

		const CompletedRequest completed = g_state.completed;
		g_state.completedReady = false;

		if (g_state.workerThread != nullptr)
		{
			WaitForSingleObject(g_state.workerThread, INFINITE);
			CloseHandle(g_state.workerThread);
			g_state.workerThread = nullptr;
		}

		switch (completed.type)
		{
		case ERequestType::StartLink:
			ApplyStartLinkResponseLocked(completed);
			break;
		case ERequestType::Poll:
			ApplyPollResponseLocked(completed);
			break;
		case ERequestType::Refresh:
			ApplyRefreshResponseLocked(completed);
			break;
		case ERequestType::Logout:
		default:
			break;
		}
	}

	std::wstring BuildStatusMessageLocked()
	{
		if (g_state.session.valid)
		{
			std::wstring message;
			message += L"Signed in as:\r\n";
			message += Utf8ToWide(g_state.session.displayName);
			if (!g_state.session.username.empty())
			{
				message += L" (@";
				message += Utf8ToWide(g_state.session.username);
				message += L")";
			}

			message += L"\r\nThis device is linked locally for LCELive features.";
			if (g_state.sessionRefreshInFlight)
				message += L"\r\nRefreshing the saved session with LCELive...";

			message += L"\r\nPress A to sign out on this device.";
			return message;
		}

		if (g_state.pendingLink.active)
		{
			std::wstring message;
			message += L"Go to:\r\n";
			message += Utf8ToWide(g_state.pendingLink.verificationUri);
			message += L"\r\nEnter code:\r\n";
			message += Utf8ToWide(g_state.pendingLink.userCode);
			message += L"\r\nThis screen will keep checking until the link completes.";
			return message;
		}

		return L"Sign-in is optional. Offline play is unchanged.\r\nPress A to request a device code.\r\nThen visit the LCELive link page and enter the code shown here.";
	}
}

namespace Win64LceLive
{
	void Tick()
	{
		EnsureInitialized();

		EnterCriticalSection(&g_state.lock);
		IntegrateCompletedRequestLocked();
		if (g_state.pendingLink.active && !g_state.requestInFlight && GetTickCount64() >= g_state.pendingLink.nextPollAt)
			QueueRequestLocked(ERequestType::Poll, "/api/auth/device/poll/" + g_state.pendingLink.deviceCode, std::string());
		LeaveCriticalSection(&g_state.lock);
	}

	Snapshot GetSnapshot()
	{
		EnsureInitialized();
		Tick();

		Snapshot snapshot = {};
		EnterCriticalSection(&g_state.lock);
		snapshot.requestInFlight = g_state.requestInFlight;
		snapshot.hasError = !g_state.lastError.empty();
		snapshot.errorMessage = Utf8ToWide(g_state.lastError);
		snapshot.statusMessage = BuildStatusMessageLocked();

		if (g_state.session.valid)
		{
			snapshot.state = EClientState::SignedIn;
			snapshot.accountDisplayName = Utf8ToWide(g_state.session.displayName);
			snapshot.accountUsername = Utf8ToWide(g_state.session.username);
			snapshot.accountId = Utf8ToWide(g_state.session.accountId);
		}
		else if (g_state.pendingLink.active)
		{
			snapshot.state = g_state.requestInFlight ? EClientState::Polling : EClientState::LinkPending;
			snapshot.verificationUri = Utf8ToWide(g_state.pendingLink.verificationUri);
			snapshot.verificationUriComplete = Utf8ToWide(g_state.pendingLink.verificationUriComplete);
			snapshot.userCode = Utf8ToWide(g_state.pendingLink.userCode);
		}
		else if (g_state.requestInFlight)
		{
			snapshot.state = EClientState::StartingLink;
		}
		else
		{
			snapshot.state = EClientState::SignedOut;
		}
		LeaveCriticalSection(&g_state.lock);

		return snapshot;
	}

	bool StartDeviceLink()
	{
		EnsureInitialized();

		EnterCriticalSection(&g_state.lock);
		IntegrateCompletedRequestLocked();
		if (g_state.requestInFlight || g_state.session.valid)
		{
			LeaveCriticalSection(&g_state.lock);
			return false;
		}

		g_state.pendingLink = {};
		g_state.lastError.clear();

		Json requestJson;
		requestJson["deviceId"] = BuildDeviceId();
		requestJson["deviceName"] = BuildDeviceName();

		const bool queued = QueueRequestLocked(ERequestType::StartLink, "/api/auth/device/start", requestJson.dump());
		LeaveCriticalSection(&g_state.lock);
		return queued;
	}

	bool SignOut()
	{
		EnsureInitialized();

		EnterCriticalSection(&g_state.lock);
		IntegrateCompletedRequestLocked();
		if (g_state.requestInFlight)
		{
			LeaveCriticalSection(&g_state.lock);
			return false;
		}

		if (!g_state.session.valid && !g_state.pendingLink.active)
		{
			LeaveCriticalSection(&g_state.lock);
			return false;
		}

		const std::string refreshToken = g_state.session.refreshToken;
		ClearSessionLocked();
		if (!refreshToken.empty() && !g_state.requestInFlight)
		{
			Json requestJson;
			requestJson["refreshToken"] = refreshToken;
			QueueRequestLocked(ERequestType::Logout, "/api/auth/logout", requestJson.dump());
		}
		LeaveCriticalSection(&g_state.lock);
		return true;
	}
}

#endif
