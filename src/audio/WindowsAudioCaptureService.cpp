#include "audio/WindowsAudioCaptureService.h"

#include <windows.h>
#include <Audioclient.h>
#include <propkeydef.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Mmdeviceapi.h>
#include <avrt.h>
#include <ksmedia.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace audio_assist::capture {

using Microsoft::WRL::ComPtr;

namespace {

class ScopedCoInitialize {
public:
    ScopedCoInitialize() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    ~ScopedCoInitialize() {
        if (hr_ == S_OK || hr_ == S_FALSE) {
            CoUninitialize();
        }
    }

    HRESULT result() const { return hr_; }

private:
    HRESULT hr_;
};

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

std::wstring readFriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return L"Unknown Device";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result = L"Unknown Device";
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    return result;
}

std::wstring getDefaultRenderDeviceId(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return L"";
    }

    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) {
        return L"";
    }

    std::wstring device_id = id;
    CoTaskMemFree(id);
    return device_id;
}

std::vector<float> convertToFloat(const BYTE* data, std::uint32_t frames, const WAVEFORMATEX& format) {
    const auto channels = static_cast<std::uint16_t>(format.nChannels);
    std::vector<float> output(static_cast<std::size_t>(frames) * channels, 0.0f);

    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) {
        const auto* source = reinterpret_cast<const float*>(data);
        std::copy(source, source + output.size(), output.begin());
        return output;
    }

    if (format.wFormatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) {
        const auto* source = reinterpret_cast<const std::int16_t*>(data);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output[i] = static_cast<float>(source[i]) / 32768.0f;
        }
        return output;
    }

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
        if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && format.wBitsPerSample == 32) {
            const auto* source = reinterpret_cast<const float*>(data);
            std::copy(source, source + output.size(), output.begin());
            return output;
        }

        if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && format.wBitsPerSample == 16) {
            const auto* source = reinterpret_cast<const std::int16_t*>(data);
            for (std::size_t i = 0; i < output.size(); ++i) {
                output[i] = static_cast<float>(source[i]) / 32768.0f;
            }
            return output;
        }
    }

    return {};
}

}  // namespace

struct WindowsAudioCaptureService::StreamState {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture_client;
    WAVEFORMATEX* mix_format = nullptr;
    std::wstring active_device_id;
    std::wstring active_device_name;
    UINT32 buffer_frames = 0;
};

WindowsAudioCaptureService::WindowsAudioCaptureService() = default;

WindowsAudioCaptureService::~WindowsAudioCaptureService() {
    stop();
}

bool WindowsAudioCaptureService::initialize(const std::wstring& device_id, AudioCaptureMode mode) {
    configured_device_id_ = device_id;
    mode_ = mode;
    initialized_.store(mode == AudioCaptureMode::EndpointLoopback);

    std::scoped_lock lock(metrics_mutex_);
    metrics_ = {};
    metrics_.output_sample_rate = target_sample_rate_;
    metrics_.active_device_name = L"";
    metrics_.device_healthy = false;
    return initialized_.load();
}

bool WindowsAudioCaptureService::start() {
    if (!initialized_.load() || running_.exchange(true)) {
        return initialized_.load();
    }

    capture_thread_ = std::thread(&WindowsAudioCaptureService::captureLoop, this);
    return true;
}

void WindowsAudioCaptureService::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    mono_frames_.clear();
    stereo_frames_.clear();

    std::scoped_lock lock(resample_mutex_);
    resample_source_.clear();
    pending_output_.clear();
    resample_position_ = 0.0;
}

std::optional<CaptureAudioFrame> WindowsAudioCaptureService::getMonoFrame() {
    return mono_frames_.pop();
}

std::optional<CaptureAudioFrame> WindowsAudioCaptureService::getStereoFrame() {
    return stereo_frames_.pop();
}

AudioCaptureMetrics WindowsAudioCaptureService::getMetrics() const {
    std::scoped_lock lock(metrics_mutex_);
    return metrics_;
}

std::vector<CaptureDeviceInfo> WindowsAudioCaptureService::listOutputDevices() {
    ScopedCoInitialize coinit;
    if (FAILED(coinit.result()) && coinit.result() != RPC_E_CHANGED_MODE) {
        return {};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return {};
    }

    const auto default_id = getDefaultRenderDeviceId(enumerator.Get());

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return {};
    }

    UINT count = 0;
    collection->GetCount(&count);

    std::vector<CaptureDeviceInfo> devices;
    devices.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }

        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) {
            continue;
        }

        CaptureDeviceInfo info;
        info.id = id;
        info.name = readFriendlyName(device.Get());
        info.is_default = (info.id == default_id);
        devices.push_back(std::move(info));
        CoTaskMemFree(id);
    }

    return devices;
}

bool WindowsAudioCaptureService::openStream(StreamState& state) {
    closeStream(state);

    const auto fail = [&]() {
        closeStream(state);
        return false;
    };

    if (mode_ != AudioCaptureMode::EndpointLoopback) {
        std::wcerr << L"Application loopback is not wired in this prototype yet." << std::endl;
        return false;
    }

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&state.enumerator)))) {
        return fail();
    }

    state.active_device_id = resolveDeviceId();
    if (state.active_device_id.empty()) {
        return fail();
    }

    if (FAILED(state.enumerator->GetDevice(state.active_device_id.c_str(), &state.device))) {
        return fail();
    }

    state.active_device_name = readFriendlyName(state.device.Get());

    if (FAILED(state.device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(state.client.GetAddressOf())))) {
        return fail();
    }

    if (FAILED(state.client->GetMixFormat(&state.mix_format)) || state.mix_format == nullptr) {
        return fail();
    }

    constexpr REFERENCE_TIME buffer_duration = 1000000;
    HRESULT hr = state.client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        buffer_duration,
        0,
        state.mix_format,
        nullptr);
    if (FAILED(hr)) {
        return fail();
    }

    if (FAILED(state.client->GetBufferSize(&state.buffer_frames))) {
        return fail();
    }

    if (FAILED(state.client->GetService(IID_PPV_ARGS(&state.capture_client)))) {
        return fail();
    }

    {
        std::scoped_lock lock(metrics_mutex_);
        metrics_.input_sample_rate = state.mix_format->nSamplesPerSec;
        metrics_.output_sample_rate = target_sample_rate_;
        metrics_.active_device_name = state.active_device_name;
        metrics_.device_healthy = true;
    }

    if (FAILED(state.client->Start())) {
        return fail();
    }

    return true;
}

void WindowsAudioCaptureService::closeStream(StreamState& state) {
    if (state.client) {
        state.client->Stop();
    }
    if (state.mix_format != nullptr) {
        CoTaskMemFree(state.mix_format);
        state.mix_format = nullptr;
    }
    state.capture_client.Reset();
    state.client.Reset();
    state.device.Reset();
    state.enumerator.Reset();

    std::scoped_lock lock(metrics_mutex_);
    metrics_.device_healthy = false;
}

void WindowsAudioCaptureService::captureLoop() {
    ScopedCoInitialize coinit;
    if (FAILED(coinit.result()) && coinit.result() != RPC_E_CHANGED_MODE) {
        running_.store(false);
        return;
    }

    DWORD task_index = 0;
    HANDLE avrt_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    auto last_default_check = std::chrono::steady_clock::now();
    StreamState state;

    while (running_.load()) {
        if (!state.client && !openStream(state)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        UINT32 packet_frames = 0;
        HRESULT hr = state.capture_client->GetNextPacketSize(&packet_frames);
        if (FAILED(hr)) {
            closeStream(state);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        while (packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames_available = 0;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            hr = state.capture_client->GetBuffer(
                &data,
                &frames_available,
                &flags,
                &device_position,
                &qpc_position);
            if (FAILED(hr)) {
                closeStream(state);
                break;
            }

            const auto channels = static_cast<std::uint16_t>(std::max<WORD>(state.mix_format->nChannels, 1));
            std::vector<float> chunk;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == AUDCLNT_BUFFERFLAGS_SILENT) {
                chunk.assign(static_cast<std::size_t>(frames_available) * channels, 0.0f);
            } else {
                chunk = convertToFloat(data, frames_available, *state.mix_format);
            }

            if (!chunk.empty()) {
                processChunk(chunk.data(), frames_available, channels);
            }

            state.capture_client->ReleaseBuffer(frames_available);

            {
                std::scoped_lock lock(metrics_mutex_);
                metrics_.capture_latency_ms =
                    static_cast<double>(state.buffer_frames) * 1000.0 / static_cast<double>(state.mix_format->nSamplesPerSec);
            }

            hr = state.capture_client->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                closeStream(state);
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const bool using_default = configured_device_id_.empty();
        if (using_default && state.enumerator &&
            now - last_default_check > std::chrono::seconds(1) &&
            getDefaultRenderDeviceId(state.enumerator.Get()) != state.active_device_id) {
            closeStream(state);
            last_default_check = now;
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    closeStream(state);

    if (avrt_handle != nullptr) {
        AvRevertMmThreadCharacteristics(avrt_handle);
    }
}

void WindowsAudioCaptureService::processChunk(const float* interleaved, std::uint32_t frames, std::uint16_t channels) {
    const auto input_rate = getMetrics().input_sample_rate;
    if (input_rate == 0) {
        return;
    }

    const double step = static_cast<double>(input_rate) / static_cast<double>(target_sample_rate_);

    {
        std::scoped_lock lock(resample_mutex_);

        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::uint16_t channel = 0; channel < 2; ++channel) {
                const auto source_channel = std::min<std::uint16_t>(channel, channels - 1);
                resample_source_.push_back(interleaved[frame * channels + source_channel]);
            }
        }

        const auto source_frames = resample_source_.size() / 2;
        while (source_frames >= 2 && resample_position_ + 1.0 < static_cast<double>(source_frames)) {
            const auto left_index = static_cast<std::size_t>(std::floor(resample_position_));
            const auto right_index = left_index + 1;
            const float alpha = static_cast<float>(resample_position_ - static_cast<double>(left_index));

            for (std::size_t channel = 0; channel < 2; ++channel) {
                const float left = resample_source_[left_index * 2 + channel];
                const float right = resample_source_[right_index * 2 + channel];
                pending_output_.push_back(left + (right - left) * alpha);
            }

            resample_position_ += step;
        }

        const auto discard_frames =
            std::min<std::size_t>(static_cast<std::size_t>(resample_position_), source_frames > 0 ? source_frames - 1 : 0);
        if (discard_frames > 0) {
            resample_source_.erase(resample_source_.begin(), resample_source_.begin() + discard_frames * 2);
            resample_position_ -= static_cast<double>(discard_frames);
        }
    }

    produceFrames();
}

void WindowsAudioCaptureService::produceFrames() {
    std::scoped_lock lock(resample_mutex_);
    const auto samples_per_stereo_frame = frame_samples_ * 2;

    while (pending_output_.size() >= samples_per_stereo_frame) {
        CaptureAudioFrame stereo;
        stereo.timestamp_ms = nowMs();
        stereo.sample_rate = target_sample_rate_;
        stereo.channels = 2;
        stereo.num_samples = static_cast<std::uint32_t>(frame_samples_);
        stereo.data.assign(pending_output_.begin(), pending_output_.begin() + samples_per_stereo_frame);

        CaptureAudioFrame mono;
        mono.timestamp_ms = stereo.timestamp_ms;
        mono.sample_rate = target_sample_rate_;
        mono.channels = 1;
        mono.num_samples = static_cast<std::uint32_t>(frame_samples_);
        mono.data.resize(frame_samples_);

        for (std::size_t i = 0; i < frame_samples_; ++i) {
            mono.data[i] = 0.5f * (stereo.data[i * 2] + stereo.data[i * 2 + 1]);
        }

        const bool stereo_ok = stereo_frames_.push(std::move(stereo));
        const bool mono_ok = mono_frames_.push(std::move(mono));

        {
            std::scoped_lock metrics_lock(metrics_mutex_);
            if (stereo_ok && mono_ok) {
                ++metrics_.pushed_frames;
            } else {
                ++metrics_.dropped_frames;
            }
        }

        pending_output_.erase(pending_output_.begin(), pending_output_.begin() + samples_per_stereo_frame);
    }
}

std::wstring WindowsAudioCaptureService::resolveDeviceId() const {
    if (!configured_device_id_.empty()) {
        return configured_device_id_;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return L"";
    }

    return getDefaultRenderDeviceId(enumerator.Get());
}

}  // namespace audio_assist::capture
