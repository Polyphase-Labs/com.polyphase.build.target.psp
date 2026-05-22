// MSVC SDL deprecation suppression — every CRT call is bounds-checked.
#define _CRT_SECURE_NO_WARNINGS

/**
 * @file ComPolyphaseBuildTargetPsp.cpp
 * @brief PSP (PSPSDK) build-target addon for Polyphase Engine.
 *
 * Adds a "Sony PSP (PSPSDK)" entry to the editor's Build Profile dropdown.
 *
 * Toolchain expectations on the host:
 *
 *   Linux / macOS — native:
 *     - PSPDEV env var pointing at the PSPSDK install prefix (the canonical
 *       install layout puts mips-allegrex tools under $PSPDEV/bin/).
 *     - mksfo / mksfoex and pack-pbp on PATH (ships with PSPSDK).
 *
 *   Windows — via WSL (pspdev ships no native Windows binaries):
 *     - `wsl` available on PATH (Windows 10 2004+ / Windows 11).
 *     - A WSL distro with the Ubuntu pspdev release installed inside it
 *       and `PSPDEV` exported in its login environment (e.g. via ~/.bashrc).
 *       The addon assumes the default distro; override with the
 *       `psp.wslDistro` profile option to pick a specific one.
 *     - Windows-side paths are passed in and translated inside WSL via
 *       `wslpath -u`, so the user never needs to think about /mnt/c/... .
 *
 *   Optional everywhere:
 *     - PPSSPPHeadless / PPSSPPWindows64.exe for emulator launch
 *       (PSP_EMULATOR env var overrides the executable name).
 *
 * Project expectations:
 *   - The PSP build's makefile (`Makefile_PSP`) ships INSIDE this addon, at
 *     `Packages/com.polyphase.build.target.psp/Makefile_PSP`. Projects don't
 *     have to add anything themselves; the addon's GetCompileCommand passes
 *     the addon-relative path to `make -f`. Users can override via the
 *     "Custom Makefile" build-profile option.
 *   - The icon / preview images for the XMB live under <projectDir>/PSP/ as
 *     ICON0.PNG, PIC1.PNG, etc. (per the standard PSP SFO layout). All are
 *     optional — pack-pbp accepts empty slots.
 *
 * Licensing isolation: the engine binary never links against PSPSDK. Every
 * mips-allegrex / pspsdk reference lives only in this addon.
 *
 * Maintainer: Polyphase Engine team.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#include "imgui.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

#if EDITOR
namespace
{
    // ----- Helpers ----------------------------------------------------------

    std::string GetEnvOrEmpty(const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }

    bool FileExists(const std::string& path)
    {
        if (path.empty()) return false;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) { std::fclose(f); return true; }
        return false;
    }

    // ----- Per-profile option keys -----------------------------------------

    constexpr const char* kTitleKey         = "psp.title";          // shown on XMB
    constexpr const char* kDiscIdKey        = "psp.discId";         // 9-char ID e.g. "POLY00001"
    constexpr const char* kMakefileKey      = "psp.makefile";       // bare filename inside addon root, or absolute override
    constexpr const char* kIconPngKey       = "psp.icon0";          // 144x80 ICON0.PNG (relative to projectDir)
    constexpr const char* kBgPngKey         = "psp.pic1";           // 480x272 PIC1.PNG  (relative to projectDir)
    constexpr const char* kFirmwareKey      = "psp.firmware";       // e.g. "1.00", "5.00" (PSP_FW_VERSION)
    constexpr const char* kWslDistroKey     = "psp.wslDistro";      // Windows only — override default WSL distro
    constexpr const char* kPspDevPathKey    = "psp.pspdevPath";     // Path to pspdev install inside the build shell
    constexpr const char* kJobsKey          = "psp.jobs";           // make -j parallelism (default 4; engine TUs are 1-2 GB/TU peak)

    constexpr const char* kTitleDefault     = "Polyphase Game";
    constexpr const char* kDiscIdDefault    = "POLY00001";
    constexpr const char* kMakefileDefault  = "Makefile_PSP";

    // Engine TUs are heavy — Engine.cpp, Renderer.cpp, Bullet, Vorbis etc.
    // each peak at 1-2 GB of psp-gcc RAM. Plain `make -j` means unlimited
    // parallelism, which OOM-freezes 16 GB hosts within seconds. Capping at
    // 4 jobs gives ~6-8 GB peak — safe baseline. Users with 32+ GB hosts
    // can bump this via the "Jobs" build-profile option.
    constexpr const char* kJobsDefault      = "4";
    constexpr const char* kFirmwareDefault  = "5.50";
    // No default for PSPDEV path — we leave it empty so the makefile's own
    // fallback ladder kicks in (/usr/local/pspdev). Users with non-standard
    // installs (e.g. /mnt/c/pspdev for Windows-side pspdev exposed through
    // WSL) enter their path in the Target Options panel.

#if defined(_WIN32)
    // Translate a Windows absolute path into its default WSL2 mount-point:
    //   C:\Foo\Bar  ->  /mnt/c/Foo/Bar
    // Covers the standard /mnt/<drive>/ layout. Users with custom WSL mount
    // roots can side-step this by setting the project's makefile path to an
    // already-WSL-shaped value (the WrapPath helper short-circuits paths
    // that already start with '/').
    std::string WinToWslPath(const std::string& winPath)
    {
        if (winPath.empty()) return winPath;
        if (winPath[0] == '/') return winPath; // already POSIX
        if (winPath.size() >= 2 && winPath[1] == ':')
        {
            std::string out = "/mnt/";
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(winPath[0])));
            for (size_t i = 2; i < winPath.size(); ++i)
            {
                out += (winPath[i] == '\\') ? '/' : winPath[i];
            }
            return out;
        }
        // Relative path — just normalise separators.
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }
#endif

    // Render a host path for embedding inside a `wsl bash -lc "..."`-style
    // command. On Windows we pre-translate to /mnt/<drive>/... so the bash
    // body never sees backslashes (which it would treat as escape chars) and
    // never needs to invoke wslpath at runtime. The path comes back single-
    // quoted so spaces in the path survive bash word-splitting.
    std::string ShellPath(const std::string& path)
    {
#if defined(_WIN32)
        return std::string("'") + WinToWslPath(path) + "'";
#else
        return std::string("'") + path + "'";
#endif
    }

    // Emit "wsl [-d <distro>]" prefix on Windows; empty on POSIX.
    std::string WslPrefix(const PolyphaseBuildContext* ctx)
    {
#if defined(_WIN32)
        std::string distro;
        if (ctx != nullptr && ctx->GetProfileSetting != nullptr)
        {
            char buf[128] = {0};
            if (ctx->GetProfileSetting(kWslDistroKey, buf, sizeof(buf)) != 0 && buf[0] != '\0')
            {
                distro = buf;
            }
        }
        if (distro.empty()) return std::string("wsl ");
        return std::string("wsl -d ") + distro + " ";
#else
        (void)ctx;
        return std::string();
#endif
    }

    // Wrap a single-line bash command for the host:
    //   Windows: `wsl [-d distro] bash -lc "<body>"` (cmd-quoting friendly)
    //   POSIX:   `bash -lc "<body>"`
    // The body must use SINGLE quotes around its paths/strings — the outer
    // double quotes survive cmd.exe and CreateProcess parsing as a single
    // argument to wsl/bash, while inner single quotes are inert to both.
    std::string WrapShell(const PolyphaseBuildContext* ctx, const std::string& body)
    {
        return WslPrefix(ctx) + "bash -lc \"" + body + "\"";
    }

    // Returns a shell prelude that auto-detects PSPDEV from the same fallback
    // ladder Makefile_PSP uses, then prepends $PSPDEV/bin to PATH. Needed
    // because `bash -lc` from the editor's WSL dispatch doesn't source
    // ~/.bashrc on Ubuntu (the stock .bashrc early-returns for non-
    // interactive shells), so an export there is invisible.
    //
    // We can't pass PSPDEV in via `make ... PSPDEV=...` for PostPackage's
    // mksfoex/pack-pbp commands the way we do for compile — these run inside
    // bash directly, not through make — so the auto-detect happens in the
    // shell instead. End result: works regardless of how the user installed
    // pspdev, and the same command line is robust on POSIX hosts too.
    std::string PspDevAutoDetectPrelude()
    {
        // Single-line so it composes cleanly with `&&` into the user-supplied
        // body. Bracket lists of candidate paths; first existing one wins.
        return
            "for d in /usr/local/pspdev /opt/pspdev \\\"\\$HOME/pspdev\\\" "
            "/mnt/c/pspdev /mnt/d/pspdev; do "
            "if [ -x \\\"\\$d/bin/psp-config\\\" ]; then "
            "export PSPDEV=\\\"\\$d\\\"; break; fi; done && "
            "export PATH=\\\"\\$PSPDEV/bin:\\$PATH\\\"";
    }

    std::string ReadOption(const PolyphaseBuildContext* ctx, const char* key, const char* fallback)
    {
        if (ctx == nullptr || ctx->GetProfileSetting == nullptr) return fallback ? fallback : "";
        char buf[512] = {0};
        if (ctx->GetProfileSetting(key, buf, sizeof(buf)) == 0 || buf[0] == '\0')
        {
            return fallback ? fallback : "";
        }
        return std::string(buf);
    }

    // ----- Build-target callbacks ------------------------------------------

    int32_t Psp_Validate(char* outReason, size_t cap)
    {
#if defined(_WIN32)
        // Windows hosts run PSPSDK inside WSL — pspdev publishes no native
        // Windows binaries. The Validate path checks that `wsl` is on PATH
        // and that PSPDEV is set + psp-gcc is present *inside* WSL. We test
        // by running a quick `wsl bash -lc 'command -v psp-gcc'` synchronously
        // and inspecting its exit code.
        char probe[1024];
        std::snprintf(probe, sizeof(probe),
            "wsl bash -lc \"command -v psp-gcc >/dev/null 2>&1 && [ -n \\\"$PSPDEV\\\" ]\"");
        const int rc = std::system(probe);
        if (rc != 0)
        {
            std::snprintf(outReason, cap,
                "PSP toolchain not reachable via WSL. Install WSL2 + Ubuntu, then inside "
                "WSL install pspdev (https://github.com/pspdev/pspdev/releases) and export "
                "PSPDEV in ~/.bashrc. Set the Target Options 'WSL Distro' field to pick a "
                "specific distro if you have more than one.");
            return 0;
        }
        return 1;
#else
        const std::string pspdev = GetEnvOrEmpty("PSPDEV");
        if (pspdev.empty())
        {
            std::snprintf(outReason, cap,
                "PSPDEV env var not set. Install PSPSDK (https://github.com/pspdev/pspdev) "
                "and add the bin/ directory to PATH, or set PSPDEV to the install prefix.");
            return 0;
        }
        if (!FileExists(pspdev + "/bin/psp-gcc"))
        {
            std::snprintf(outReason, cap,
                "PSPDEV='%s' but psp-gcc not found under bin/. Re-run PSPSDK install "
                "or fix PSPDEV.", pspdev.c_str());
            return 0;
        }
        return 1;
#endif
    }

    int32_t Psp_GetCompileCommand(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr) return 0;

        const std::string makefileOpt = ReadOption(ctx, kMakefileKey,    kMakefileDefault);
        const std::string pspdevPath  = ReadOption(ctx, kPspDevPathKey, "");
        const std::string jobsOpt     = ReadOption(ctx, kJobsKey,       kJobsDefault);

        // Validate jobs option — must be a positive decimal int. Reject
        // anything else (silently falls back to default) so we never emit
        // `make -j abc` or `make -j-99` to the shell.
        int jobs = 0;
        for (char c : jobsOpt) { if (c < '0' || c > '9') { jobs = 0; break; } jobs = jobs * 10 + (c - '0'); }
        if (jobs < 1 || jobs > 64) jobs = 4;

        // Resolve the makefile path. Makefile_PSP ships inside the addon —
        // bare filenames (the default) live under
        // <projectDir>/Packages/com.polyphase.build.target.psp/. Absolute
        // overrides (starting with '/' or a Windows drive letter) are taken
        // as-is so users can point at a fork. The `-C` flag below still puts
        // make's working dir at projectDir, so all relative paths inside the
        // makefile (Generated/, Source/, Build/PSP/, etc.) resolve against
        // the project as before.
        const bool isAbsolute =
            !makefileOpt.empty() &&
            (makefileOpt[0] == '/' ||
             (makefileOpt.size() >= 2 && makefileOpt[1] == ':'));
        const std::string makefilePath = isAbsolute
            ? makefileOpt
            : (std::string(ctx->projectDir) +
               "/Packages/com.polyphase.build.target.psp/" + makefileOpt);

        // Pass PSPDEV directly to make so the Makefile doesn't have to depend
        // on the shell having sourced ~/.bashrc (Ubuntu's stock .bashrc
        // early-returns for non-interactive shells, so `bash -lc` invocations
        // see an empty $PSPDEV even if the user added the export there).
        //
        // Plain VAR=VAL on the make line takes precedence over any
        // assignments made inside the makefile body, which is exactly what
        // we want.
        std::string makePspDev;
        if (!pspdevPath.empty())
        {
            makePspDev = " PSPDEV='" + pspdevPath + "'";
        }

        // Pass POLYPHASE_PATH from the build context. The makefile's
        // walk-up auto-detect only finds Polyphase.sln in *direct ancestors*
        // of the project dir — many project layouts (sibling repos, addons
        // checked out separately) violate that. ctx->engineDir is the
        // engine's absolute path from the editor's side; we translate it
        // for the build shell and inject it as a make-line variable so the
        // Makefile never has to guess.
        std::string makePolyphasePath;
        if (ctx->engineDir != nullptr && ctx->engineDir[0] != '\0')
        {
            // ShellPath already handles the Windows→/mnt/<drv> translation
            // on Windows hosts and passes through unchanged on POSIX. It
            // also single-quotes the path so spaces survive.
            makePolyphasePath = " POLYPHASE_PATH=" + ShellPath(ctx->engineDir);
        }

        char jobsArg[16];
        std::snprintf(jobsArg, sizeof(jobsArg), " -j%d", jobs);

        // Out-of-tree build: all .o / .d intermediates live in
        // <projectDir>/Intermediate/PSP/ instead of being scattered across the
        // project root. Make's CWD = Intermediate/PSP/, and the Makefile's
        // PROJECT_ROOT variable lets it find source/include/output paths
        // relative to the user's actual project. Keeps the project root clean
        // and `git add .`-safe.
        const std::string intermediateDir =
            std::string(ctx->projectDir) + "/Intermediate/PSP";
        const std::string mkIntermediate =
            "mkdir -p " + ShellPath(intermediateDir) + " && ";

        // Force Rebuild: clean *.o / *.d / *.elf in the intermediate dir AND
        // the staged binary in Build/PSP. Necessary because PSPSDK's
        // build.mak emits no .d dep files, so header-only changes never
        // invalidate stale .o's.
        std::string cleanPrefix;
        if (ctx->forceRebuild)
        {
            cleanPrefix =
                "(cd " + ShellPath(intermediateDir) +
                " && rm -f *.o *.d *.elf 2>/dev/null; true) && " +
                "(rm -f " + ShellPath(std::string(ctx->projectDir) + "/Build/PSP") +
                "/*.elf 2>/dev/null; true) && ";
        }

        // Pass PROJECT_ROOT explicitly so the Makefile doesn't have to derive
        // it from CURDIR (which works for the standard layout but breaks if
        // someone moves Intermediate/ elsewhere).
        const std::string makeProjectRoot =
            " PROJECT_ROOT=" + ShellPath(ctx->projectDir);

        const std::string body =
            mkIntermediate +
            cleanPrefix +
            "make -C " + ShellPath(intermediateDir) +
            " -f " + ShellPath(makefilePath) +
            makeProjectRoot + makePspDev + makePolyphasePath + jobsArg;

        std::snprintf(outCmd, cap, "%s", WrapShell(ctx, body).c_str());
        return 1;
    }

    int32_t Psp_GetCompiledBinaryPath(const PolyphaseBuildContext* ctx, char* outPath, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr || ctx->projectName == nullptr) return 0;

        // PSPSDK projects compile to an unwrapped ELF first; pack-pbp later
        // turns that into EBOOT.PBP. Convention: Build/PSP/<name>.elf.
        std::snprintf(outPath, cap, "%s/Build/PSP/%s.elf",
                      ctx->projectDir, ctx->projectName);
        return 1;
    }

    // Generate a minimal PARAM.SFO. The full SFO layout is documented at
    // https://www.psdevwiki.com/ps3/PARAM.SFO — for PSP we only need a
    // handful of fields. We shell out to mksfoex (or fall back to mksfo) so
    // the addon doesn't need to embed the binary format.
    //
    // On Windows the call is routed through `wsl bash -lc` so the pspdev
    // binary inside WSL runs against /mnt/c paths. On POSIX it runs natively
    // against the same logical bash command — both paths share one body.
    bool RunSfoMaker(const PolyphaseBuildContext* ctx,
                     const std::string& title,
                     const std::string& discId,
                     const std::string& firmware,
                     const std::string& outSfoPath)
    {
        // Title may contain spaces — embed under single quotes inside the
        // bash body. discId and firmware are constrained to alphanumerics so
        // they're safe unquoted.
        auto quoteForBash = [](const std::string& s) {
            std::string out = "'";
            for (char c : s)
            {
                // Close-and-reopen to escape any literal single quotes the
                // title might contain.
                if (c == '\'') out += "'\\''";
                else           out += c;
            }
            out += '\'';
            return out;
        };

        const std::string titleQuoted = quoteForBash(title);
        const std::string sfoQuoted   = ShellPath(outSfoPath);

        // Try mksfoex (PSPSDK's modern variant — supports DISC_ID + firmware).
        {
            std::string body = PspDevAutoDetectPrelude() + " && mksfoex -d MEMSIZE=1 -s DISC_ID=" +
                               discId + " -s PSP_SYSTEM_VER=" + firmware + " " +
                               titleQuoted + " " + sfoQuoted;
            const std::string cmd = WrapShell(ctx, body);
            if (ctx && ctx->WriteOutputLine) ctx->WriteOutputLine(cmd.c_str());
            if (std::system(cmd.c_str()) == 0) return true;
        }

        // Fall back to mksfo (older PSPSDK / minimal layouts).
        {
            std::string body = PspDevAutoDetectPrelude() + " && mksfo " +
                               titleQuoted + " " + sfoQuoted;
            const std::string cmd = WrapShell(ctx, body);
            if (ctx && ctx->WriteOutputLine) ctx->WriteOutputLine(cmd.c_str());
            return std::system(cmd.c_str()) == 0;
        }
    }

    // Force `WindowWidth=480` / `WindowHeight=272` in a packaged Config.ini.
    // PSP screen is fixed at 480x272, but the project's `Config.ini` carries
    // whatever the user authored for desktop (typically 1280x720), and the
    // engine's `ReadEngineConfig` reads those values at boot, overriding the
    // runtime's `OctPreInitialize` defaults. Without this rewrite step,
    // FullStretch widgets compute their rects against a 1280x720 viewport
    // and PSP's 480x272 scissor crops the result to a tiny corner.
    //
    // Strategy: read the file line-by-line, replace any existing
    // WindowWidth/WindowHeight lines, append them if absent. Other lines pass
    // through unchanged so the user's other settings (Logging, ColorScale,
    // etc.) are preserved.
    void ForcePspWindowSizeInConfig(const std::string& configPath)
    {
        std::ifstream in(configPath);
        if (!in.is_open()) return;

        std::ostringstream out;
        std::string line;
        bool sawW = false;
        bool sawH = false;
        while (std::getline(in, line))
        {
            if (line.rfind("WindowWidth=", 0) == 0)
            {
                out << "WindowWidth=480\n";
                sawW = true;
            }
            else if (line.rfind("WindowHeight=", 0) == 0)
            {
                out << "WindowHeight=272\n";
                sawH = true;
            }
            else
            {
                out << line << "\n";
            }
        }
        in.close();

        if (!sawW) out << "WindowWidth=480\n";
        if (!sawH) out << "WindowHeight=272\n";

        std::ofstream o(configPath, std::ios::trunc);
        if (o.is_open()) o << out.str();
    }

    // Copy a single file using std::ifstream/std::ofstream. Creates the
    // destination directory tree as needed (via mkdir -p shelled out — keeps
    // this addon free of <filesystem>, which not all PSPSDK hosts ship).
    // Returns true on success, false if the source can't be opened. Engine-
    // wide bundling (cacert.pem, etc.) goes through this from Psp_PostPackage.
    bool PspCopyFile(const std::string& srcPath, const std::string& dstPath)
    {
        std::ifstream in(srcPath, std::ios::binary);
        if (!in.is_open()) return false;

        // Carve the parent dir for dstPath. shell `mkdir -p` handles missing
        // intermediates. ShellPath quotes for spaces. Errors bubble through
        // as the subsequent ofstream open failing.
        const size_t lastSlash = dstPath.find_last_of("/\\");
        if (lastSlash != std::string::npos)
        {
            const std::string parent = dstPath.substr(0, lastSlash);
            const std::string mk = std::string("mkdir -p ") + ShellPath(parent);
            std::system(mk.c_str());
        }

        std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << in.rdbuf();
        return out.good();
    }

    // Ship the Mozilla CA bundle into the package so HttpBackend_PSP can
    // load it at runtime via Stream::ReadFile("Engine/CACerts/cacert.pem").
    // Without this the PSP runtime warns + falls back to verify=none for
    // every HTTPS request. Source lives in <engineDir>/External/CACerts/.
    void StageCaBundle(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->engineDir == nullptr || ctx->packageOutputDir == nullptr) return;

        const std::string src = std::string(ctx->engineDir) + "/External/CACerts/cacert.pem";
        const std::string dst = std::string(ctx->packageOutputDir) + "/Engine/CACerts/cacert.pem";
        if (!PspCopyFile(src, dst))
        {
            if (ctx->Log != nullptr)
            {
                char msg[512];
                std::snprintf(msg, sizeof(msg),
                    "PSP: CA bundle copy failed (src=%s). HTTPS will run "
                    "without peer verification.", src.c_str());
                ctx->Log(POLYPHASE_BT_LOG_DEBUG, msg);
            }
            return;
        }
        if (ctx->Log != nullptr)
        {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                "PSP: staged CA bundle -> %s", dst.c_str());
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, msg);
        }
    }

    int32_t Psp_PostPackage(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        // Drop the Mozilla CA bundle in alongside the engine assets.
        // HttpBackend_PSP.cpp loads it at runtime to verify HTTPS peers.
        StageCaBundle(ctx);

        const std::string title    = ReadOption(ctx, kTitleKey,    kTitleDefault);
        const std::string discId   = ReadOption(ctx, kDiscIdKey,   kDiscIdDefault);
        const std::string firmware = ReadOption(ctx, kFirmwareKey, kFirmwareDefault);

        // Resolve relative icon/bg paths to absolute via the engine trampoline
        // so users can write "PSP/ICON0.PNG" in the profile without worrying
        // about cwd.
        std::string iconPath, bgPath;
        if (ctx->ResolvePath != nullptr)
        {
            const std::string iconRel = ReadOption(ctx, kIconPngKey, "");
            const std::string bgRel   = ReadOption(ctx, kBgPngKey,   "");
            char abs[1024] = {0};
            if (!iconRel.empty() && ctx->ResolvePath(iconRel.c_str(), abs, sizeof(abs))) iconPath = abs;
            abs[0] = '\0';
            if (!bgRel.empty()   && ctx->ResolvePath(bgRel.c_str(),   abs, sizeof(abs))) bgPath   = abs;
        }

        const std::string outDir   = ctx->packageOutputDir;
        const std::string elfPath  = outDir + "/" + ctx->projectName + ".elf";
        const std::string sfoPath  = outDir + "/PARAM.SFO";
        const std::string ebootPath = outDir + "/EBOOT.PBP";

        // Rewrite both packaged Config.ini copies (root + projectName/) so the
        // PSP runtime always boots with the native 480x272 logical viewport.
        // Project Config.ini carries the desktop window size (typically
        // 1280x720) which ReadEngineConfig would otherwise reload at boot,
        // overriding the OctPreInitialize defaults. Override is silent and
        // non-configurable — PSP has only one screen size.
        ForcePspWindowSizeInConfig(outDir + "/Config.ini");
        ForcePspWindowSizeInConfig(outDir + "/" + std::string(ctx->projectName) + "/Config.ini");
        if (ctx->Log != nullptr)
        {
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, "Patched WindowWidth/Height to 480/272 in packaged Config.ini");
        }

        // (1) Generate PARAM.SFO. PSP firmware refuses to load PBPs without
        //     a valid one, so a failure here aborts the package step.
        if (!RunSfoMaker(ctx, title, discId, firmware, sfoPath))
        {
            if (ctx->Log != nullptr)
            {
                ctx->Log(POLYPHASE_BT_LOG_ERROR,
                         "mksfo/mksfoex failed. Is PSPSDK on PATH (e.g. $PSPDEV/bin)?");
            }
            return 0;
        }

        // (2) pack-pbp slot order is fixed by PSPSDK convention:
        //       EBOOT.PBP <sfo> <icon0.png> <icon1.pmf> <pic0.png> <pic1.png> <snd0.at3> <data.psp> <psar>
        //     We pass NULL for any slot we don't use. The .elf becomes the
        //     DATA.PSP payload (slot 7), which the PSP loader executes.
        auto slotOrNull = [](const std::string& path) {
            return path.empty() ? std::string("NULL") : ShellPath(path);
        };

        std::string body = PspDevAutoDetectPrelude() + " && pack-pbp " +
                           ShellPath(ebootPath) + " " +
                           ShellPath(sfoPath) + " " +
                           slotOrNull(iconPath) + " NULL NULL " +
                           slotOrNull(bgPath) + " NULL " +
                           ShellPath(elfPath) + " NULL";

        const std::string cmd = WrapShell(ctx, body);
        if (ctx->WriteOutputLine != nullptr) ctx->WriteOutputLine(cmd.c_str());
        const int rc = std::system(cmd.c_str());
        if (rc != 0)
        {
            if (ctx->Log != nullptr)
            {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "pack-pbp failed (rc=%d). Verify pack-pbp is reachable "
                    "(WSL on Windows; $PSPDEV/bin on POSIX).", rc);
                ctx->Log(POLYPHASE_BT_LOG_ERROR, msg);
            }
            return 0;
        }

        if (ctx->Log != nullptr)
        {
            char ok[512];
            std::snprintf(ok, sizeof(ok),
                "PSP package complete: %s (DISC_ID=%s, FW>=%s)",
                ebootPath.c_str(), discId.c_str(), firmware.c_str());
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, ok);
        }
        return 1;
    }

    int32_t Psp_RunInEmulator(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr) return 0;

        // PPSSPP is the de-facto Windows emulator. Users override via
        // PSP_EMULATOR if they prefer a different binary (JPCSP, etc).
        const std::string override_ = GetEnvOrEmpty("PSP_EMULATOR");
        const std::string exe = override_.empty()
            ? std::string("PPSSPPWindows64.exe")
            : override_;

        // PPSSPP accepts a path to either EBOOT.PBP or the bare .elf —
        // we hand it the packed PBP so the SFO metadata is honoured.
        std::snprintf(outCmd, cap,
            "\"%s\" \"%s/EBOOT.PBP\"",
            exe.c_str(),
            ctx->packageOutputDir);
        return 1;
    }

    int32_t Psp_RunOnDevice(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        // PSPLINK + pspsh is the canonical USB-deploy flow. Users set
        // PSP_PSPLINK_HOST to the host:port their psplink server listens on.
        // pspsh lives inside PSPSDK, so on Windows it runs through WSL.
        if (ctx == nullptr || ctx->packageOutputDir == nullptr) return 0;

        const std::string host = GetEnvOrEmpty("PSP_PSPLINK_HOST");
        if (host.empty())
        {
            std::snprintf(outCmd, cap,
                "echo \"PSP_PSPLINK_HOST not set. Boot PSP into PSPLINK, set PSP_PSPLINK_HOST=<ip>:<port>\" && exit 1");
            return 1;
        }

        const std::string pbp = std::string(ctx->packageOutputDir) + "/EBOOT.PBP";
        const std::string body = PspDevAutoDetectPrelude() + " && pspsh -H " + host +
                                 " -e " + ShellPath(pbp);
        std::snprintf(outCmd, cap, "%s", WrapShell(ctx, body).c_str());
        return 1;
    }

    void Psp_DrawProfileOptions(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->SetProfileSetting == nullptr) return;

        // ----- Title (XMB display name) ------------------------------------
        {
            std::string current = ReadOption(ctx, kTitleKey, kTitleDefault);
            char buf[128] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Title (XMB)", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kTitleKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Shown in the XMB / on the EBOOT.PBP tile.");
        }

        // ----- Disc ID -----------------------------------------------------
        {
            std::string current = ReadOption(ctx, kDiscIdKey, kDiscIdDefault);
            char buf[16] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Disc ID", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kDiscIdKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("9-character DISC_ID (e.g. POLY00001). Used by PSP to namespace save data.");
        }

        // ----- Minimum firmware --------------------------------------------
        {
            static const char* kFirmwares[] = { "1.00", "1.50", "2.00", "3.00", "3.40", "5.00", "5.50", "6.20" };
            std::string current = ReadOption(ctx, kFirmwareKey, kFirmwareDefault);
            int idx = 6; // default to 5.50
            for (int i = 0; i < IM_ARRAYSIZE(kFirmwares); ++i)
            {
                if (current == kFirmwares[i]) { idx = i; break; }
            }
            if (ImGui::Combo("Min Firmware", &idx, kFirmwares, IM_ARRAYSIZE(kFirmwares)))
            {
                ctx->SetProfileSetting(kFirmwareKey, kFirmwares[idx]);
            }
        }

        // ----- Makefile path -----------------------------------------------
        {
            std::string current = ReadOption(ctx, kMakefileKey, kMakefileDefault);
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Makefile", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kMakefileKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Makefile that wraps PSPSDK's build.mak. Bare filename resolves inside the addon (default: Makefile_PSP). Absolute paths point at a fork.");
        }

        // ----- make -j parallelism ----------------------------------------
        // Each engine TU peaks at 1-2 GB during psp-gcc compile. Plain `-j`
        // (unlimited) freezes 16 GB hosts. Default 4 ≈ 6-8 GB peak, safe on
        // most laptops. Bump to nproc on workstations with 32+ GB.
        {
            std::string current = ReadOption(ctx, kJobsKey, kJobsDefault);
            int jobs = std::atoi(current.c_str());
            if (jobs < 1 || jobs > 64) jobs = 4;
            if (ImGui::SliderInt("Parallel Jobs", &jobs, 1, 32))
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", jobs);
                ctx->SetProfileSetting(kJobsKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("`make -j<N>` parallelism. Each compile job peaks at 1-2 GB RAM.\n"
                                  "Rule of thumb: jobs = min(CPU cores, host RAM GB / 2).\n"
                                  "16 GB host → 4-6 jobs. 32 GB → 8-12. Default 4.");
        }

        // ----- WSL distro override (Windows-only host concept, but harmless
        //       to expose elsewhere — POSIX hosts simply ignore it) ---------
        {
            std::string current = ReadOption(ctx, kWslDistroKey, "");
            char buf[64] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("WSL Distro (Windows)", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kWslDistroKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Windows-only: pass to `wsl -d <name>`. Leave empty to use the default distro.\n"
                                  "Ignored on Linux/macOS.");
        }

        // ----- PSPDEV path -------------------------------------------------
        // Path to the pspdev install as seen from the BUILD shell (WSL on
        // Windows hosts; native filesystem on Linux/macOS). Passed straight
        // through to `make PSPDEV=<value>` so the Makefile doesn't have to
        // guess. Leave empty if pspdev is at /usr/local/pspdev (the makefile
        // default).
        {
            std::string current = ReadOption(ctx, kPspDevPathKey, "");
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("PSPDEV path", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kPspDevPathKey, buf);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Absolute path to your pspdev install AS SEEN BY THE BUILD SHELL.\n"
                    "  - Windows: WSL-side path, e.g. /mnt/c/pspdev or /usr/local/pspdev.\n"
                    "  - Linux/macOS: native path, e.g. /usr/local/pspdev or $HOME/pspdev.\n"
                    "Leave empty to use the makefile's /usr/local/pspdev default.");
            }
        }

        // ----- Icon / background -------------------------------------------
        {
            std::string current = ReadOption(ctx, kIconPngKey, "");
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("ICON0.PNG (144x80)", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kIconPngKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Project-relative path to ICON0.PNG. Optional — pack-pbp accepts an empty slot.");
        }
        {
            std::string current = ReadOption(ctx, kBgPngKey, "");
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("PIC1.PNG (480x272)", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kBgPngKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Project-relative path to PIC1.PNG. Optional XMB background.");
        }

        ImGui::Spacing();
#if defined(_WIN32)
        ImGui::TextDisabled("Windows: PSPSDK runs inside WSL. Install WSL2 + Ubuntu, then");
        ImGui::TextDisabled("install pspdev inside WSL and export PSPDEV in ~/.bashrc.");
#else
        ImGui::TextDisabled("Requires PSPSDK ($PSPDEV), mksfo[ex], and pack-pbp on PATH.");
#endif
        ImGui::TextDisabled("Default emulator is PPSSPPWindows64.exe — override with PSP_EMULATOR env var.");
        ImGui::TextDisabled("Set PSP_PSPLINK_HOST=<ip>:<port> to enable 'Run on Device' via pspsh.");
    }

    // Canonical descriptor. Strings are deep-copied by the registry; this
    // static instance just needs to outlive the RegisterBuildTarget call.
    static PolyphaseBuildTargetDesc gPspTarget{};
}
#endif // EDITOR

// ----- Plugin lifecycle -----------------------------------------------------

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    if (api) api->LogDebug("com.polyphase.build.target.psp loaded.");
    return 0;
}

static void OnUnload()
{
    if (sEngineAPI) sEngineAPI->LogDebug("com.polyphase.build.target.psp unloaded.");
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* /*nodeFactory*/) {}
static void RegisterScriptFuncs(lua_State* L) { (void)L; }

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    if (hooks == nullptr) return;

    if (hooks->RegisterBuildTarget == nullptr)
    {
        if (sEngineAPI)
        {
            sEngineAPI->LogWarning("com.polyphase.build.target.psp: this engine "
                                   "build predates the build-target API (need plugin "
                                   "apiVersion >= 4). Target not registered.");
        }
        return;
    }

    gPspTarget = {};
    gPspTarget.apiVersion            = POLYPHASE_BUILD_TARGET_API_VERSION;
    gPspTarget.targetId              = "homebrew.psp";
    gPspTarget.displayName           = "Sony PSP (PSPSDK)";
    gPspTarget.iconText              = "";
    gPspTarget.category              = "Handheld";
    gPspTarget.basePlatform          = 1; /* Platform::Linux — Unix-like cook + ELF */
    gPspTarget.binaryExtension       = ".pbp";
    gPspTarget.requiresDocker        = 0;
    gPspTarget.supportsRunOnDevice   = 1;
    gPspTarget.supportsEmulator      = 1;
    gPspTarget.Validate              = &Psp_Validate;
    gPspTarget.PreCook               = nullptr;
    gPspTarget.CookAsset             = nullptr; // Linux cook is fine for PSP V1
    gPspTarget.GetCompileCommand     = &Psp_GetCompileCommand;
    gPspTarget.GetCompiledBinaryPath = &Psp_GetCompiledBinaryPath;
    gPspTarget.PostPackage           = &Psp_PostPackage;
    gPspTarget.RunOnDevice           = &Psp_RunOnDevice;
    gPspTarget.RunInEmulator         = &Psp_RunInEmulator;
    gPspTarget.DrawProfileOptions    = &Psp_DrawProfileOptions;
    gPspTarget.SerializeProfileOptions   = nullptr;
    gPspTarget.DeserializeProfileOptions = nullptr;

    // Variant 2: point the engine at the addon-shipped platform extension
    // headers. The path is interpreted relative to the addon's root (the
    // directory containing package.json). When this build target is selected,
    // ActionManager will write Generated/PolyphasePlatform_*.h bridges that
    // #include the addon's SystemTypes_Platform.h / InputTypes_Platform.h /
    // AudioTypes_Platform.h / NetworkTypes_Platform.h via absolute path. The
    // Makefile_PSP then sets -DPOLYPHASE_PLATFORM_ADDON=1 and -I<Generated/>
    // so the engine's fork headers pick up the addon-provided typedefs.
    gPspTarget.platformExtensionDir = "Runtime/PSP";

    hooks->RegisterBuildTarget(hookId, &gPspTarget);
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    if (desc == nullptr) return 1;
    desc->apiVersion          = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName          = "com.polyphase.build.target.psp";
    desc->pluginVersion       = "1.0.0";
    desc->OnLoad              = OnLoad;
    desc->OnUnload            = OnUnload;
    desc->Tick                = nullptr;
    desc->TickEditor          = nullptr;
    desc->RegisterTypes       = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI    = RegisterEditorUI;
#else
    desc->RegisterEditorUI    = nullptr;
#endif
    desc->OnEditorPreInit     = nullptr;
    desc->OnEditorReady       = nullptr;
    return 0;
}
