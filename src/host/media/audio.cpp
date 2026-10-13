#include "host/media/audio.hpp"

using namespace std::chrono_literals;

namespace {
void InitCOM() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) throw std::runtime_error("COM initialization failed");
}
void ChkHR(HRESULT h, const char* m) { if (FAILED(h)) throw std::runtime_error(m); }

IMMDeviceEnumerator* CreateDeviceEnumerator() {
    IMMDeviceEnumerator* enumerator = nullptr;
    ChkHR(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)), "MMDeviceEnumerator");
    return enumerator;
}

IAudioClient* ActivateAudioClient(IMMDevice* device, const char* context) {
    IAudioClient* client = nullptr;
    ChkHR(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)), context);
    return client;
}

WAVEFORMATEX* GetAudioMixFormat(IAudioClient* client) {
    WAVEFORMATEX* format = nullptr;
    ChkHR(client->GetMixFormat(&format), "GetMixFormat");
    return format;
}

template <typename LogFn>
void LimitResamplerBuffer(std::vector<float>& buffer, size_t& readPos, size_t maxSamples, size_t keepSamples, int channelCount, LogFn&& logFn) {
    const size_t bufferedSamples = buffer.size() > readPos ? (buffer.size() - readPos) : 0;
    if (bufferedSamples <= maxSamples) return;

    const size_t dropSamples = bufferedSamples - keepSamples;
    readPos = std::min(readPos + dropSamples, buffer.size());
    logFn(dropSamples, bufferedSamples, maxSamples, channelCount);
}

void TrimConsumedResamplerBuffer(std::vector<float>& buffer, size_t& readPos) {
    if (readPos == 0 || (readPos < buffer.size() && readPos < 8192)) return;

    if (readPos < buffer.size()) {
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(readPos));
    } else {
        buffer.clear();
    }
    readPos = 0;
}

void ResetResamplerState(std::unique_ptr<SpeexResampler>& resampler, size_t& readPos) {
    resampler->Reset();
    readPos = 0;
}

}

// ═══════════════════════════════════════════════════════════════
// AudioCapture — loopback capture → Opus encode → queue
// ═══════════════════════════════════════════════════════════════

AudioCapture::AudioCapture() {
    InitCOM();

    deviceEnumerator = CreateDeviceEnumerator();
    opusBuffer.resize(4000);

    // A missing output device is not fatal: the capture loop keeps retrying, so audio comes up
    // once a device (or virtual audio driver) appears.
    try {
        OpenDevice();
    } catch (const std::exception& e) {
        WARN("AudioCapture: No usable output device yet (%s); will retry", e.what());
        CloseDevice();
    }

    isInitialized = true;
    CoUninitialize();
}

AudioCapture::~AudioCapture() {
    Stop();
    CloseDevice();
    if (opusEncoder) opus_encoder_destroy(opusEncoder);
    SafeRelease(deviceEnumerator);
}

void AudioCapture::OpenDevice() {
    ChkHR(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice), "GetDefaultAudioEndpoint");

    deviceId.clear();
    LPWSTR id = nullptr;
    if (SUCCEEDED(audioDevice->GetId(&id)) && id) {
        deviceId = id;
        CoTaskMemFree(id);
    }

    audioClient = ActivateAudioClient(audioDevice, "AudioClient activation");
    waveFormat = GetAudioMixFormat(audioClient);

    systemSampleRate = static_cast<int>(waveFormat->nSamplesPerSec);
    sourceChannelCount = std::max(1, static_cast<int>(waveFormat->nChannels));
    const int nextChannelCount = std::min(sourceChannelCount, 2);

    ChkHR(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 30000, 0, waveFormat, nullptr),
           "AudioClient Initialize");
    ChkHR(audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient)), "IAudioCaptureClient");

    if (!opusEncoder || nextChannelCount != channelCount) {
        if (opusEncoder) opus_encoder_destroy(opusEncoder);
        opusEncoder = nullptr;
        channelCount = nextChannelCount;

        int err = OPUS_OK;
        opusEncoder = opus_encoder_create(kSampleRate, channelCount, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (err != OPUS_OK || !opusEncoder) {
            opusEncoder = nullptr;
            throw std::runtime_error("Opus encoder creation failed");
        }
        opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(96000));
        opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(3));
        opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    }

    ConfigureDownmix();
    encodeBuffer.resize(kFrameSamples * channelCount);
    resampler = std::make_unique<SpeexResampler>(systemSampleRate, kSampleRate, channelCount);
    resamplerReadPos = 0;

    LOG("Audio: %dHz -> %dHz, %dch -> %dch", systemSampleRate, kSampleRate, sourceChannelCount, channelCount);
}

void AudioCapture::CloseDevice() {
    if (audioClient) audioClient->Stop();
    if (waveFormat) CoTaskMemFree(waveFormat);
    waveFormat = nullptr;
    SafeRelease(captureClient, audioClient, audioDevice);
}

bool AudioCapture::ReopenDevice(const char* reason) {
    LOG("AudioCapture: Reopening output device (%s)", reason);
    CloseDevice();
    try {
        OpenDevice();
        if (running.load(std::memory_order_acquire)) ChkHR(audioClient->Start(), "IAudioClient::Start");
    } catch (const std::exception& e) {
        WARN("AudioCapture: Reopen failed: %s", e.what());
        CloseDevice();
        return false;
    }
    ClearQueue(packetQueue, queueMutex);
    LOG("AudioCapture: Output device reopened");
    return true;
}

bool AudioCapture::DefaultDeviceChanged() {
    IMMDevice* current = nullptr;
    if (FAILED(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current)) || !current) return false;
    bool changed = false;
    LPWSTR id = nullptr;
    if (SUCCEEDED(current->GetId(&id)) && id) {
        changed = deviceId != id;
        CoTaskMemFree(id);
    }
    current->Release();
    return changed;
}

void AudioCapture::ConfigureDownmix() {
    downmixGainL.assign(sourceChannelCount, 0.0f);
    downmixGainR.assign(sourceChannelCount, 0.0f);
    if (sourceChannelCount <= 2) return;

    // KSAUDIO speaker position bits (mmreg.h), in channel order.
    constexpr DWORD kFrontLeft = 0x1, kFrontRight = 0x2, kFrontCenter = 0x4, kLowFrequency = 0x8;
    constexpr DWORD kLeftSide = 0x10 | 0x40 | 0x200 | 0x1000 | 0x8000;    // back/front-of-center/side/top-front/top-back left
    constexpr DWORD kRightSide = 0x20 | 0x80 | 0x400 | 0x4000 | 0x20000;  // matching right positions
    constexpr float kMix = 0.7071f;

    DWORD mask = 0;
    if (waveFormat && waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && waveFormat->cbSize >= 22) {
        mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(waveFormat)->dwChannelMask;
    }

    int channel = 0;
    for (DWORD bit = 1; mask && bit && channel < sourceChannelCount; bit <<= 1) {
        if (!(mask & bit)) continue;
        if (bit == kFrontLeft) downmixGainL[channel] = 1.0f;
        else if (bit == kFrontRight) downmixGainR[channel] = 1.0f;
        else if (bit == kLowFrequency) {}
        else if (bit == kFrontCenter) downmixGainL[channel] = downmixGainR[channel] = kMix;
        else if (bit & kLeftSide) downmixGainL[channel] = kMix;
        else if (bit & kRightSide) downmixGainR[channel] = kMix;
        else downmixGainL[channel] = downmixGainR[channel] = 0.5f;  // back/top centers
        channel++;
    }

    // No mask (or fewer bits than channels): assume FL, FR, FC, LFE, then left/right pairs.
    for (; channel < sourceChannelCount; channel++) {
        if (channel == 0) downmixGainL[channel] = 1.0f;
        else if (channel == 1) downmixGainR[channel] = 1.0f;
        else if (channel == 2) downmixGainL[channel] = downmixGainR[channel] = kMix;
        else if (channel == 3) {}
        else if (channel % 2 == 0) downmixGainL[channel] = kMix;
        else downmixGainR[channel] = kMix;
    }
}

const float* AudioCapture::Downmix(const float* data, UINT32 frames) {
    if (sourceChannelCount <= 2) return data;

    downmixBuffer.resize(static_cast<size_t>(frames) * 2);
    for (UINT32 i = 0; i < frames; i++) {
        const float* in = data + static_cast<size_t>(i) * sourceChannelCount;
        float left = 0.0f, right = 0.0f;
        for (int c = 0; c < sourceChannelCount; c++) {
            left += in[c] * downmixGainL[c];
            right += in[c] * downmixGainR[c];
        }
        downmixBuffer[i * 2] = left;
        downmixBuffer[i * 2 + 1] = right;
    }
    return downmixBuffer.data();
}

void AudioCapture::Loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    using Clock = std::chrono::steady_clock;
    std::vector<float> silBuf;
    bool deviceLost = captureClient == nullptr;
    Clock::time_point lastDeviceCheck = Clock::now();
    Clock::time_point lastReopenAttempt{};

    while (running.load(std::memory_order_acquire)) {
        if (resetRequested.exchange(false, std::memory_order_acq_rel) && resampler) {
            ResetResamplerState(resampler, resamplerReadPos);
        }

        const auto now = Clock::now();
        if (deviceLost) {
            if (now - lastReopenAttempt >= 2s) {
                lastReopenAttempt = now;
                deviceLost = !ReopenDevice("device unavailable");
                lastDeviceCheck = now;
            }
            std::this_thread::sleep_for(10ms);
            continue;
        }

        // Endpoint swaps (headset plugged in, RDP/virtual device, user picks a new default)
        // leave loopback capturing a device nobody hears; follow the default endpoint.
        if (now - lastDeviceCheck >= 1s) {
            lastDeviceCheck = now;
            if (DefaultDeviceChanged()) {
                deviceLost = !ReopenDevice("default output device changed");
                lastReopenAttempt = now;
                continue;
            }
        }

        if (!captureActive.load() || !captureClient || !isInitialized.load()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        UINT32 packetLength = 0;
        HRESULT packetHr = captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(packetHr)) {
            ERR("AudioCapture: GetNextPacketSize failed: 0x%08X", packetHr);
            deviceLost = true;
            continue;
        }

        while (packetLength > 0 && running.load() && captureActive.load() && captureClient) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            HRESULT bufHr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(bufHr)) {
                ERR("AudioCapture: GetBuffer failed: 0x%08X", bufHr);
                deviceLost = true;
                break;
            }

            if (data && frames > 0) {
                const float* src;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silBuf.assign(static_cast<size_t>(frames) * sourceChannelCount, 0.0f);
                    src = silBuf.data();
                } else {
                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                        WARN("AudioCapture: Data discontinuity detected (frames=%u) - possible audio glitch", frames);
                    }
                    src = reinterpret_cast<const float*>(data);
                }
                if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
                    WARN("AudioCapture: Timestamp error flag set (frames=%u)", frames);
                }
                Process(Downmix(src, frames), frames, GetTimestamp());
            } else if (frames == 0) {
                DBG("AudioCapture: GetBuffer returned 0 frames (flags=0x%X)", flags);
            }

            HRESULT relHr = captureClient->ReleaseBuffer(frames);
            if (FAILED(relHr)) {
                ERR("AudioCapture: ReleaseBuffer failed: 0x%08X (frames=%u)", relHr, frames);
                deviceLost = true;
                break;
            }
            HRESULT nextHr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(nextHr)) {
                ERR("AudioCapture: GetNextPacketSize failed in loop: 0x%08X", nextHr);
                deviceLost = true;
                break;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    CoUninitialize();
}

void AudioCapture::Process(const float* data, UINT32 frames, int64_t ts) {
    if (!opusEncoder || !resampler || !streaming.load()) {
        if (resampler && !streaming.load()) {
            resampler->buf.clear();
            resamplerReadPos = 0;
        }
        return;
    }

    resampler->Process(data, frames);

    const size_t maxBuf = kFrameSamples * channelCount * 6;
    LimitResamplerBuffer(resampler->buf, resamplerReadPos, maxBuf, kFrameSamples * channelCount * 2, channelCount,
        [&](size_t dropSamples, size_t bufferedSamples, size_t limitSamples, int channels) {
        WARN("AudioCapture: Resampler buffer overflow, dropping %zu samples (buf=%zu, max=%zu)",
            dropSamples / channels, bufferedSamples / channels, limitSamples / channels);
        });

    const size_t frameSamples = static_cast<size_t>(kFrameSamples * channelCount);
    while (resampler->buf.size() >= resamplerReadPos + frameSamples) {
        const float* src = resampler->buf.data() + resamplerReadPos;
        for (int i = 0; i < kFrameSamples * channelCount; i++)
            encodeBuffer[i] = static_cast<int16_t>(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f);
        resamplerReadPos += frameSamples;

        int bytes = opus_encode(opusEncoder, encodeBuffer.data(), kFrameSamples, opusBuffer.data(), static_cast<opus_int32>(opusBuffer.size()));
        if (bytes < 0) {
            ERR("AudioCapture: Opus encode error: %s (code=%d)", opus_strerror(bytes), bytes);
            continue;
        }
        if (bytes == 0) {
            DBG("AudioCapture: Opus encode returned 0 bytes (DTX silence?)");
            continue;
        }

        std::lock_guard<std::mutex> lk(queueMutex);
        PushBoundedQueue(packetQueue, kMaxQueueSize, AudioPacket{{opusBuffer.begin(), opusBuffer.begin() + bytes}, ts, kFrameSamples});
        queueCv.notify_one();
    }

    TrimConsumedResamplerBuffer(resampler->buf, resamplerReadPos);
}

void AudioCapture::Start() {
    if (running.load() || !isInitialized.load()) {
        WARN("AudioCapture: Start called but running=%d init=%d", running.load() ? 1 : 0, isInitialized.load() ? 1 : 0);
        return;
    }
    running = true;
    captureActive = true;
    if (resampler) ResetResamplerState(resampler, resamplerReadPos);
    if (audioClient) {
        HRESULT startHr = audioClient->Start();
        if (FAILED(startHr)) {
            // The loop treats a missing client as a lost device and retries.
            ERR("AudioCapture: IAudioClient::Start failed: 0x%08X", startHr);
            CloseDevice();
        }
    }
    captureThread = std::thread(&AudioCapture::Loop, this);
    LOG("AudioCapture: Started (rate=%d, ch=%d)", systemSampleRate, channelCount);
}

void AudioCapture::Stop() {
    if (!running.load()) return;
    running = false;
    captureActive = false;
    streaming = false;
    queueCv.notify_all();
    if (captureThread.joinable()) captureThread.join();
    if (audioClient) audioClient->Stop();
    LOG("AudioCapture: Stopped");
}

void AudioCapture::SetStreaming(bool s) {
    bool was = streaming.exchange(s);
    if (s != was) {
        LOG("AudioCapture: Streaming %s -> %s", was ? "on" : "off", s ? "on" : "off");
    }
    if (s && !was) {
        // The resampler belongs to the capture thread; ask it to reset rather than racing it.
        ClearQueue(packetQueue, queueMutex);
        resetRequested.store(true, std::memory_order_release);
    }
}

bool AudioCapture::PopPacket(AudioPacket& out, int ms) {
    std::unique_lock<std::mutex> lk(queueMutex);
    if (!queueCv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !packetQueue.empty() || !running.load(); }))
        return false;
    if (packetQueue.empty()) return false;
    out = std::move(packetQueue.front());
    packetQueue.pop();
    return true;
}

// ═══════════════════════════════════════════════════════════════
// MicPlayback — Opus decode → resample → WASAPI render
// ═══════════════════════════════════════════════════════════════

IMMDevice* MicPlayback::FindDevice(const std::string& name) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT cnt = 0; col->GetCount(&cnt);
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    std::string tl = lower(name);
    for (UINT i = 0; i < cnt; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d))) continue;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &var))) {
                char dn[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, dn, sizeof(dn), nullptr, nullptr);
                PropVariantClear(&var);
                if (lower(dn).find(tl) != std::string::npos) {
                    ps->Release(); col->Release();
                    actualDeviceName = dn; return d;
                }
            }
            ps->Release();
        }
        d->Release();
    }
    col->Release();
    WARN("MicPlayback: Device '%s' not found", name.c_str());
    return nullptr;
}

void MicPlayback::Loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        ERR("MicPlayback: CoInitializeEx failed: 0x%08X", hr);
        return;
    }

    std::vector<float> decodedFloat(kFrameSamples);

    while (running.load(std::memory_order_acquire)) {
        if (resetRequested.exchange(false, std::memory_order_acq_rel)) {
            ResetResamplerState(resampler, resamplerReadPos);
        }
        if (!streaming.load(std::memory_order_acquire) || !renderClient || !init.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        std::vector<uint8_t> pkt;
        {
            std::unique_lock<std::mutex> lk(queueMutex);
            if (!queueCv.wait_for(lk, 5ms, [this] { return !packetQueue.empty() || !running.load(std::memory_order_acquire); }))
                continue;
            if (!running.load(std::memory_order_acquire) || packetQueue.empty()) continue;
            pkt = std::move(packetQueue.front());
            packetQueue.pop();
        }

        if (pkt.size() < sizeof(MicPacketHeader)) continue;

        auto* h = reinterpret_cast<MicPacketHeader*>(pkt.data());
        if (h->magic != MSG_MIC_DATA || h->dataLength > pkt.size() - sizeof(MicPacketHeader)) continue;

        packetsReceived++;

        int ds = opus_decode(opusDecoder, pkt.data() + sizeof(MicPacketHeader), h->dataLength, decodeBuffer.data(), kFrameSamples, 0);
        if (ds <= 0) {
            if (++decodeErrors % 100 == 1)
                WARN("MicPlayback: Opus decode error: %s (total: %llu)", opus_strerror(ds), decodeErrors.load());
            continue;
        }

        packetsDecoded++;
        for (int i = 0; i < ds; i++) decodedFloat[i] = decodeBuffer[i] / 32768.0f;
        resampler->ProcessMono(decodedFloat.data(), ds, channelCount);

        for (int attempts = 0; resampler->buf.size() > resamplerReadPos && running.load(std::memory_order_acquire) && attempts < 50; attempts++) {
            UINT32 bufFr = 0, pad = 0;
            if (FAILED(audioClient->GetBufferSize(&bufFr)) || FAILED(audioClient->GetCurrentPadding(&pad))) break;

            UINT32 avail = bufFr - pad;
            if (avail == 0) { std::this_thread::sleep_for(1ms); continue; }

            const size_t bufferedSamples = resampler->buf.size() - resamplerReadPos;
            UINT32 toW = std::min(avail, static_cast<UINT32>(bufferedSamples / channelCount));
            if (toW == 0) break;

            BYTE* buf = nullptr;
            if (FAILED(renderClient->GetBuffer(toW, &buf))) break;

            float* out = reinterpret_cast<float*>(buf);
            const float* src = resampler->buf.data() + resamplerReadPos;
            std::memcpy(out, src, static_cast<size_t>(toW) * channelCount * sizeof(float));

            if (FAILED(renderClient->ReleaseBuffer(toW, 0))) break;
            resamplerReadPos += static_cast<size_t>(toW) * channelCount;
            samplesWritten += toW;
        }

        LimitResamplerBuffer(
            resampler->buf,
            resamplerReadPos,
            static_cast<size_t>(kFrameSamples * channelCount * 10),
            static_cast<size_t>(kFrameSamples * channelCount * 4),
            channelCount,
            [&](size_t dropSamples, size_t, size_t, int channels) {
            bufferOverruns++;
            DBG("MicPlayback: Buffer overrun, dropped %zu samples", dropSamples / channels);
            });

        TrimConsumedResamplerBuffer(resampler->buf, resamplerReadPos);
    }
    CoUninitialize();
    DBG("MicPlayback: Loop thread exiting");
}

MicPlayback::MicPlayback(const std::string& deviceName) : targetDevice(deviceName) {
    InitCOM();

    deviceEnumerator = CreateDeviceEnumerator();

    audioDevice = FindDevice(targetDevice);
    if (!audioDevice) {
        LOG("MicPlayback: '%s' not found, using default", targetDevice.c_str());
        ChkHR(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audioDevice), "GetDefaultAudioEndpoint");
        actualDeviceName = "(default output)";
    }

    audioClient = ActivateAudioClient(audioDevice, "IAudioClient");
    waveFormat = GetAudioMixFormat(audioClient);

    deviceSampleRate = waveFormat->nSamplesPerSec;
    channelCount = waveFormat->nChannels;
    DBG("MicPlayback: Device format: %dHz, %dch", deviceSampleRate, channelCount);

    HRESULT hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 300000, 0, waveFormat, nullptr);
    if (FAILED(hr)) {
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 300000, 0, waveFormat, nullptr);
        if (FAILED(hr)) throw std::runtime_error("AudioClient Initialize failed");
        WARN("MicPlayback: Initialized without auto-conversion");
    }

    ChkHR(audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient)), "IAudioRenderClient");

    int err;
    opusDecoder = opus_decoder_create(kSampleRate, 1, &err);
    if (err != OPUS_OK) throw std::runtime_error("Opus decoder creation failed");

    decodeBuffer.resize(kFrameSamples);
    resampler = std::make_unique<SpeexResampler>(kSampleRate, deviceSampleRate, 1);
    init.store(true, std::memory_order_release);
    LOG("MicPlayback: %dHz -> %dHz, %dch, device: %s", kSampleRate, deviceSampleRate, channelCount, actualDeviceName.c_str());
    CoUninitialize();
}

MicPlayback::~MicPlayback() {
    Stop();
    init.store(false, std::memory_order_release);
    LOG("MicPlayback: Stats - recv:%llu dec:%llu err:%llu written:%llu overruns:%llu",
        packetsReceived.load(), packetsDecoded.load(), decodeErrors.load(),
        samplesWritten.load(), bufferOverruns.load());
    if (opusDecoder) opus_decoder_destroy(opusDecoder);
    if (waveFormat) CoTaskMemFree(waveFormat);
    SafeRelease(renderClient, audioClient, audioDevice, deviceEnumerator);
}

void MicPlayback::Start() {
    if (running.load(std::memory_order_acquire) || !init.load(std::memory_order_acquire)) return;
    running.store(true, std::memory_order_release);
    ResetResamplerState(resampler, resamplerReadPos);
    if (FAILED(audioClient->Start())) { ERR("MicPlayback: IAudioClient::Start failed"); running.store(false, std::memory_order_release); return; }
    playbackThread = std::thread(&MicPlayback::Loop, this);
    LOG("MicPlayback: Started");
}

void MicPlayback::Stop() {
    if (!running.load(std::memory_order_acquire)) return;
    running.store(false, std::memory_order_release);
    streaming.store(false, std::memory_order_release);
    queueCv.notify_all();
    if (playbackThread.joinable()) playbackThread.join();
    if (audioClient) audioClient->Stop();
    ClearQueue(packetQueue, queueMutex);
    LOG("MicPlayback: Stopped");
}

void MicPlayback::SetStreaming(bool s) {
    bool was = streaming.exchange(s, std::memory_order_acq_rel);
    if (s != was) {
        // The resampler belongs to the playback thread; ask it to reset rather than racing it.
        ClearQueue(packetQueue, queueMutex);
        resetRequested.store(true, std::memory_order_release);
    }
}

void MicPlayback::PushPacket(const uint8_t* data, size_t len) {
    if (!streaming.load(std::memory_order_acquire) || len < sizeof(MicPacketHeader)) {
        if (len < sizeof(MicPacketHeader) && len > 0) {
            WARN("MicPlayback: PushPacket too small (%zu < %zu)", len, sizeof(MicPacketHeader));
        }
        return;
    }
    std::lock_guard<std::mutex> lk(queueMutex);
    if (packetQueue.size() >= kMaxQueueSize) {
        DBG("MicPlayback: Queue full, oldest packet will be dropped (queue=%zu)", packetQueue.size());
    }
    PushBoundedQueue(packetQueue, kMaxQueueSize, std::vector<uint8_t>(data, data + len));
    queueCv.notify_one();
}
