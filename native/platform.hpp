#pragma once
#include "common.hpp"
#include <memory>

namespace localstt {
// dwExtraInfo on every key LocalSTT sends itself; the same value localstt/text_input.py
// uses. The keyboard hook skips injected events carrying it, so typed text can never
// trigger a hotkey or the cancel key, while injected input from other tools still can.
constexpr ULONG_PTR input_marker = 0x4c535454;
struct Target { HWND window = nullptr; HWND control = nullptr; };
Target capture_target();
bool target_matches(Target target);
bool modifiers_down();
// Returns UTF-16 code units actually sent. Never changes the foreground window.
std::size_t type_text(Target target, std::wstring_view text, double interval_seconds);
bool clipboard_text(std::wstring_view text);
bool paste_text(Target target, std::wstring_view text, bool restore, double delay_seconds);

class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    void start(const Json& config);
    void stop();
    double duration() const;
    double tail_silence(double threshold, double max_seconds = 3) const;
    std::vector<float> snapshot(double offset = 0, double max_seconds = 0) const;
    // IDs are Windows waveIn device IDs, not sounddevice/PortAudio IDs.
    // Configure native_microphone; null selects the Windows default input.
    static Json devices();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The name and icon Windows puts on notifications. Toasts are attributed to the
// process AppUserModelID; this claims the one localstt/shell_identity.py uses and
// registers its display name under HKCU. Call before creating any window.
constexpr wchar_t app_user_model_id[] = L"iFrosta.LocalSTT";
bool apply_shell_identity(const fs::path& icon);

// Decode formats supported by the installed Windows Media Foundation codecs.
// Output is always 16 kHz, mono, float32. Throws on codec/format/read errors.
std::vector<float> decode_audio(const fs::path& file);
}
