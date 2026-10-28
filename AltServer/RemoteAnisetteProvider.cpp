#include "RemoteAnisetteProvider.h"

#include <WinSock2.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>

#include <cpprest/http_client.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <plist/plist.h>

#include "AnisetteData.h"
#include "AltServerApp.h"

#pragma comment(lib, "winhttp.lib")

using namespace web;
using namespace web::http;
using namespace web::http::client;

namespace fs = std::filesystem;

extern std::string make_uuid();
extern std::string StringFromWideString(std::wstring wideString);
extern std::wstring WideStringFromString(std::string string);

#define odslog(msg) { std::stringstream ss; ss << msg << std::endl; OutputDebugStringA(ss.str().c_str()); }

// Same servers (in the same order) iPASide uses, all of which generate anisette data on a Mac.
static const std::vector<std::string> ALTDefaultAnisetteServers = {
	"https://mac-ani.jkcoxson.com",
	"https://new.ani.sidestore.io",
	"https://ani3.stikstore.app",
};

static const utility::string_t ALTStartProvisioningPath = U("/grandslam/MidService/startMachineProvisioning");
static const utility::string_t ALTFinishProvisioningPath = U("/grandslam/MidService/finishMachineProvisioning");

namespace
{
	class WinHttpHandle
	{
	public:
		WinHttpHandle(HINTERNET handle = NULL) : _handle(handle) {}
		~WinHttpHandle() { this->reset(NULL); }

		WinHttpHandle(const WinHttpHandle&) = delete;
		WinHttpHandle& operator=(const WinHttpHandle&) = delete;

		HINTERNET get() const { return _handle; }
		explicit operator bool() const { return _handle != NULL; }

		void reset(HINTERNET handle)
		{
			if (_handle != NULL)
			{
				WinHttpCloseHandle(_handle);
			}

			_handle = handle;
		}

	private:
		HINTERNET _handle;
	};

	[[noreturn]] void ThrowWinHttpError(std::string operation, DWORD error = GetLastError())
	{
		std::stringstream ss;
		ss << operation << " failed (WinHTTP error " << error << ").";
		throw RemoteAnisetteError((int)error, ss.str());
	}

	struct WinHttpConnection
	{
		WinHttpHandle session;
		WinHttpHandle connection;
		bool isSecure = false;
	};

	std::unique_ptr<WinHttpConnection> OpenConnection(const uri& serverURI)
	{
		auto connection = std::make_unique<WinHttpConnection>();
		connection->isSecure = (serverURI.scheme() == U("https"));

		INTERNET_PORT port = (serverURI.port() > 0) ? (INTERNET_PORT)serverURI.port() : (connection->isSecure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

		connection->session.reset(WinHttpOpen(L"AltServer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
		if (!connection->session)
		{
			ThrowWinHttpError("Opening WinHTTP session");
		}

		WinHttpSetTimeouts(connection->session.get(), 30000, 30000, 30000, 30000);

		connection->connection.reset(WinHttpConnect(connection->session.get(), serverURI.host().c_str(), port, 0));
		if (!connection->connection)
		{
			ThrowWinHttpError("Connecting to " + StringFromWideString(serverURI.host()));
		}

		return connection;
	}

	// Uses WinHTTP directly rather than cpprestsdk, because cpprestsdk requires checking certificate revocation,
	// which fails on some PCs (WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED) even though the certificates are trusted.
	std::string SendHTTPRequest(std::string url, const wchar_t* method, const std::map<utility::string_t, utility::string_t>& headers, const std::string& body, bool validateCertificateAuthority = true)
	{
		uri requestURI(WideStringFromString(url));
		auto connection = OpenConnection(requestURI);

		auto resource = requestURI.resource().to_string();

		WinHttpHandle request(WinHttpOpenRequest(connection->connection.get(), method, resource.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, connection->isSecure ? WINHTTP_FLAG_SECURE : 0));
		if (!request)
		{
			ThrowWinHttpError("Creating request to " + url);
		}

		if (!validateCertificateAuthority)
		{
			DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
			WinHttpSetOption(request.get(), WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
		}

		std::wstring headerLines;
		for (auto& pair : headers)
		{
			headerLines += pair.first + L": " + pair.second + L"\r\n";
		}

		if (!WinHttpSendRequest(request.get(), headerLines.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerLines.c_str(), headerLines.empty() ? 0 : (DWORD)-1L,
								body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
			|| !WinHttpReceiveResponse(request.get(), NULL))
		{
			ThrowWinHttpError("Sending request to " + url);
		}

		DWORD statusCode = 0;
		DWORD statusCodeSize = sizeof(statusCode);
		WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

		std::string responseBody;
		while (true)
		{
			DWORD availableBytes = 0;
			if (!WinHttpQueryDataAvailable(request.get(), &availableBytes))
			{
				ThrowWinHttpError("Reading response from " + url);
			}

			if (availableBytes == 0)
			{
				break;
			}

			std::vector<char> buffer(availableBytes);

			DWORD bytesRead = 0;
			if (!WinHttpReadData(request.get(), buffer.data(), availableBytes, &bytesRead))
			{
				ThrowWinHttpError("Reading response from " + url);
			}

			responseBody.append(buffer.data(), bytesRead);
		}

		if (statusCode < 200 || statusCode >= 300)
		{
			std::stringstream ss;
			ss << url << " returned HTTP " << statusCode << ".";
			throw RemoteAnisetteError((int)statusCode, ss.str());
		}

		return responseBody;
	}

	json::value ReceiveWebSocketMessage(HINTERNET webSocket)
	{
		std::string message;
		char buffer[4096];

		while (true)
		{
			DWORD bytesRead = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;

			DWORD error = WinHttpWebSocketReceive(webSocket, buffer, sizeof(buffer), &bytesRead, &bufferType);
			if (error != NO_ERROR)
			{
				ThrowWinHttpError("Receiving message from anisette server", error);
			}

			if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
			{
				throw RemoteAnisetteError(0, "The anisette server closed the connection unexpectedly.");
			}

			message.append(buffer, bytesRead);

			if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
			{
				break;
			}
		}

		return json::value::parse(WideStringFromString(message));
	}

	void SendWebSocketMessage(HINTERNET webSocket, const json::value& message)
	{
		auto text = StringFromWideString(message.serialize());

		DWORD error = WinHttpWebSocketSend(webSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(), (DWORD)text.size());
		if (error != NO_ERROR)
		{
			ThrowWinHttpError("Sending message to anisette server", error);
		}
	}

	std::string ClientTime()
	{
		time_t now = time(NULL);

		struct tm utc;
		gmtime_s(&utc, &now);

		char dateString[64];
		strftime(dateString, sizeof(dateString), "%Y-%m-%dT%H:%M:%SZ", &utc);
		return dateString;
	}

	std::optional<std::string> PlistString(plist_t dictionary, const char* key)
	{
		auto node = plist_dict_get_item(dictionary, key);
		if (node == nullptr || plist_get_node_type(node) != PLIST_STRING)
		{
			return std::nullopt;
		}

		char* value = nullptr;
		plist_get_string_val(node, &value);

		std::string string(value);
		free(value);
		return string;
	}

	int64_t PlistErrorCode(plist_t statusDictionary)
	{
		auto node = plist_dict_get_item(statusDictionary, "ec");
		if (node == nullptr)
		{
			return 0;
		}

		switch (plist_get_node_type(node))
		{
		case PLIST_UINT:
		{
			uint64_t value = 0;
			plist_get_uint_val(node, &value);
			return (int64_t)value;
		}

		case PLIST_STRING:
		{
			char* value = nullptr;
			plist_get_string_val(node, &value);

			int64_t code = atoll(value);
			free(value);
			return code;
		}

		default: return 0;
		}
	}

	// Sends a machine provisioning request to Apple, returning the "Response" dictionary. Takes ownership of requestDictionary.
	plist_t SendProvisioningRequest(utility::string_t path, plist_t requestDictionary, const std::map<utility::string_t, utility::string_t>& headers)
	{
		auto plist = plist_new_dict();
		plist_dict_set_item(plist, "Header", plist_new_dict());
		plist_dict_set_item(plist, "Request", requestDictionary);

		char* plistXML = nullptr;
		uint32_t length = 0;
		plist_to_xml(plist, &plistXML, &length);

		std::string body(plistXML, length);
		free(plistXML);
		plist_free(plist);

		auto requestHeaders = headers;
		requestHeaders[U("Content-Type")] = U("text/x-xml-plist");

		// Like AltSign's GSA requests, don't require Windows to trust Apple's certificate authority.
		auto data = SendHTTPRequest("https://gsa.apple.com" + StringFromWideString(path), L"POST", requestHeaders, body, false);

		plist_t responsePlist = nullptr;
		plist_from_xml(data.data(), (uint32_t)data.size(), &responsePlist);

		auto responseDictionary = (responsePlist != nullptr) ? plist_dict_get_item(responsePlist, "Response") : nullptr;
		if (responseDictionary == nullptr)
		{
			if (responsePlist != nullptr)
			{
				plist_free(responsePlist);
			}

			throw RemoteAnisetteError(0, "Apple returned an invalid machine provisioning response.");
		}

		responseDictionary = plist_copy(responseDictionary);
		plist_free(responsePlist);

		auto statusDictionary = plist_dict_get_item(responseDictionary, "Status");
		auto errorCode = (statusDictionary != nullptr) ? PlistErrorCode(statusDictionary) : 0;
		if (errorCode != 0)
		{
			auto errorMessage = PlistString(statusDictionary, "em");
			plist_free(responseDictionary);

			std::stringstream ss;
			ss << "Apple could not provision this PC: " << errorMessage.value_or("Unknown error") << " (" << errorCode << ")";
			throw RemoteAnisetteError((int)errorCode, ss.str());
		}

		return responseDictionary;
	}
}

RemoteAnisetteProvider* RemoteAnisetteProvider::_instance = nullptr;

RemoteAnisetteProvider* RemoteAnisetteProvider::instance()
{
	if (_instance == 0)
	{
		_instance = new RemoteAnisetteProvider();
	}

	return _instance;
}

RemoteAnisetteProvider::RemoteAnisetteProvider()
{
}

bool RemoteAnisetteProvider::isEnabled()
{
	auto mode = std::getenv("ALTSERVER_ANISETTE");
	return (mode == nullptr || std::string(mode) != "local");
}

std::vector<std::string> RemoteAnisetteProvider::serverURLs() const
{
	auto serverURL = std::getenv("ALTSERVER_ANISETTE_SERVER");
	if (serverURL != nullptr && std::string(serverURL).length() > 0)
	{
		return { serverURL };
	}

	return ALTDefaultAnisetteServers;
}

std::shared_ptr<AnisetteData> RemoteAnisetteProvider::FetchAnisetteData()
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (_state.is_null())
	{
		this->LoadState();
	}

	std::stringstream errors;

	for (auto& serverURL : this->serverURLs())
	{
		try
		{
			return this->FetchAnisetteData(serverURL);
		}
		catch (Error& error)
		{
			odslog("Failed to fetch anisette data from " << serverURL << ". " << error.localizedDescription());
			errors << "\n\n" << serverURL << ": " << error.localizedDescription();
		}
		catch (std::exception& exception)
		{
			odslog("Failed to fetch anisette data from " << serverURL << ". " << exception.what());
			errors << "\n\n" << serverURL << ": " << exception.what();
		}
	}

	throw RemoteAnisetteError(0, "AltServer could not fetch anisette data from any anisette server." + errors.str());
}

std::shared_ptr<AnisetteData> RemoteAnisetteProvider::FetchAnisetteData(const std::string& serverURL)
{
	auto clientInfoData = SendHTTPRequest(serverURL + "/v3/client_info", L"GET", {}, "");
	auto clientInfoResponse = json::value::parse(WideStringFromString(clientInfoData));
	auto clientInfo = StringFromWideString(clientInfoResponse.at(U("client_info")).as_string());
	auto userAgent = StringFromWideString(clientInfoResponse.at(U("user_agent")).as_string());

	auto serverKey = WideStringFromString(serverURL);
	auto identifier = utility::conversions::to_base64(this->identifier());

	// Provision again if the anisette server rejects our existing provisioning data.
	for (int attempt = 0; attempt < 2; attempt++)
	{
		std::string adiPB;
		if (_state.at(U("servers")).has_field(serverKey))
		{
			adiPB = StringFromWideString(_state.at(U("servers")).at(serverKey).as_string());
		}
		else
		{
			odslog("Provisioning PC with anisette server " << serverURL << "...");

			adiPB = this->Provision(serverURL, clientInfo, userAgent);

			_state[U("servers")][serverKey] = json::value::string(WideStringFromString(adiPB));
			this->SaveState();
		}

		auto body = json::value::object();
		body[U("identifier")] = json::value::string(identifier);
		body[U("adi_pb")] = json::value::string(WideStringFromString(adiPB));

		auto headersData = SendHTTPRequest(serverURL + "/v3/get_headers", L"POST", { { U("Content-Type"), U("application/json") } }, StringFromWideString(body.serialize()));
		auto headers = json::value::parse(WideStringFromString(headersData));

		if (headers.has_field(U("result")) && headers.at(U("result")).as_string() == U("Headers"))
		{
			auto routingInfoValue = headers.at(U("X-Apple-I-MD-RINFO"));
			unsigned long long routingInfo = routingInfoValue.is_number() ? routingInfoValue.as_number().to_uint64() : std::stoull(StringFromWideString(routingInfoValue.as_string()));

			TIMEVAL date;
			date.tv_sec = (long)time(NULL);
			date.tv_usec = 0;

			auto anisetteData = std::make_shared<AnisetteData>(
				StringFromWideString(headers.at(U("X-Apple-I-MD-M")).as_string()),
				StringFromWideString(headers.at(U("X-Apple-I-MD")).as_string()),
				this->localUserID(),
				routingInfo,
				this->deviceID(),
				"0",
				clientInfo,
				date,
				"en_US",
				"UTC");

			odslog("Fetched anisette data from " << serverURL << ":\n" << *anisetteData);

			return anisetteData;
		}

		odslog("Anisette server " << serverURL << " rejected provisioning data: " << StringFromWideString(headers.serialize()));

		_state[U("servers")].erase(serverKey);
		this->SaveState();
	}

	throw RemoteAnisetteError(0, "The anisette server rejected this PC's provisioning data.");
}

std::string RemoteAnisetteProvider::Provision(const std::string& serverURL, const std::string& clientInfo, const std::string& userAgent)
{
	uri serverURI(WideStringFromString(serverURL));
	auto connection = OpenConnection(serverURI);

	auto path = serverURI.path();
	if (!path.empty() && path.back() == U('/'))
	{
		path.pop_back();
	}
	path += U("/v3/provisioning_session");

	WinHttpHandle request(WinHttpOpenRequest(connection->connection.get(), L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, connection->isSecure ? WINHTTP_FLAG_SECURE : 0));
	if (!request)
	{
		ThrowWinHttpError("Creating provisioning session request");
	}

	if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0))
	{
		ThrowWinHttpError("Requesting WebSocket upgrade");
	}

	if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), NULL))
	{
		ThrowWinHttpError("Starting provisioning session");
	}

	DWORD statusCode = 0;
	DWORD statusCodeSize = sizeof(statusCode);
	WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

	if (statusCode != HTTP_STATUS_SWITCH_PROTOCOLS)
	{
		std::stringstream ss;
		ss << "The anisette server refused the provisioning session (HTTP " << statusCode << ").";
		throw RemoteAnisetteError((int)statusCode, ss.str());
	}

	WinHttpHandle webSocket(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
	if (!webSocket)
	{
		ThrowWinHttpError("Completing WebSocket upgrade");
	}

	request.reset(NULL);

	std::map<utility::string_t, utility::string_t> appleHeaders = {
		{ U("Accept"), U("*/*") },
		{ U("User-Agent"), WideStringFromString(userAgent) },
		{ U("X-Mme-Client-Info"), WideStringFromString(clientInfo) },
		{ U("X-Mme-Device-Id"), WideStringFromString(this->deviceID()) },
		{ U("X-Apple-I-MD-LU"), WideStringFromString(this->localUserID()) },
		{ U("X-Apple-I-Client-Time"), WideStringFromString(ClientTime()) },
		{ U("X-Apple-I-TimeZone"), U("UTC") },
		{ U("X-Apple-Locale"), U("en_US") },
	};

	while (true)
	{
		auto message = ReceiveWebSocketMessage(webSocket.get());
		auto result = message.has_field(U("result")) ? message.at(U("result")).as_string() : U("");

		odslog("Anisette server: " << StringFromWideString(result));

		if (result == U("GiveIdentifier"))
		{
			auto response = json::value::object();
			response[U("identifier")] = json::value::string(utility::conversions::to_base64(this->identifier()));
			SendWebSocketMessage(webSocket.get(), response);
		}
		else if (result == U("GiveStartProvisioningData"))
		{
			auto responseDictionary = SendProvisioningRequest(ALTStartProvisioningPath, plist_new_dict(), appleHeaders);
			auto spim = PlistString(responseDictionary, "spim");
			plist_free(responseDictionary);

			if (!spim.has_value())
			{
				throw RemoteAnisetteError(0, "Apple's machine provisioning response is missing 'spim'.");
			}

			auto response = json::value::object();
			response[U("spim")] = json::value::string(WideStringFromString(*spim));
			SendWebSocketMessage(webSocket.get(), response);
		}
		else if (result == U("GiveEndProvisioningData"))
		{
			auto cpim = StringFromWideString(message.at(U("cpim")).as_string());

			auto requestDictionary = plist_new_dict();
			plist_dict_set_item(requestDictionary, "cpim", plist_new_string(cpim.c_str()));

			auto responseDictionary = SendProvisioningRequest(ALTFinishProvisioningPath, requestDictionary, appleHeaders);
			auto ptm = PlistString(responseDictionary, "ptm");
			auto tk = PlistString(responseDictionary, "tk");
			plist_free(responseDictionary);

			if (!ptm.has_value() || !tk.has_value())
			{
				throw RemoteAnisetteError(0, "Apple's machine provisioning response is missing 'ptm' or 'tk'.");
			}

			auto response = json::value::object();
			response[U("ptm")] = json::value::string(WideStringFromString(*ptm));
			response[U("tk")] = json::value::string(WideStringFromString(*tk));
			SendWebSocketMessage(webSocket.get(), response);
		}
		else if (result == U("ProvisioningSuccess"))
		{
			auto adiPB = StringFromWideString(message.at(U("adi_pb")).as_string());
			WinHttpWebSocketClose(webSocket.get(), WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
			return adiPB;
		}
		else
		{
			std::stringstream ss;
			ss << "The anisette server could not provision this PC (" << StringFromWideString(message.serialize()) << ").";
			throw RemoteAnisetteError(0, ss.str());
		}
	}
}

void RemoteAnisetteProvider::ResetProvisioning()
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (_state.is_null())
	{
		this->LoadState();
	}

	_state[U("servers")] = json::value::object();
	this->SaveState();
}

std::vector<unsigned char> RemoteAnisetteProvider::identifier()
{
	if (_state.has_field(U("identifier")))
	{
		return utility::conversions::from_base64(_state.at(U("identifier")).as_string());
	}

	std::vector<unsigned char> identifier(16);
	if (RAND_bytes(identifier.data(), (int)identifier.size()) != 1)
	{
		throw RemoteAnisetteError(0, "Could not generate anisette identifier.");
	}

	_state[U("identifier")] = json::value::string(utility::conversions::to_base64(identifier));
	this->SaveState();

	return identifier;
}

std::string RemoteAnisetteProvider::deviceID()
{
	if (_state.has_field(U("deviceID")))
	{
		return StringFromWideString(_state.at(U("deviceID")).as_string());
	}

	auto deviceID = make_uuid();
	std::transform(deviceID.begin(), deviceID.end(), deviceID.begin(), [](unsigned char c) { return std::toupper(c); });

	_state[U("deviceID")] = json::value::string(WideStringFromString(deviceID));
	this->SaveState();

	return deviceID;
}

std::string RemoteAnisetteProvider::localUserID()
{
	auto identifier = this->identifier();

	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(identifier.data(), identifier.size(), digest);

	std::stringstream ss;
	for (auto byte : digest)
	{
		ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
	}

	return ss.str();
}

void RemoteAnisetteProvider::LoadState()
{
	auto statePath = AltServerApp::instance()->appDataDirectoryPath().append("Anisette.json");

	if (fs::exists(statePath))
	{
		try
		{
			std::ifstream file(statePath);

			std::stringstream contents;
			contents << file.rdbuf();

			_state = json::value::parse(WideStringFromString(contents.str()));
		}
		catch (std::exception& exception)
		{
			odslog("Failed to load anisette state. " << exception.what());
		}
	}

	if (!_state.is_object())
	{
		_state = json::value::object();
	}

	if (!_state.has_field(U("servers")) || !_state.at(U("servers")).is_object())
	{
		_state[U("servers")] = json::value::object();
	}
}

void RemoteAnisetteProvider::SaveState()
{
	auto statePath = AltServerApp::instance()->appDataDirectoryPath().append("Anisette.json");

	std::ofstream file(statePath, std::ios::out | std::ios::trunc);
	file << StringFromWideString(_state.serialize());
}
