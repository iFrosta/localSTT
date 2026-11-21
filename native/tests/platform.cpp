#include "platform.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace localstt;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void u16(std::ostream& out, unsigned value) {
    out.put(static_cast<char>(value & 255));
    out.put(static_cast<char>((value >> 8) & 255));
}
void u32(std::ostream& out, unsigned value) {
    u16(out, value & 65535); u16(out, value >> 16);
}
void write_stereo_wav(const fs::path& file) {
    std::ofstream out(file, std::ios::binary);
    constexpr unsigned rate = 48000;
    constexpr unsigned bytes = rate * 2 * 2;
    out.write("RIFF", 4); u32(out, 36 + bytes); out.write("WAVEfmt ", 8);
    u32(out, 16); u16(out, 1); u16(out, 2); u32(out, rate); u32(out, rate * 4);
    u16(out, 4); u16(out, 16); out.write("data", 4); u32(out, bytes);
    for (unsigned i = 0; i < rate; ++i) {
        const auto value = static_cast<short>(12000 * std::sin(2 * 3.141592653589793 * 440 * i / rate));
        u16(out, static_cast<unsigned short>(value));
        u16(out, static_cast<unsigned short>(value));
    }
    if (!out) throw std::runtime_error("Could not write test audio");
}
}
int main() {
    const auto root = fs::temp_directory_path() / (L"localstt-platform-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    try {
        const auto wave = root / L"audio-\u0442\u0435\u0441\u0442.wav";
        write_stereo_wav(wave);
        const auto samples = decode_audio(wave);
        require(samples.size() >= 15990 && samples.size() <= 16010, "48 kHz stereo must decode to one second of 16 kHz mono");
        double energy = 0;
        unsigned crossings = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            require(std::isfinite(samples[i]) && std::abs(samples[i]) <= 1, "Decoded samples must be finite and normalized");
            energy += samples[i] * samples[i];
            if (i && samples[i - 1] < 0 && samples[i] >= 0) ++crossings;
        }
        const double rms = std::sqrt(energy / samples.size());
        require(rms > 0.24 && rms < 0.28, "Downmix/resample should preserve waveform amplitude");
        require(crossings >= 438 && crossings <= 442, "Downsampling must preserve 440 Hz frequency");
        bool rejected = false;
        try { decode_audio(root / L"missing.wav"); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Missing audio must produce an error");
        { std::ofstream invalid(root / L"invalid.wav"); invalid << "not an audio file"; }
        rejected = false;
        try { decode_audio(root / L"invalid.wav"); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid audio must produce an error");
        require(!target_matches({}), "A missing target must never receive text");
        require(type_text({}, L"Do not type this", 0) == 0, "Typing must fail closed on a missing target");
        require(!paste_text({}, L"Do not copy this", false, 0), "Pasting must fail closed on a missing target");
        Recorder recorder;
        require(recorder.duration() == 0 && recorder.snapshot().empty(), "A new recorder must be empty");
        require(recorder.tail_silence(0.008) == 0, "An empty recorder must not report silence");
        rejected = false;
        try { recorder.start({{"native_microphone", -2}}); } catch (const std::exception&) { rejected = true; }
        require(rejected, "An invalid microphone must not open the default device");
        recorder.stop(); recorder.stop();
        require(Recorder::devices().is_array(), "Microphone enumeration must return an array");
        fs::remove_all(root);
        std::cout << "platform tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        fs::remove_all(root);
        std::cerr << "platform test failed: " << e.what() << '\n';
        return 1;
    }
}
