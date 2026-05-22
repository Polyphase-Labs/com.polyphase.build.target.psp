/**
 * @file Network_PSP.cpp
 * @brief PSP networking layer. Brings up sceNet / sceNetInet / sceNetApctl
 *        on NET_Initialize, auto-connects to the user's WiFi profile #1,
 *        and tears everything down in NET_Shutdown. The engine's HTTP
 *        backend (HttpBackend_PSP.cpp) layers sceHttp/sceHttps on top.
 *
 * Why "auto-connect to profile #1": the PSP XMB has a Network Settings
 * area that stores up to 10 WiFi profiles. Profile #1 is the user's
 * default. PPSSPP fakes an instant connection when its "Enable
 * networking/WLAN" option is on, so dev flow on the emulator needs no
 * extra setup. On real hardware the user just needs ONE WiFi profile
 * configured in XMB before launching the game.
 *
 * Module-load discipline: every sceNet* family lives in a separate PRX
 * that must be sceUtilityLoadNetModule'd before its Init function works.
 * Order matters — INET depends on COMMON, HTTP depends on INET, etc.
 * We load all of them up front so HttpBackend_PSP.cpp can assume the
 * stack is fully ready.
 *
 * Socket API note: the BSD socket NET_* surface (Recv/Send/etc.) is still
 * stubbed. The HTTP backend doesn't need raw sockets — it talks to the
 * higher-level sceHttp service. Filling in real sceNetInet socket wiring
 * is a separate phase (would be needed for Lua TCP/UDP, Adhoc multi-PSP,
 * NetDatum replication, etc.). For now: real HTTP yes, raw sockets later.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Network/Network.h"
#include "Log.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspsdk.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netmodules.h>

// BSD-style sockets API surface. PSPSDK's pspnet_inet.h pulls in
// <sys/socket.h> for sockaddr/socklen_t, but htons/htonl/inet_aton live
// under <arpa/inet.h> and in_addr is in <netinet/in.h>. netdb.h gives us
// gethostbyname (PSPSDK ships a simple implementation that does manual
// UDP DNS via sceNetInetSocket — works on PPSSPP, which UNIMPLs the
// higher-level sceNetResolver* functions but supports raw sockets).
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <string.h>
#include <stdio.h>

namespace
{
    bool sNetUp = false;

    // sceNet wants a memory pool size for its internal buffers. 64 KB is
    // what every PSPSDK example uses and what most homebrew picks; bumping
    // helps if many concurrent HTTP requests are in flight. We're driven
    // by HttpClient's single worker thread (one in-flight request at a
    // time), so 64 KB is plenty.
    constexpr int kNetPoolBytes        = 64 * 1024;
    constexpr int kResolverPoolBytes   = 16 * 1024;

    // Apctl-connect polling interval. The connect handshake (DHCP, etc.)
    // takes a couple seconds on real hardware; PPSSPP completes it
    // immediately. 100 ms ticks let us bail fast on PPSSPP without
    // burning CPU on real hardware.
    constexpr int kApctlPollMs = 100;
    constexpr int kApctlTimeoutMs = 15 * 1000;  // 15 s — DHCP can be slow on real PSPs

    // Which profile slot to connect with. PSP supports 1..10; profile 1
    // is the user's default-configured one. Most users only ever set up
    // slot 1. Could be made configurable later via the build profile if
    // someone wants to ship a game that defaults to a specific slot.
    constexpr int kApctlProfileNum = 1;

    bool LoadNetModules()
    {
        // PSP_NET_MODULE_* identifiers; failure of any one means the
        // corresponding sceNet*Init below will return ERROR_NOT_INITIALIZED.
        // The PRX is shared across the system so loading twice is harmless
        // (returns ALREADY_LOADED, which we treat as success).
        struct ModSpec { int id; const char* name; };
        const ModSpec kModules[] = {
            { PSP_NET_MODULE_COMMON,   "common"   },
            { PSP_NET_MODULE_INET,     "inet"     },
            // HTTP-family modules are loaded by the HTTP backend itself
            // to keep the network-layer surface clean of HTTP concerns;
            // a project that only needs raw sockets doesn't pay the
            // module-slot cost for HTTP/HTTPS.
        };
        for (const ModSpec& m : kModules)
        {
            const int rc = sceUtilityLoadNetModule(m.id);
            // 0 = ok; 0x80111102 = "already loaded" which is fine.
            if (rc < 0 && rc != 0x80111102)
            {
                LogError("sceUtilityLoadNetModule(%s) failed: 0x%08X", m.name, rc);
                return false;
            }
        }
        return true;
    }

    void UnloadNetModules()
    {
        // Best-effort unload; ignore failures (the system unloads them
        // on game exit anyway). Order is reverse of load.
        sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    }

    bool ApctlConnectAndWaitForIp(int profileNum, int timeoutMs)
    {
        // Apctl state machine: DISCONNECTED → SCANNING → JOINING →
        // GETTING_IP → GOT_IP (or KEY_REQUIRED / DISCONNECTED on failure).
        // We poll sceNetApctlGetState until we see GOT_IP or run out of
        // time. PPSSPP jumps straight to GOT_IP synchronously when fake
        // networking is enabled; real PSPs take ~3-5 seconds with WPA.
        int rc = sceNetApctlConnect(profileNum);
        if (rc != 0)
        {
            LogError("sceNetApctlConnect(profile=%d) failed: 0x%08X", profileNum, rc);
            return false;
        }

        const int totalTicks = timeoutMs / kApctlPollMs;
        for (int i = 0; i < totalTicks; ++i)
        {
            int state = 0;
            rc = sceNetApctlGetState(&state);
            if (rc != 0)
            {
                LogError("sceNetApctlGetState failed: 0x%08X", rc);
                return false;
            }
            if (state == 4 /* PSP_NET_APCTL_STATE_GOT_IP */)
            {
                return true;
            }
            // 0 = disconnected, 1 = scanning, 2 = joining, 3 = getting IP,
            // 4 = got IP. Anything else (e.g. KEY_REQUIRED) means setup
            // failure — bail rather than spin forever.
            sceKernelDelayThread(kApctlPollMs * 1000);
        }
        LogError("Apctl connect timed out after %d ms (profile=%d)", timeoutMs, profileNum);
        sceNetApctlDisconnect();
        return false;
    }
} // namespace

void NET_Initialize()
{
    if (sNetUp)
    {
        LogDebug("Network_PSP: already initialized");
        return;
    }

    if (!LoadNetModules())
    {
        LogError("Network_PSP: module load failed; network disabled");
        return;
    }

    // sceNetInit takes (poolSize, calloutPrio, calloutStackSize, netintrPrio,
    // netintrStackSize). The default priority (0x20) collides with nothing
    // we run on the audio mixer (0x12) or main thread (0x11), so this is
    // safe to leave alone. Stack sizes of 0x1000 (4 KB) each are what every
    // homebrew picks.
    int rc = sceNetInit(kNetPoolBytes, 0x20, 0x1000, 0x20, 0x1000);
    if (rc != 0)
    {
        LogError("sceNetInit failed: 0x%08X", rc);
        UnloadNetModules();
        return;
    }

    rc = sceNetInetInit();
    if (rc != 0)
    {
        LogError("sceNetInetInit failed: 0x%08X", rc);
        sceNetTerm();
        UnloadNetModules();
        return;
    }

    rc = sceNetResolverInit();
    if (rc != 0)
    {
        LogError("sceNetResolverInit failed: 0x%08X", rc);
        sceNetInetTerm();
        sceNetTerm();
        UnloadNetModules();
        return;
    }

    rc = sceNetApctlInit(0x1800 /* stack size */, 0x30 /* priority */);
    if (rc != 0)
    {
        LogError("sceNetApctlInit failed: 0x%08X", rc);
        sceNetResolverTerm();
        sceNetInetTerm();
        sceNetTerm();
        UnloadNetModules();
        return;
    }

    // Auto-connect to the user's WiFi profile. If the user has no profile
    // configured (common on a brand-new PSP) Apctl reports a failure and
    // we leave the stack partially initialized — the HTTP backend will
    // see NET_IsActive()==false and report Unavailable to callers.
    if (!ApctlConnectAndWaitForIp(kApctlProfileNum, kApctlTimeoutMs))
    {
        LogWarning("Network_PSP: WiFi profile #%d unavailable; net stack up but offline",
                   kApctlProfileNum);
        // Don't tear down — leave the stack initialized so a future
        // manual Reconnect API (TODO) can retry. NET_IsActive returns
        // false until GOT_IP is reached.
        sNetUp = true;
        return;
    }

    sNetUp = true;
    LogDebug("Network_PSP: online (profile #%d connected)", kApctlProfileNum);
}

void NET_Shutdown()
{
    if (!sNetUp) return;
    sceNetApctlDisconnect();
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    UnloadNetModules();
    sNetUp = false;
}

void NET_Update() {}

bool NET_IsActive()
{
    if (!sNetUp) return false;
    int state = 0;
    if (sceNetApctlGetState(&state) != 0) return false;
    return state == 4 /* GOT_IP */;
}

// =========================================================================
// Raw socket surface — still stubbed (Phase: defer to a separate "PSP raw
// sockets" task if Lua TCP/UDP / NetDatum replication becomes needed). The
// HTTP backend doesn't need any of these — it goes through sceHttp.
// =========================================================================

// =========================================================================
// TCP socket surface — real implementation via sceNetInet (BSD-ish API).
// Engine's HttpBackend_PSP uses these to run raw TCP / mbedTLS directly
// instead of going through PSPSDK's libcurl (which was buggy in practice).
// Datagram / SendTo / RecvFrom remain stubbed — not used yet by any
// engine system.
// =========================================================================

namespace
{
    // Single, lazily-created resolver context for NET_ResolveHost.
    // PSP's sceNetResolverCreate wants a memory pool; we keep one alive
    // across calls so we don't pay for the alloc/free per resolve.
    int  sResolverId = -1;
    char sResolverBuf[kResolverPoolBytes];

    int EnsureResolver()
    {
        if (sResolverId >= 0) return sResolverId;
        int id = -1;
        const int rc = sceNetResolverCreate(&id, sResolverBuf, sizeof(sResolverBuf));
        if (rc < 0)
        {
            LogError("sceNetResolverCreate failed: 0x%08X", rc);
            return -1;
        }
        sResolverId = id;
        return sResolverId;
    }
}

SocketHandle NET_SocketCreate() { return -1; }
void NET_SocketBind(SocketHandle /*s*/, uint32_t /*ip*/, uint16_t /*port*/) {}
int32_t NET_SocketRecvFrom(SocketHandle /*s*/, char* /*buf*/, uint32_t /*size*/, uint32_t& /*addr*/, uint16_t& /*port*/) { return -1; }
int32_t NET_SocketSendTo(SocketHandle /*s*/, const char* /*buf*/, uint32_t /*size*/, uint32_t /*addr*/, uint16_t /*port*/) { return -1; }
void NET_SocketSetBlocking(SocketHandle /*s*/, bool /*blocking*/) {}    // Not needed for blocking-IO TLS transport
void NET_SocketSetBroadcast(SocketHandle /*s*/, bool /*broadcast*/) {}
void NET_SocketGetIpAndPort(SocketHandle /*s*/, uint32_t& outIp, uint16_t& outPort) { outIp = 0; outPort = 0; }

int32_t NET_SocketRecv(SocketHandle s, char* buf, uint32_t size)
{
    if (s < 0 || buf == nullptr || size == 0) return -1;

    // Retry on EAGAIN/EWOULDBLOCK. PPSSPP's sceNetInetRecv was observed to
    // return -1 with errno=11 (EAGAIN) immediately if no data is buffered
    // host-side yet, even on a "blocking" socket — the host syscall is
    // effectively non-blocking. Real PSP firmware blocks. We poll-spin
    // here with a tiny yield so both behave the same to mbedTLS / our
    // HTTP reader, which expect blocking semantics. Any other negative
    // errno is a real error.
    for (;;)
    {
        const int n = (int)sceNetInetRecv(s, buf, size, 0);
        if (n >= 0) return (int32_t)n;
        const int err = sceNetInetGetErrno();
        // EAGAIN=11, EWOULDBLOCK=11 (or 140 on some BSD-isms). Retry.
        if (err == 11 || err == 140)
        {
            sceKernelDelayThread(1000); // 1ms yield
            continue;
        }
        LogWarning("NET_SocketRecv: sceNetInetRecv rc=%d errno=%d", n, err);
        return -1;
    }
}

void NET_SocketClose(SocketHandle s)
{
    if (s < 0) return;
    sceNetInetClose(s);
}

SocketHandle NET_SocketCreateStream()
{
    const int s = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
    {
        LogError("sceNetInetSocket failed: 0x%08X (errno=%d)", s, sceNetInetGetErrno());
        return -1;
    }
    return s;
}

bool NET_SocketConnect(SocketHandle s, uint32_t ipAddr, uint16_t port, int32_t /*timeoutMs*/)
{
    if (s < 0) return false;

    // sceNetInetConnect is blocking; the OS-level connect timeout (handful
    // of seconds) applies. For tight per-request timeouts a non-blocking
    // connect + select would be needed — can layer on later if needed.
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(ipAddr);

    const int rc = sceNetInetConnect(s, (sockaddr*)&addr, sizeof(addr));
    if (rc < 0)
    {
        LogError("sceNetInetConnect failed: 0x%08X (errno=%d)", rc, sceNetInetGetErrno());
        return false;
    }
    return true;
}

int32_t NET_SocketSend(SocketHandle s, const char* buf, uint32_t size)
{
    if (s < 0 || buf == nullptr || size == 0) return -1;
    return (int32_t)sceNetInetSend(s, buf, size, 0);
}

uint32_t NET_ResolveHost(const char* hostname)
{
    if (hostname == nullptr || *hostname == '\0') return 0;

    // Literal-IP fast path — manual dotted-quad parse. PSPSDK's inet_aton
    // calls sceNetInetInetAton which PPSSPP UNIMPLs, so we can't use it.
    // This four-component parse is portable and avoids the libcurl
    // weirdness around literal IPs entirely.
    {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        char tail = 0;
        if (sscanf(hostname, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4
            && a <= 255 && b <= 255 && c <= 255 && d <= 255)
        {
            return (a << 24) | (b << 16) | (c << 8) | d;
        }
    }

    // gethostbyname first — PSPSDK ships a UDP-DNS implementation that
    // works on both real hardware AND PPSSPP (PPSSPP supports the
    // underlying raw sockets even though it UNIMPLs sceNetResolver*).
    if (hostent* he = gethostbyname(hostname))
    {
        if (he->h_addr_list != nullptr && he->h_addr_list[0] != nullptr)
        {
            uint32_t ipBe = 0;
            memcpy(&ipBe, he->h_addr_list[0], sizeof(ipBe));
            return ntohl(ipBe);
        }
    }

    // Fall back to sceNetResolverStartNtoA. Works on real PSP; on PPSSPP
    // this returns failure (UNIMPL'd there) but we've already tried
    // gethostbyname above.
    const int rid = EnsureResolver();
    if (rid < 0) return 0;
    in_addr resolved = {};
    const int rc = sceNetResolverStartNtoA(rid, hostname, &resolved,
                                           5u /*timeout sec*/, 4 /*retries*/);
    if (rc < 0)
    {
        LogWarning("DNS resolve failed for '%s' (gethostbyname + sceNetResolver both failed)", hostname);
        return 0;
    }
    return ntohl(resolved.s_addr);
}

uint32_t NET_IpStringToUint32(const char* ipString)
{
    if (ipString == nullptr) return 0;
    in_addr a = {};
    if (inet_aton(ipString, &a) == 0) return 0;
    return ntohl(a.s_addr);
}

void NET_IpUint32ToString(uint32_t ip, char* outIpString)
{
    if (outIpString == nullptr) return;
    // PSP's uint32_t is `unsigned long`, so use %lu (or cast). %u would warn
    // and on different toolchains might print garbage.
    snprintf(outIpString, 16, "%lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xFF),
             (unsigned long)((ip >> 16) & 0xFF),
             (unsigned long)((ip >>  8) & 0xFF),
             (unsigned long)( ip        & 0xFF));
}

// Pull the device's local IP from Apctl's connection-info table.
uint32_t NET_GetIpAddress()
{
    if (!sNetUp) return 0;
    union { uint32_t u32; uint8_t b[4]; } addr = {};
    // PSP_NET_APCTL_INFO_IP = 8: returns "xxx.xxx.xxx.xxx" as a 16-byte string.
    SceNetApctlInfo info;
    if (sceNetApctlGetInfo(8 /* IP */, &info) != 0) return 0;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (sscanf(info.ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    addr.b[0] = (uint8_t)a; addr.b[1] = (uint8_t)b; addr.b[2] = (uint8_t)c; addr.b[3] = (uint8_t)d;
    return addr.u32;
}

uint32_t NET_GetSubnetMask()
{
    if (!sNetUp) return 0;
    union { uint32_t u32; uint8_t b[4]; } mask = {};
    SceNetApctlInfo info;
    if (sceNetApctlGetInfo(9 /* SUBNET_MASK */, &info) != 0) return 0;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (sscanf(info.subNetMask, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    mask.b[0] = (uint8_t)a; mask.b[1] = (uint8_t)b; mask.b[2] = (uint8_t)c; mask.b[3] = (uint8_t)d;
    return mask.u32;
}

#endif // POLYPHASE_PLATFORM_ADDON
