/**
 * @file Network_PSP.cpp
 * @brief Phase-1 stub for PSP networking. NET_IsActive returns false; every
 *        socket op fails with -1. Phase 5+ can wire in sceNetAdhoc / sceNetInet
 *        if needed; most homebrew games skip network entirely.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Network/Network.h"
#include "Log.h"

#include <string.h>

void NET_Initialize() { LogDebug("Network_PSP: stub init (no network)"); }
void NET_Shutdown()   {}
void NET_Update()     {}

bool NET_IsActive() { return false; }

SocketHandle NET_SocketCreate() { return -1; }
void NET_SocketBind(SocketHandle /*s*/, uint32_t /*ip*/, uint16_t /*port*/) {}
int32_t NET_SocketRecv(SocketHandle /*s*/, char* /*buf*/, uint32_t /*size*/) { return -1; }
int32_t NET_SocketRecvFrom(SocketHandle /*s*/, char* /*buf*/, uint32_t /*size*/, uint32_t& /*addr*/, uint16_t& /*port*/) { return -1; }
int32_t NET_SocketSendTo(SocketHandle /*s*/, const char* /*buf*/, uint32_t /*size*/, uint32_t /*addr*/, uint16_t /*port*/) { return -1; }
void NET_SocketClose(SocketHandle /*s*/) {}
void NET_SocketSetBlocking(SocketHandle /*s*/, bool /*blocking*/) {}
void NET_SocketSetBroadcast(SocketHandle /*s*/, bool /*broadcast*/) {}
void NET_SocketGetIpAndPort(SocketHandle /*s*/, uint32_t& outIp, uint16_t& outPort) { outIp = 0; outPort = 0; }

SocketHandle NET_SocketCreateStream() { return -1; }
bool         NET_SocketConnect(SocketHandle /*s*/, uint32_t /*ip*/, uint16_t /*port*/, int32_t /*timeoutMs*/) { return false; }
int32_t      NET_SocketSend(SocketHandle /*s*/, const char* /*buf*/, uint32_t /*size*/) { return -1; }

uint32_t NET_ResolveHost(const char* /*hostname*/) { return 0; }

uint32_t NET_IpStringToUint32(const char* /*ipString*/) { return 0; }
void NET_IpUint32ToString(uint32_t /*ip*/, char* outIpString) { if (outIpString) outIpString[0] = '\0'; }

uint32_t NET_GetIpAddress() { return 0; }
uint32_t NET_GetSubnetMask() { return 0; }

#endif // POLYPHASE_PLATFORM_ADDON
