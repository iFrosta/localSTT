#include "platform.hpp"
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <ole2.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace localstt {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t sample_rate = 16000;
constexpr std::size_t maximum_samples = sample_rate * 60 * 60 * 4; // Four hours.

void check_hr(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        throw std::runtime_error(message.str());
    }
}
void check_wave(MMRESULT result, const char* operation) {
    if (result != MMSYSERR_NOERROR) {
        wchar_t description[MAXERRORLENGTH]{};
        waveInGetErrorTextW(result, description, MAXERRORLENGTH);
        throw std::runtime_error(std::string(operation) + ": " + utf8(description));
    }
}
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComApartment() {
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) check_hr(result, "CoInitializeEx");
    }
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct MediaFoundation {
    MediaFoundation() { check_hr(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup"); }
    ~MediaFoundation() { MFShutdown(); }
};
struct OleApartment {
    HRESULT result = OleInitialize(nullptr);
    ~OleApartment() { if (SUCCEEDED(result)) OleUninitialize(); }
};
struct ClipboardLock {
    bool opened = false;
    explicit ClipboardLock(HWND owner) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (OpenClipboard(owner)) { opened = true; break; }
            Sleep(20);
        }
    }
    ~ClipboardLock() { if (opened) CloseClipboard(); }
};
struct ClipboardOwner {
    HWND window = CreateWindowExW(0, L"STATIC", L"LocalSTT Clipboard", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~ClipboardOwner() { if (window) DestroyWindow(window); }
};
void delay(double seconds) {
    if (std::isfinite(seconds) && seconds > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(seconds, 30.0)));
}
INPUT key(WORD vk, WORD scan, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scan;
    input.ki.dwFlags = flags;
    input.ki.dwExtraInfo = input_marker;
    return input;
}
}

Target capture_target() {
    Target result;
    result.window = GetForegroundWindow();
    if (result.window) {
        GUITHREADINFO info{sizeof(info)};
        if (GetGUIThreadInfo(GetWindowThreadProcessId(result.window, nullptr), &info))
            result.control = info.hwndFocus;
    }
    return result;
}

bool target_matches(Target target) {
    if (!target.window || !IsWindow(target.window)) return false;
    const auto current = capture_target();
    return current.window == target.window && (!target.control || current.control == target.control);
}

bool modifiers_down() {
    for (int key_code : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key_code) & 0x8000) return true;
    return false;
}

std::size_t type_text(Target target, std::wstring_view text, double interval_seconds) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        if (!target_matches(target) || modifiers_down()) break;
        std::array<INPUT, 4> inputs{};
        std::size_t units = 1;
        // Send surrogate pairs in a single SendInput call, never split them on focus loss.
        const wchar_t ch = text[sent];
        if (ch >= 0xd800 && ch <= 0xdbff && sent + 1 < text.size()
            && text[sent + 1] >= 0xdc00 && text[sent + 1] <= 0xdfff) units = 2;
        for (std::size_t i = 0; i < units; ++i) {
            inputs[2 * i] = key(0, text[sent + i], KEYEVENTF_UNICODE);
            inputs[2 * i + 1] = key(0, text[sent + i], KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
        }
        const UINT count = static_cast<UINT>(units * 2);
        const UINT accepted = SendInput(count, inputs.data(), sizeof(INPUT));
        if (accepted != count) {
            // A partial insertion must not leave the Unicode key held down.
            if (accepted % 2) {
                INPUT release = inputs[accepted];
                SendInput(1, &release, sizeof(INPUT));
            }
            // Count key-downs which may already have inserted characters, preventing duplicates.
            sent += (accepted + 1) / 2;
            break;
        }
        sent += units;
        delay(interval_seconds);
    }
    return sent;
}

bool clipboard_text(std::wstring_view text) {
    ClipboardOwner owner;
    if (!owner.window) return false;
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    auto destination = static_cast<wchar_t*>(GlobalLock(memory));
    if (!destination) { GlobalFree(memory); return false; }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    destination[text.size()] = 0;
    GlobalUnlock(memory);
    ClipboardLock lock(owner.window);
    if (!lock.opened || !EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        return false;
    }
    // Ownership of memory transfers to Windows.
    return true;
}

bool paste_text(Target target, std::wstring_view text, bool restore, double delay_seconds) {
    if (!target_matches(target) || modifiers_down()) return false;
    OleApartment apartment;
    ComPtr<IDataObject> previous;
    // Preserve all clipboard formats using OLE, not just text. If preservation is
    // impossible, leave the user's clipboard untouched and let the caller retry.
    if (restore && (FAILED(apartment.result) || FAILED(OleGetClipboard(&previous)))) return false;
    if (!clipboard_text(text)) return false;
    const DWORD sequence = GetClipboardSequenceNumber();
    bool sent = false;
    if (target_matches(target) && !modifiers_down()) {
        INPUT inputs[] = {key(VK_CONTROL, 0, 0), key('V', 0, 0),
                          key('V', 0, KEYEVENTF_KEYUP), key(VK_CONTROL, 0, KEYEVENTF_KEYUP)};
        sent = SendInput(4, inputs, sizeof(INPUT)) == 4;
        if (!sent) {
            INPUT release[] = {key('V', 0, KEYEVENTF_KEYUP), key(VK_CONTROL, 0, KEYEVENTF_KEYUP)};
            SendInput(2, release, sizeof(INPUT));
        }
    }
    if (restore) {
        if (sent) delay(delay_seconds);
        // Do not overwrite text the user copied while waiting for the paste.
        if (sequence == GetClipboardSequenceNumber()) {
            if (SUCCEEDED(OleSetClipboard(previous.Get()))) OleFlushClipboard();
            else log("Could not restore the previous clipboard after paste");
        }
    }
    return sent;
}

bool apply_shell_identity(const fs::path& icon) {
    // Registered first: the name has to resolve by the time the first balloon is sent.
    bool registered = false;
    HKEY key = nullptr;
    const std::wstring path = std::wstring(L"Software\\Classes\\AppUserModelId\\") + app_user_model_id;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const auto set = [&](const wchar_t* name, const std::wstring& value) {
            return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        };
        registered = set(L"DisplayName", L"LocalSTT");
        std::error_code ec;
        if (!icon.empty() && fs::exists(icon, ec)) set(L"IconUri", icon.wstring());
        RegCloseKey(key);
    }
    const bool claimed = SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(app_user_model_id));
    if (!registered || !claimed) log("Notification identity was not fully applied");
    return registered && claimed;
}

struct Recorder::Impl {
    mutable std::mutex samples_mutex;
    std::mutex lifecycle_mutex;
    std::vector<float> samples;
    std::string error;
    double normalize_peak = 0.35, max_gain = 25.0;
    HWAVEIN input = nullptr;
    HANDLE event = nullptr;
    std::atomic<bool> stopping{false};
    std::thread worker;
    static constexpr std::size_t block_frames = 1600;
    std::array<std::array<std::int16_t, block_frames>, 4> buffers{};
    std::array<WAVEHDR, 4> headers{};
    std::size_t next_header = 0;

    void harvest(bool requeue) {
        // Events can coalesce across the ring's wraparound. Consume in queue
        // order (3,0,1...), never array order, or recorded speech is reordered.
        for (std::size_t processed = 0; processed < headers.size(); ++processed) {
            auto& header = headers[next_header];
            if (!(header.dwFlags & WHDR_DONE)) break;
            const std::size_t frames = header.dwBytesRecorded / sizeof(std::int16_t);
            if (frames) {
                std::lock_guard lock(samples_mutex);
                if (samples.size() + frames > maximum_samples)
                    throw std::runtime_error("Recording exceeded four hours; stop and start a new recording");
                const auto* data = reinterpret_cast<const std::int16_t*>(header.lpData);
                for (std::size_t i = 0; i < frames; ++i) samples.push_back(data[i] / 32768.0f);
            }
            header.dwBytesRecorded = 0;
            if (requeue) check_wave(waveInAddBuffer(input, &header, sizeof(header)), "waveInAddBuffer");
            next_header = (next_header + 1) % headers.size();
        }
    }
    void run() noexcept {
        try {
            while (!stopping.load()) {
                const DWORD wait = WaitForSingleObject(event, 1000);
                if (wait == WAIT_FAILED) throw std::runtime_error("Microphone event wait failed");
                if (!stopping.load()) harvest(true);
            }
            check_wave(waveInStop(input), "waveInStop");
            check_wave(waveInReset(input), "waveInReset");
            harvest(false); // Include partial buffers recorded before Stop.
        } catch (const std::exception& e) {
            waveInReset(input);
            std::lock_guard lock(samples_mutex);
            error = e.what();
        }
    }
    void cleanup() noexcept {
        if (input) {
            waveInReset(input);
            for (auto& header : headers)
                if (header.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(input, &header, sizeof(header));
            waveInClose(input);
            input = nullptr;
        }
        if (event) { CloseHandle(event); event = nullptr; }
    }
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() { stop(); }

void Recorder::start(const Json& config) {
    std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (impl_->input) return;
    {
        std::lock_guard lock(impl_->samples_mutex);
        impl_->samples.clear();
        impl_->error.clear();
        impl_->normalize_peak = std::clamp(config.value("input_normalize_peak", 0.35), 0.0, 1.0);
        impl_->max_gain = std::clamp(config.value("input_max_gain", 25.0), 1.0, 1000.0);
    }
    impl_->stopping = false;
    impl_->next_header = 0;
    UINT device = WAVE_MAPPER;
    // Sounddevice indexes cannot be mapped reliably to Windows waveIn indexes.
    // Existing `microphone` is intentionally ignored; native_microphone starts at default.
    const auto selected = config.find("native_microphone");
    if (selected != config.end() && !selected->is_null()) {
        if (!selected->is_number_integer()) throw std::runtime_error("native_microphone must be an integer or null");
        const int index = selected->get<int>();
        if (index < 0 || static_cast<UINT>(index) >= waveInGetNumDevs())
            throw std::runtime_error("Selected native microphone is unavailable; select another input in Settings");
        device = static_cast<UINT>(index);
    }
    try {
        impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!impl_->event) throw std::runtime_error("Cannot create microphone event");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        check_wave(waveInOpen(&impl_->input, device, &format, reinterpret_cast<DWORD_PTR>(impl_->event),
                             0, CALLBACK_EVENT), "Open microphone");
        for (std::size_t i = 0; i < impl_->headers.size(); ++i) {
            auto& header = impl_->headers[i];
            header = {};
            header.lpData = reinterpret_cast<LPSTR>(impl_->buffers[i].data());
            header.dwBufferLength = static_cast<DWORD>(impl_->buffers[i].size() * sizeof(std::int16_t));
            check_wave(waveInPrepareHeader(impl_->input, &header, sizeof(header)), "waveInPrepareHeader");
            check_wave(waveInAddBuffer(impl_->input, &header, sizeof(header)), "waveInAddBuffer");
        }
        check_wave(waveInStart(impl_->input), "Start microphone");
        impl_->worker = std::thread([this] { impl_->run(); });
    } catch (...) { impl_->cleanup(); throw; }
}

void Recorder::stop() {
    std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (!impl_->input) return;
    impl_->stopping = true;
    SetEvent(impl_->event);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->cleanup();
}

double Recorder::duration() const {
    std::lock_guard lock(impl_->samples_mutex);
    if (!impl_->error.empty()) throw std::runtime_error(impl_->error);
    return static_cast<double>(impl_->samples.size()) / sample_rate;
}

std::vector<float> Recorder::snapshot(double offset, double max_seconds) const {
    if (!std::isfinite(offset) || !std::isfinite(max_seconds)) throw std::runtime_error("Invalid audio time range");
    std::lock_guard lock(impl_->samples_mutex);
    if (!impl_->error.empty()) throw std::runtime_error(impl_->error);
    const auto start = static_cast<std::size_t>(std::min(std::max(0.0, offset) * sample_rate,
                                                       static_cast<double>(impl_->samples.size())));
    std::size_t end = impl_->samples.size();
    if (max_seconds > 0)
        end = start + static_cast<std::size_t>(std::min(max_seconds * sample_rate, static_cast<double>(end - start)));
    std::vector<float> result(impl_->samples.begin() + start, impl_->samples.begin() + end);
    float peak = 0;
    for (float value : result) peak = std::max(peak, std::abs(value));
    const double gain = peak > 1e-5f && peak < impl_->normalize_peak
        ? std::min(impl_->normalize_peak / peak, impl_->max_gain) : 1.0;
    for (float& value : result) value = static_cast<float>(std::clamp(value * gain, -1.0, 1.0));
    return result;
}

double Recorder::tail_silence(double threshold, double max_seconds) const {
    if (!std::isfinite(threshold) || !std::isfinite(max_seconds) || max_seconds <= 0) return 0;
    std::lock_guard lock(impl_->samples_mutex);
    if (!impl_->error.empty()) throw std::runtime_error(impl_->error);
    constexpr std::size_t window = sample_rate / 20; // 50 ms RMS, matching the Python implementation.
    const auto limit = static_cast<std::size_t>(std::min(max_seconds * sample_rate,
                                                       static_cast<double>(impl_->samples.size())));
    std::size_t silent = 0;
    while (silent + window <= limit) {
        double energy = 0;
        const auto end = impl_->samples.size() - silent;
        for (std::size_t i = end - window; i < end; ++i) energy += impl_->samples[i] * impl_->samples[i];
        if (std::sqrt(energy / window) >= threshold) break;
        silent += window;
    }
    return static_cast<double>(silent) / sample_rate;
}

Json Recorder::devices() {
    Json result = Json::array();
    for (UINT i = 0; i < waveInGetNumDevs(); ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        std::wstring name = caps.szPname;
        for (auto& ch : name) if (ch == L'\n' || ch == L'\r' || ch == L'\t') ch = L' ';
        result.push_back({{"index", i}, {"name", utf8(name)}, {"default_samplerate", sample_rate}, {"api", "waveIn"}});
    }
    return result;
}

std::vector<float> decode_audio(const fs::path& file) {
    if (!fs::is_regular_file(file)) throw std::runtime_error("Audio file does not exist");
    ComApartment com;
    MediaFoundation mf;
    ComPtr<IMFSourceReader> reader;
    check_hr(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader), "Open audio file (Media Foundation)");
    check_hr(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), "Disable non-audio streams");
    check_hr(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE), "Select audio stream");
    ComPtr<IMFMediaType> type;
    check_hr(MFCreateMediaType(&type), "Create audio format");
    check_hr(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "Set audio type");
    check_hr(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float), "Set float samples");
    check_hr(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1), "Set mono");
    check_hr(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate), "Set 16 kHz");
    check_hr(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32), "Set float32");
    check_hr(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, sizeof(float)), "Set block alignment");
    check_hr(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sample_rate * sizeof(float)), "Set data rate");
    check_hr(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE), "Set sample independence");
    check_hr(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get()),
             "Decode and resample audio to 16 kHz mono");
    std::vector<float> result;
    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        check_hr(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample), "Read audio");
        if (flags & MF_SOURCE_READERF_ERROR) throw std::runtime_error("Audio decoder reported an error");
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            throw std::runtime_error("Audio format changed mid-file; convert the file to WAV first");
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            check_hr(sample->ConvertToContiguousBuffer(&buffer), "Get audio buffer");
            BYTE* bytes = nullptr;
            DWORD length = 0;
            check_hr(buffer->Lock(&bytes, nullptr, &length), "Lock audio buffer");
            try {
                if (length % sizeof(float)) throw std::runtime_error("Decoder returned a partial float sample");
                const std::size_t count = length / sizeof(float);
                if (result.size() + count > maximum_samples) throw std::runtime_error("Audio file exceeds the four-hour limit");
                const auto old_size = result.size();
                result.resize(old_size + count);
                std::memcpy(result.data() + old_size, bytes, length);
                for (auto i = old_size; i < result.size(); ++i)
                    result[i] = std::isfinite(result[i]) ? std::clamp(result[i], -1.0f, 1.0f) : 0.0f;
            } catch (...) { buffer->Unlock(); throw; }
            check_hr(buffer->Unlock(), "Unlock audio buffer");
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (result.empty()) throw std::runtime_error("Audio file contains no decoded samples");
    return result;
}
}
