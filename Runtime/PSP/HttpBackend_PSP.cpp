/**
 * @file HttpBackend_PSP.cpp
 * @brief PSP HTTP/HTTPS backend — raw sceNetInet + direct mbedTLS.
 *
 * Mirrors the HttpBackend_Dolphin.cpp pattern (Wii/GameCube): we drive
 * the TCP socket via engine NET_* primitives (themselves backed by
 * sceNetInet on PSP), and run TLS via raw mbedTLS calls — NOT through
 * PSPSDK's libcurl. Two reasons:
 *
 *   - PSPSDK's prebuilt libcurl was observed to corrupt internal state
 *     on PSP (visible as garbage upload-counter numbers in verbose mode
 *     and wild-pointer crashes in pthread workers during response
 *     handling). Going around it removes a whole class of bugs.
 *   - Direct mbedTLS lets us load the engine-vendored CA bundle
 *     (Engine/CACerts/cacert.pem) the same way Dolphin does, and gives
 *     us control over which TLS modes/ciphers we actually use.
 *
 * Entropy: PSPSDK's mbedTLS ships without any registered entropy
 * source (no MBEDTLS_ENTROPY_HARDWARE_ALT, no platform entropy). We
 * override mbedtls_entropy_func directly via --allow-multiple-definition
 * in the Makefile, mixing sceRtc and sceKernel timing for dev-grade
 * entropy. See [[project-psp-mbedtls-entropy]] for context.
 *
 * The HTTP/1.1 framing — request build, response read, header parse,
 * chunked-decode — is identical to the Dolphin code on purpose. Keeping
 * the two backends shape-aligned makes platform parity easier as we
 * add more consoles.
 */

#if PLATFORM_PSP

#include "Network/Http/Backends/HttpBackend.h"
#include "Network/Network.h"
#include "Stream.h"
#include "Log.h"
#include "System/System.h"      // SYS_GetPolyphasePath — PSP runtime root

#include <pspkernel.h>
#include <psprtc.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>

#include <atomic>
#include <memory>
#include <string.h>
#include <strings.h>   // strncasecmp (POSIX, not standard <string.h>)
#include <ctype.h>
#include <string>
#include <vector>

#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED   -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED   -0x004C
#endif

// ---------------------------------------------------------------------------
// mbedTLS entropy override. PSPSDK's libmbedcrypto.a has zero registered
// entropy sources, so its `mbedtls_entropy_func` returns failure for every
// caller. Our definition wins thanks to -Wl,--allow-multiple-definition in
// the PSP Makefile. See [[project-psp-mbedtls-entropy]].
// ---------------------------------------------------------------------------
namespace
{
    void PspGenerateEntropy(unsigned char* output, size_t len)
    {
        static unsigned int sCounter = 0;
        size_t filled = 0;
        while (filled < len)
        {
            u64 tick = 0;
            sceRtcGetCurrentTick(&tick);
            const u64 wide = sceKernelGetSystemTimeWide();

            volatile int spin = 0;
            for (int i = 0; i < 23; ++i) ++spin;

            u64 tick2 = 0;
            sceRtcGetCurrentTick(&tick2);

            const u64 mix = tick ^ (wide << 13) ^ (tick2 << 27)
                              ^ ((u64)(++sCounter) << 41);

            const size_t chunk = (len - filled) < sizeof(mix) ? (len - filled) : sizeof(mix);
            memcpy(output + filled, &mix, chunk);
            filled += chunk;
        }
    }
}

extern "C" int mbedtls_entropy_func(void* /*data*/,
                                    unsigned char* output,
                                    size_t len)
{
    PspGenerateEntropy(output, len);
    return 0;
}

extern "C" int mbedtls_hardware_poll(void* /*data*/,
                                     unsigned char* output,
                                     size_t len,
                                     size_t* olen)
{
    PspGenerateEntropy(output, len);
    if (olen != nullptr) *olen = len;
    return 0;
}

namespace
{
    // -------------------------------------------------------------------
    // URL parsing — no malloc, no exceptions, no regex.
    // -------------------------------------------------------------------
    struct ParsedUrl
    {
        std::string scheme;            // "http" or "https"
        std::string host;
        uint16_t    port = 0;
        std::string pathAndQuery;      // "/foo?bar"
        bool        valid = false;
    };

    ParsedUrl ParseUrl(const std::string& url)
    {
        ParsedUrl out;

        size_t i = url.find("://");
        if (i == std::string::npos) return out;

        out.scheme = url.substr(0, i);
        for (char& c : out.scheme) c = (char)tolower((unsigned char)c);

        const size_t hostStart = i + 3;
        size_t pathStart = url.find('/', hostStart);
        if (pathStart == std::string::npos)
        {
            out.host = url.substr(hostStart);
            out.pathAndQuery = "/";
        }
        else
        {
            out.host = url.substr(hostStart, pathStart - hostStart);
            out.pathAndQuery = url.substr(pathStart);
        }

        // Optional :port suffix
        size_t portColon = out.host.rfind(':');
        if (portColon != std::string::npos && out.host.find(']') == std::string::npos)
        {
            const std::string portStr = out.host.substr(portColon + 1);
            out.host = out.host.substr(0, portColon);
            out.port = (uint16_t)atoi(portStr.c_str());
        }
        if (out.port == 0)
        {
            out.port = (out.scheme == "https") ? 443 : 80;
        }

        out.valid = !out.host.empty();
        return out;
    }

    // -------------------------------------------------------------------
    // Transport abstraction. Plain TCP and TLS share an interface so the
    // HTTP framing code is identical for HTTP and HTTPS.
    // -------------------------------------------------------------------
    class ITransport
    {
    public:
        virtual ~ITransport() = default;
        virtual bool Connect(const ParsedUrl& url, int timeoutMs) = 0;
        virtual int  Send(const char* data, size_t size) = 0;
        virtual int  Recv(char* buffer, size_t size) = 0;
        virtual void Close() = 0;
    };

    class TcpTransport : public ITransport
    {
    public:
        bool Connect(const ParsedUrl& url, int timeoutMs) override
        {
            const uint32_t ip = NET_ResolveHost(url.host.c_str());
            if (ip == 0)
            {
                LogError("PSP HTTP: DNS resolve failed for '%s'", url.host.c_str());
                return false;
            }
            mSock = NET_SocketCreateStream();
            if (mSock < 0) return false;
            if (!NET_SocketConnect(mSock, ip, url.port, timeoutMs))
            {
                NET_SocketClose(mSock); mSock = -1;
                return false;
            }
            return true;
        }
        int Send(const char* data, size_t size) override
        {
            return NET_SocketSend(mSock, data, (uint32_t)size);
        }
        int Recv(char* buffer, size_t size) override
        {
            return NET_SocketRecv(mSock, buffer, (uint32_t)size);
        }
        void Close() override
        {
            if (mSock >= 0) { NET_SocketClose(mSock); mSock = -1; }
        }
    private:
        SocketHandle mSock = -1;
    };

    // mbedTLS BIO callbacks — bridge between mbedTLS and our NET_Socket*.
    int MbedNetSend(void* ctx, const unsigned char* buf, size_t len)
    {
        SocketHandle h = (SocketHandle)(intptr_t)ctx;
        const int n = NET_SocketSend(h, (const char*)buf, (uint32_t)len);
        return n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : n;
    }
    int MbedNetRecv(void* ctx, unsigned char* buf, size_t len)
    {
        SocketHandle h = (SocketHandle)(intptr_t)ctx;
        const int n = NET_SocketRecv(h, (char*)buf, (uint32_t)len);
        return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
    }

    class TlsTransport : public ITransport
    {
    public:
        TlsTransport(mbedtls_x509_crt* caChain,
                     mbedtls_ctr_drbg_context* drbg,
                     bool verify)
            : mCa(caChain), mDrbg(drbg), mVerify(verify)
        {
            mbedtls_ssl_init(&mSsl);
            mbedtls_ssl_config_init(&mCfg);
        }
        ~TlsTransport() override
        {
            Close();
            mbedtls_ssl_free(&mSsl);
            mbedtls_ssl_config_free(&mCfg);
        }

        bool Connect(const ParsedUrl& url, int timeoutMs) override
        {
            const uint32_t ip = NET_ResolveHost(url.host.c_str());
            if (ip == 0)
            {
                LogError("PSP HTTPS: DNS resolve failed for '%s'", url.host.c_str());
                return false;
            }
            mSock = NET_SocketCreateStream();
            if (mSock < 0) return false;
            if (!NET_SocketConnect(mSock, ip, url.port, timeoutMs))
            {
                NET_SocketClose(mSock); mSock = -1; return false;
            }

            if (mbedtls_ssl_config_defaults(&mCfg,
                MBEDTLS_SSL_IS_CLIENT,
                MBEDTLS_SSL_TRANSPORT_STREAM,
                MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            {
                LogError("PSP HTTPS: mbedtls_ssl_config_defaults failed");
                return false;
            }

            // VERIFY_NONE on PSP by default — CA bundle parse may have failed
            // at backend Initialize. Caller can SetVerifySsl(true) explicitly
            // and then we'll insist on real verification (requires the CA
            // bundle to be present and parsed).
            mbedtls_ssl_conf_authmode(&mCfg, mVerify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
            mbedtls_ssl_conf_ca_chain(&mCfg, mCa, nullptr);
            mbedtls_ssl_conf_rng(&mCfg, mbedtls_ctr_drbg_random, mDrbg);

            if (mbedtls_ssl_setup(&mSsl, &mCfg) != 0)
            {
                LogError("PSP HTTPS: mbedtls_ssl_setup failed");
                return false;
            }
            if (mbedtls_ssl_set_hostname(&mSsl, url.host.c_str()) != 0)
            {
                LogError("PSP HTTPS: mbedtls_ssl_set_hostname failed");
                return false;
            }

            mbedtls_ssl_set_bio(&mSsl, (void*)(intptr_t)mSock, MbedNetSend, MbedNetRecv, nullptr);

            int rc = 0;
            while ((rc = mbedtls_ssl_handshake(&mSsl)) != 0)
            {
                if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
                {
                    char errBuf[128] = {};
                    mbedtls_strerror(rc, errBuf, sizeof(errBuf));
                    LogError("PSP HTTPS: mbedtls_ssl_handshake failed: -0x%04X (%s)", -rc, errBuf);
                    return false;
                }
            }
            return true;
        }

        int Send(const char* data, size_t size) override
        {
            int rc;
            do { rc = mbedtls_ssl_write(&mSsl, (const unsigned char*)data, size); }
            while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
            return rc < 0 ? -1 : rc;
        }
        int Recv(char* buffer, size_t size) override
        {
            int rc;
            do { rc = mbedtls_ssl_read(&mSsl, (unsigned char*)buffer, size); }
            while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
            if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            return rc < 0 ? -1 : rc;
        }
        void Close() override
        {
            mbedtls_ssl_close_notify(&mSsl);
            if (mSock >= 0) { NET_SocketClose(mSock); mSock = -1; }
        }

    private:
        mbedtls_x509_crt*         mCa;
        mbedtls_ctr_drbg_context* mDrbg;
        bool                      mVerify = true;
        mbedtls_ssl_context       mSsl;
        mbedtls_ssl_config        mCfg;
        SocketHandle              mSock = -1;
    };

    // -------------------------------------------------------------------
    // HTTP/1.1 framing — works identically over plain TCP and TLS.
    // -------------------------------------------------------------------
    bool ReadAll(ITransport& t, std::string& out,
                 std::atomic<bool>& cancel,
                 int64_t maxBytes, bool& tooLarge)
    {
        char buf[4096];
        while (true)
        {
            if (cancel.load(std::memory_order_acquire)) return false;
            const int n = t.Recv(buf, sizeof(buf));
            if (n < 0) return false;
            if (n == 0) return true;
            if (maxBytes > 0 && (int64_t)out.size() + (int64_t)n > maxBytes)
            {
                tooLarge = true;
                return false;
            }
            out.append(buf, (size_t)n);
        }
    }

    void TrimRight(std::string& s)
    {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
    }

    bool ParseResponse(const std::string& raw, HttpResponse& outResponse)
    {
        const size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false;

        const std::string headers = raw.substr(0, headerEnd);
        const std::string body    = raw.substr(headerEnd + 4);

        size_t lineStart = 0;
        bool firstLine = true;
        while (lineStart < headers.size())
        {
            size_t eol = headers.find("\r\n", lineStart);
            if (eol == std::string::npos) eol = headers.size();
            const std::string line = headers.substr(lineStart, eol - lineStart);
            lineStart = eol + 2;

            if (firstLine)
            {
                firstLine = false;
                // "HTTP/1.1 200 OK"
                size_t sp1 = line.find(' ');
                if (sp1 == std::string::npos) return false;
                size_t sp2 = line.find(' ', sp1 + 1);
                if (sp2 == std::string::npos) sp2 = line.size();
                outResponse.SetStatus(atoi(line.c_str() + sp1 + 1));
                continue;
            }

            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name  = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
            TrimRight(value);
            outResponse.MutableHeaders().emplace(std::move(name), std::move(value));
        }

        // Body. Handle Content-Length or Transfer-Encoding: chunked.
        const auto& hdrs = outResponse.GetHeaders();
        auto teIt = hdrs.find("Transfer-Encoding");
        if (teIt != hdrs.end() && teIt->second.find("chunked") != std::string::npos)
        {
            std::vector<uint8_t>& bodyBytes = outResponse.MutableBody();
            size_t pos = 0;
            while (pos < body.size())
            {
                size_t crlf = body.find("\r\n", pos);
                if (crlf == std::string::npos) break;
                std::string sizeLine = body.substr(pos, crlf - pos);
                size_t semi = sizeLine.find(';');
                if (semi != std::string::npos) sizeLine.resize(semi);
                const size_t chunkSize = (size_t)strtoul(sizeLine.c_str(), nullptr, 16);
                pos = crlf + 2;
                if (chunkSize == 0) break;
                if (pos + chunkSize > body.size()) break;
                const size_t before = bodyBytes.size();
                bodyBytes.resize(before + chunkSize);
                memcpy(bodyBytes.data() + before, body.data() + pos, chunkSize);
                pos += chunkSize + 2;
            }
        }
        else
        {
            outResponse.MutableBody().assign(body.begin(), body.end());
        }
        return true;
    }

    // -------------------------------------------------------------------
    // Backend
    // -------------------------------------------------------------------
    class PspBackend : public HttpBackend
    {
    public:
        bool Initialize() override
        {
            mbedtls_x509_crt_init(&mCa);
            mbedtls_entropy_init(&mEntropy);
            mbedtls_ctr_drbg_init(&mDrbg);

            const char* pers = "polyphase-psp-http";
            const int seedRc = mbedtls_ctr_drbg_seed(&mDrbg, mbedtls_entropy_func, &mEntropy,
                                                    (const unsigned char*)pers, strlen(pers));
            if (seedRc != 0)
            {
                char errBuf[128] = {};
                mbedtls_strerror(seedRc, errBuf, sizeof(errBuf));
                LogError("PSP HTTP: mbedtls_ctr_drbg_seed failed: -0x%04X (%s)", -seedRc, errBuf);
                mTlsReady = false;
            }
            else
            {
                // Load CA bundle from runtime asset path. On PSP, sceIoOpen
                // treats relative paths as rooted at ms0:/ (PPSSPP behaves
                // the same way), so we must hand it a full absolute path —
                // ms0:/PSP/GAME/POLYPHASE/Engine/CACerts/cacert.pem — via
                // SYS_GetPolyphasePath(). Dolphin's libogc filesystem
                // resolves relative paths against the EBOOT dir natively,
                // hence the bare "Engine/..." path works there.
                const std::string caPath = SYS_GetPolyphasePath() + "Engine/CACerts/cacert.pem";
                Stream s;
                if (s.ReadFile(caPath.c_str(), true))
                {
                    std::vector<unsigned char> pem(s.GetSize() + 1);
                    memcpy(pem.data(), s.GetData(), s.GetSize());
                    pem[s.GetSize()] = 0;
                    const int parseRc = mbedtls_x509_crt_parse(&mCa, pem.data(), pem.size());
                    if (parseRc < 0)
                    {
                        char errBuf[128] = {};
                        mbedtls_strerror(parseRc, errBuf, sizeof(errBuf));
                        LogWarning("PSP HTTPS: CA bundle parse failed: -0x%04X (%s). "
                                   "Verification disabled.", -parseRc, errBuf);
                    }
                    else
                    {
                        mHaveCa = true;
                        LogDebug("PSP HTTPS: CA bundle loaded (%u bytes)", (unsigned)s.GetSize());
                    }
                }
                else
                {
                    LogWarning("PSP HTTPS: CA bundle not found at '%s'. "
                               "Verification disabled.", caPath.c_str());
                }
                mTlsReady = true;
            }
            mInitialized = true;
            return true;
        }

        void Shutdown() override
        {
            mbedtls_ctr_drbg_free(&mDrbg);
            mbedtls_entropy_free(&mEntropy);
            mbedtls_x509_crt_free(&mCa);
            mTlsReady = false;
            mHaveCa   = false;
            mInitialized = false;
        }

        bool        IsAvailable()                 const override { return mInitialized; }
        const char* GetMissingDependencyMessage() const override { return ""; }

        void PerformRequest(const HttpRequest& req,
                            std::atomic<bool>& cancelFlag,
                            HttpResponse& outResponse) override
        {
            const ParsedUrl url = ParseUrl(req.GetUrl());
            if (!url.valid)
            {
                outResponse.SetError(HttpError::InvalidUrl, "URL parse failed");
                return;
            }
            outResponse.SetFinalUrl(req.GetUrl());

            std::unique_ptr<ITransport> t;
            if (url.scheme == "https")
            {
                if (!mTlsReady)
                {
                    outResponse.SetError(HttpError::Tls, "mbedTLS not initialized on PSP");
                    return;
                }
                // VERIFY_REQUIRED only when both caller asks AND we have a
                // CA bundle. Otherwise fall back to VERIFY_NONE (still
                // encrypted) so HTTPS connections still work for dev
                // testing without shipping a CA bundle.
                const bool wantVerify = req.GetVerifySsl() && mHaveCa;
                if (req.GetVerifySsl() && !mHaveCa)
                {
                    static bool sWarned = false;
                    if (!sWarned)
                    {
                        LogWarning("PSP HTTPS: SSL_VERIFYPEER force-disabled (no CA bundle). "
                                   "Ship Engine/CACerts/cacert.pem to enable verification.");
                        sWarned = true;
                    }
                }
                t.reset(new TlsTransport(&mCa, &mDrbg, wantVerify));
            }
            else if (url.scheme == "http")
            {
                t.reset(new TcpTransport());
            }
            else
            {
                outResponse.SetError(HttpError::InvalidUrl, "Only http:// and https:// schemes supported on PSP");
                return;
            }

            if (cancelFlag.load(std::memory_order_acquire))
            {
                outResponse.SetError(HttpError::Cancelled, "Cancelled before connect");
                return;
            }

            if (!t->Connect(url, req.GetTimeoutMs()))
            {
                outResponse.SetError(HttpError::Network, "Connect failed");
                return;
            }

            // Build the request line + headers.
            std::string reqBuf;
            reqBuf.reserve(512);
            reqBuf.append(HttpVerbToString(req.GetVerb()));
            reqBuf.push_back(' ');
            reqBuf.append(url.pathAndQuery);
            reqBuf.append(" HTTP/1.1\r\n");
            reqBuf.append("Host: "); reqBuf.append(url.host); reqBuf.append("\r\n");
            reqBuf.append("User-Agent: Polyphase/1.0 (PSP)\r\n");
            reqBuf.append("Connection: close\r\n");

            bool hasContentLength = false;
            for (const auto& kv : req.GetHeaders())
            {
                reqBuf.append(kv.first); reqBuf.append(": ");
                reqBuf.append(kv.second); reqBuf.append("\r\n");
                if (kv.first.size() == 14
                    && strncasecmp(kv.first.c_str(), "Content-Length", 14) == 0)
                {
                    hasContentLength = true;
                }
            }
            if (!req.GetBody().empty() && !hasContentLength)
            {
                char clbuf[64];
                snprintf(clbuf, sizeof(clbuf), "Content-Length: %u\r\n", (unsigned)req.GetBody().size());
                reqBuf.append(clbuf);
            }
            reqBuf.append("\r\n");
            if (!req.GetBody().empty())
            {
                reqBuf.append(reinterpret_cast<const char*>(req.GetBody().data()), req.GetBody().size());
            }

            if (t->Send(reqBuf.data(), reqBuf.size()) < (int)reqBuf.size())
            {
                outResponse.SetError(HttpError::Network, "send() truncated");
                return;
            }

            std::string raw;
            bool tooLarge = false;
            if (!ReadAll(*t, raw, cancelFlag, req.GetMaxBodyBytes(), tooLarge))
            {
                if (cancelFlag.load(std::memory_order_acquire))
                    outResponse.SetError(HttpError::Cancelled, "Cancelled");
                else if (tooLarge)
                    outResponse.SetError(HttpError::TooLarge, "Response exceeded MaxBodyBytes");
                else
                    outResponse.SetError(HttpError::Network, "recv() failed");
                return;
            }

            if (!ParseResponse(raw, outResponse))
            {
                outResponse.SetError(HttpError::BadResponse, "Malformed HTTP response");
                return;
            }
        }

    private:
        bool                          mInitialized = false;
        bool                          mTlsReady    = false;
        bool                          mHaveCa      = false;
        mbedtls_x509_crt              mCa;
        mbedtls_entropy_context       mEntropy;
        mbedtls_ctr_drbg_context      mDrbg;
    };
}

std::unique_ptr<HttpBackend> CreatePlatformHttpBackend()
{
    return std::unique_ptr<HttpBackend>(new PspBackend());
}

#endif // PLATFORM_PSP
