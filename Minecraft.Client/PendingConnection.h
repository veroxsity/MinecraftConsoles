#pragma once
#include "../Minecraft.World/PacketListener.h"
#include <string>
class MinecraftServer;
class Socket;
class LoginPacket;
class Connection;
class Random;
using namespace std;

class PendingConnection : public PacketListener
{
private:
	static const int FAKE_LAG = 0;
	static const int MAX_TICKS_BEFORE_LOGIN = 20 * 30;

	//    public static Logger logger = Logger.getLogger("Minecraft");
	static Random *random;

public:
	Connection *connection;
public:
	bool done;
private:
	MinecraftServer *server;
	int _tick;
	wstring name;
	shared_ptr<LoginPacket> acceptedLogin;
	wstring loginKey;

public:
	PendingConnection(MinecraftServer *server, Socket *socket, const wstring& id);
	~PendingConnection();
	void tick();
	void disconnect(DisconnectPacket::eDisconnectReason reason);
	virtual void handlePreLogin(shared_ptr<PreLoginPacket> packet);
	virtual void handleLogin(shared_ptr<LoginPacket> packet);
	virtual void handleAcceptedLogin(shared_ptr<LoginPacket> packet);
	virtual void onDisconnect(DisconnectPacket::eDisconnectReason reason, void *reasonObjects);
	virtual void handleGetInfo(shared_ptr<GetInfoPacket> packet);
	virtual void handleKeepAlive(shared_ptr<KeepAlivePacket> packet);
	virtual void handleCustomPayload(shared_ptr<CustomPayloadPacket> packet);
	virtual void onUnhandledPacket(shared_ptr<Packet> packet);

#ifdef _WINDOWS64
	// LceLive join ticket state — set by handleCustomPayload, checked in handleLogin.
	bool m_lceLiveTicketPresented = false;
	bool m_lceLiveTicketValid     = false;
	std::string m_lceLiveAccountId;
	std::string m_lceLiveUsername;
	std::string m_lceLiveDisplayName;
#endif
	void send(shared_ptr<Packet> packet);
	wstring getName();
	virtual bool isServerPacketListener();
	virtual bool isDisconnected();

private:
	void sendPreLoginResponse();
};