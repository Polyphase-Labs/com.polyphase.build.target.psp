/**
 * @file Audio_PSP.cpp
 * @brief PSP audio implementation — software mixer + one sceAudio HW channel.
 *
 * Architecture:
 *   - Engine drives AUD_Play(voiceIndex 0..7, soundWave, vol, pitch, loop, ...)
 *     onto an internal voice table. AUD_Stop/SetVolume/SetPitch mutate entries.
 *   - A dedicated mixer thread sleeps inside sceAudioOutputBlocking; when the
 *     HW consumes the last submitted buffer the call returns, the mixer pulls
 *     fresh samples from every active voice into a stereo S16LE buffer, and
 *     submits the next one. Output rate is fixed 44100 Hz stereo.
 *   - Per-voice resampling is nearest-neighbour with a fractional source-frame
 *     cursor advanced by `rate = sourceSampleRate * pitch / 44100` each output
 *     frame. Good enough for SFX + most music; upgrade to linear interp later
 *     if quality wants it.
 *
 * Why a software mixer instead of one sceAudio channel per voice:
 *   PSP exposes 8 HW channels which matches AUDIO_MAX_VOICES, but per-channel
 *   submit semantics force you to keep up with each channel independently
 *   (the blocking variant blocks ONE channel; the non-blocking variant lets
 *   you under-run silently). A single mixer + single HW channel is simpler
 *   to reason about, gives unlimited voice count if we ever need it, and
 *   lets us cleanly handle per-voice pitch / sample-rate conversion in one
 *   place. Mirrors what most modern engines do on PSP (Doom port, OpenLara,
 *   etc.) and what Polyphase's Wii (ASND-via-libogc2) and 3DS (NDSP) paths
 *   effectively do under the hood.
 *
 * Threading discipline:
 *   - Voice state is protected by a counting semaphore (sceKernelCreateSema
 *     with initial=1, max=1) so the mutate path is symmetric on every
 *     PSPSDK version (sceKernelCreateMutex requires firmware >= 2.71).
 *   - Lock is held briefly: engine-side mutators take it, do one struct
 *     write, release. Mixer takes it for the duration of one buffer fill
 *     (~few hundred microseconds at this voice count) and then drops it
 *     before the long blocking sceAudioOutputBlocking call so the engine
 *     thread is never blocked waiting on audio HW.
 *   - SoundWave pointers in voices are weak refs — the engine guarantees
 *     the SoundWave outlives any active play via its asset reference count.
 *
 * The streaming API (AUD_OpenStream etc.) currently stays as a graceful
 * no-op success returning zero so the engine's video-player consumers don't
 * crash. Real streaming-voice routing through the mixer is a follow-up.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include <stdlib.h>
#include <string.h>

namespace
{
    // ----- Output format ---------------------------------------------------
    constexpr int kOutputRate         = 44100;
    constexpr int kFramesPerBuffer    = 1024;   // ~23.2 ms @ 44.1 kHz
    constexpr int kSamplesPerBuffer   = kFramesPerBuffer * 2;  // stereo

    // Master headroom — N voices summing into one s32 lane can easily clip
    // past s16. 0.5x master leaves ~6 dB headroom; raise after measuring
    // if you need more loudness and you've verified clipping isn't an issue.
    constexpr float kMasterAtten      = 0.5f;

    // ----- Voice table -----------------------------------------------------
    struct PspVoice
    {
        bool            active        = false;
        const uint8_t*  pcmData       = nullptr;
        uint32_t        numFrames     = 0;     // total source frames (not bytes)
        uint32_t        sampleRate    = 0;
        uint8_t         numChannels   = 1;
        uint8_t         bitsPerSample = 16;
        bool            loop          = false;

        // Source-frame cursor (fractional for resampling) and the rate at
        // which the cursor advances per output frame. rate = sourceRate *
        // pitch / kOutputRate.
        double          positionFrac  = 0.0;
        double          rate          = 1.0;

        // Q15 per-side volume — 0..32768. Engine passes 0..1 floats; we
        // pre-multiply by kMasterAtten and scale to Q15. Per-side so spatial
        // panning lands here naturally.
        int32_t         leftVolQ15    = 32768;
        int32_t         rightVolQ15   = 32768;
    };

    static PspVoice sVoices[AUDIO_MAX_VOICES];
    static SceUID   sVoiceLock        = -1;
    static SceUID   sMixerThread      = -1;
    static int      sAudioChannel     = -1;
    static volatile bool sMixerRun    = false;

    // Output / scratch buffers. Aligned to 64 B for safe DMA.
    static int16_t  __attribute__((aligned(64))) sOutBuffer[kSamplesPerBuffer];
    static int32_t  __attribute__((aligned(64))) sMixBuffer[kSamplesPerBuffer];

    inline int32_t SaturateS16(int32_t v)
    {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return v;
    }

    // Read one source frame at the given integer position and produce
    // L / R int32 samples scaled to s16-equivalent magnitude (s32 to keep
    // headroom for per-voice volume multiply later).
    inline void FetchFrame(const PspVoice& v, uint32_t srcFrame,
                           int32_t& outL, int32_t& outR)
    {
        if (v.bitsPerSample == 16)
        {
            const int16_t* s = reinterpret_cast<const int16_t*>(v.pcmData);
            if (v.numChannels == 2)
            {
                outL = s[srcFrame * 2 + 0];
                outR = s[srcFrame * 2 + 1];
            }
            else
            {
                const int32_t mono = s[srcFrame];
                outL = outR = mono;
            }
        }
        else   // 8-bit unsigned PCM, scale to s16 range
        {
            if (v.numChannels == 2)
            {
                outL = (int32_t(v.pcmData[srcFrame * 2 + 0]) - 128) << 8;
                outR = (int32_t(v.pcmData[srcFrame * 2 + 1]) - 128) << 8;
            }
            else
            {
                const int32_t mono = (int32_t(v.pcmData[srcFrame]) - 128) << 8;
                outL = outR = mono;
            }
        }
    }

    void MixOneBuffer()
    {
        memset(sMixBuffer, 0, sizeof(sMixBuffer));

        sceKernelWaitSema(sVoiceLock, 1, nullptr);

        for (uint32_t vi = 0; vi < AUDIO_MAX_VOICES; ++vi)
        {
            PspVoice& v = sVoices[vi];
            if (!v.active || v.pcmData == nullptr || v.numFrames == 0) continue;

            for (int f = 0; f < kFramesPerBuffer; ++f)
            {
                uint32_t srcFrame = (uint32_t)v.positionFrac;

                if (srcFrame >= v.numFrames)
                {
                    if (v.loop)
                    {
                        // Wrap fractional cursor — preserves any sub-frame
                        // offset so looped audio doesn't drift in pitch.
                        v.positionFrac -= (double)v.numFrames;
                        srcFrame = (uint32_t)v.positionFrac;
                        if (srcFrame >= v.numFrames) srcFrame = 0;
                    }
                    else
                    {
                        v.active = false;
                        break;
                    }
                }

                int32_t srcL, srcR;
                FetchFrame(v, srcFrame, srcL, srcR);

                sMixBuffer[f * 2 + 0] += (srcL * v.leftVolQ15)  >> 15;
                sMixBuffer[f * 2 + 1] += (srcR * v.rightVolQ15) >> 15;

                v.positionFrac += v.rate;
            }
        }

        sceKernelSignalSema(sVoiceLock, 1);

        // Saturate + write to output buffer.
        for (int i = 0; i < kSamplesPerBuffer; ++i)
        {
            sOutBuffer[i] = (int16_t)SaturateS16(sMixBuffer[i]);
        }
    }

    int MixerThread(SceSize /*argc*/, void* /*argv*/)
    {
        while (sMixerRun)
        {
            MixOneBuffer();

            // Blocks until the previously-submitted buffer has finished
            // playing on the HW channel, then queues this one. Naturally
            // syncs the mixer to the audio output rate (~43 Hz @ 1024
            // frames per buffer / 44100 Hz).
            sceAudioOutputBlocking(sAudioChannel, PSP_AUDIO_VOLUME_MAX, sOutBuffer);
        }

        // Drain by submitting a zero buffer — prevents the HW channel from
        // looping the last sample audibly on exit.
        memset(sOutBuffer, 0, sizeof(sOutBuffer));
        sceAudioOutputBlocking(sAudioChannel, 0, sOutBuffer);

        sceKernelExitThread(0);
        return 0;
    }

    inline int32_t ClampVolQ15(float v)
    {
        const float scaled = v * kMasterAtten * 32768.0f;
        if (scaled < 0.0f)     return 0;
        if (scaled > 32768.0f) return 32768;
        return (int32_t)scaled;
    }
}  // namespace

// ----- API impl ------------------------------------------------------------

void AUD_Initialize()
{
    sVoiceLock = sceKernelCreateSema("polyaud_voice", 0, 1, 1, nullptr);
    if (sVoiceLock < 0)
    {
        LogError("Audio_PSP: sceKernelCreateSema failed: 0x%08X", (unsigned)sVoiceLock);
        return;
    }

    // PSP_AUDIO_SAMPLE_ALIGN rounds up to the next legal HW buffer size; our
    // 1024 is already aligned but keep the call for clarity / future safety.
    sAudioChannel = sceAudioChReserve(
        PSP_AUDIO_NEXT_CHANNEL,
        PSP_AUDIO_SAMPLE_ALIGN(kFramesPerBuffer),
        PSP_AUDIO_FORMAT_STEREO);
    if (sAudioChannel < 0)
    {
        LogError("Audio_PSP: sceAudioChReserve failed: 0x%08X", (unsigned)sAudioChannel);
        sceKernelDeleteSema(sVoiceLock);
        sVoiceLock = -1;
        return;
    }

    sMixerRun    = true;
    sMixerThread = sceKernelCreateThread(
        "polyaud_mixer",
        &MixerThread,
        0x12,                 // priority — slightly higher than default (0x18)
        0x4000,               // 16 KB stack
        0,
        nullptr);
    if (sMixerThread < 0)
    {
        LogError("Audio_PSP: sceKernelCreateThread failed: 0x%08X", (unsigned)sMixerThread);
        sceAudioChRelease(sAudioChannel);
        sAudioChannel = -1;
        sceKernelDeleteSema(sVoiceLock);
        sVoiceLock = -1;
        sMixerRun = false;
        return;
    }
    sceKernelStartThread(sMixerThread, 0, nullptr);

    LogDebug("Audio_PSP: mixer up, ch=%d 44100Hz stereo, %d frames/buf, %d voices",
             sAudioChannel, kFramesPerBuffer, AUDIO_MAX_VOICES);
}

void AUD_Shutdown()
{
    sMixerRun = false;

    // Mixer is blocked inside sceAudioOutputBlocking — it'll wake on its
    // own when the current buffer finishes playing (worst case ~23 ms).
    // sceKernelWaitThreadEnd cleanly blocks the shutdown call until then.
    if (sMixerThread >= 0)
    {
        sceKernelWaitThreadEnd(sMixerThread, nullptr);
        sceKernelDeleteThread(sMixerThread);
        sMixerThread = -1;
    }
    if (sAudioChannel >= 0)
    {
        sceAudioChRelease(sAudioChannel);
        sAudioChannel = -1;
    }
    if (sVoiceLock >= 0)
    {
        sceKernelDeleteSema(sVoiceLock);
        sVoiceLock = -1;
    }
}

void AUD_Update() { /* mixer runs on its own thread, nothing to do per-frame */ }

void AUD_Play(uint32_t voiceIndex, SoundWave* soundWave, float volume,
              float pitch, bool loop, float /*startTime*/, bool spatial)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    if (soundWave == nullptr) return;
    const uint8_t* pcm = soundWave->GetWaveData();
    const uint32_t numFrames = soundWave->GetNumSamples();
    if (pcm == nullptr || numFrames == 0) return;

    sceKernelWaitSema(sVoiceLock, 1, nullptr);

    PspVoice& v       = sVoices[voiceIndex];
    v.pcmData         = pcm;
    v.numFrames       = numFrames;
    v.sampleRate      = soundWave->GetSampleRate();
    v.numChannels     = (uint8_t)soundWave->GetNumChannels();
    v.bitsPerSample   = (uint8_t)soundWave->GetBitsPerSample();
    v.loop            = loop;
    v.positionFrac    = 0.0;
    v.rate            = (v.sampleRate > 0)
                         ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate)
                         : 1.0;
    // Volume is single-channel from this API — engine calls SetVolume later
    // for spatial L/R split. Initialise both sides to the same level.
    const int32_t vq15 = ClampVolQ15(volume);
    v.leftVolQ15      = vq15;
    v.rightVolQ15     = vq15;
    v.active          = true;
    (void)spatial;   // spatial routing is handled by the engine setting L/R volumes separately

    sceKernelSignalSema(sVoiceLock, 1);
}

void AUD_Stop(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    sceKernelWaitSema(sVoiceLock, 1, nullptr);
    sVoices[voiceIndex].active = false;
    sceKernelSignalSema(sVoiceLock, 1);
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return false;
    // Atomic enough — bool read on PSP MIPS, no torn read possible.
    return sVoices[voiceIndex].active;
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    sceKernelWaitSema(sVoiceLock, 1, nullptr);
    sVoices[voiceIndex].leftVolQ15  = ClampVolQ15(leftVolume);
    sVoices[voiceIndex].rightVolQ15 = ClampVolQ15(rightVolume);
    sceKernelSignalSema(sVoiceLock, 1);
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    sceKernelWaitSema(sVoiceLock, 1, nullptr);
    PspVoice& v = sVoices[voiceIndex];
    v.rate = (v.sampleRate > 0)
               ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate)
               : 1.0;
    sceKernelSignalSema(sVoiceLock, 1);
}

// ----- Wave-buffer allocation (no special PSP requirement, plain heap) ----
uint8_t* AUD_AllocWaveBuffer(uint32_t size) { return (uint8_t*)malloc(size); }
void AUD_FreeWaveBuffer(void* buffer) { free(buffer); }
void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

// ----- Streaming voices (used by VideoPlayer addon, etc.) -----------------
// Stub for now — return zero stream-id to signal "stream creation failed",
// which the engine's streaming consumers handle gracefully (they fall back
// to silent video or skip the audio track). Plumbing a streaming voice
// through the mixer is a follow-up: needs a PCM ring buffer per stream,
// drained by MixOneBuffer the same way active voices are.
uint32_t AUD_OpenStream(uint32_t /*sampleRate*/, uint32_t /*numChannels*/, uint32_t /*bitsPerSample*/) { return 0; }
void     AUD_CloseStream(uint32_t /*streamId*/) {}
int32_t  AUD_SubmitStreamBuffer(uint32_t /*streamId*/, const uint8_t* /*data*/, uint32_t byteSize) { return (int32_t)byteSize; }
uint64_t AUD_GetStreamPlayedSamples(uint32_t /*streamId*/) { return 0; }
void     AUD_SetStreamVolume(uint32_t /*streamId*/, float /*volume*/) {}
void     AUD_SetStreamPaused(uint32_t /*streamId*/, bool /*paused*/) {}
void     AUD_FlushStream(uint32_t /*streamId*/) {}

#endif // POLYPHASE_PLATFORM_ADDON
