#pragma once
#include <cstddef>
#include <vector>

namespace localstt {
struct Features {
    // Contiguous, mel-major layout: data[mel * frames + frame].
    std::vector<float> data;
    std::size_t n_mels = 0;
    std::size_t frames = 0;
};

// Matches faster_whisper.FeatureExtractor.__call__ for 16 kHz float audio:
// zero tail padding, centered reflect STFT, periodic Hann, Slaney mel filters,
// drop the final STFT frame, log10/8-decade clipping and Whisper normalization.
// Defaults intentionally include faster-whisper's 160-sample tail padding.
Features log_mel(const std::vector<float>& audio, int n_mels = 128,
                 int n_fft = 400, int hop_length = 160, int padding = 160);
}
