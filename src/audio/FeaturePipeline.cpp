#include "audio/FeaturePipeline.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace audio_assist {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kEpsilon = 1.0e-6f;

double hzToMel(double hz) {
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}

double melToHz(double mel) {
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

}  // namespace

FeaturePipeline::FeaturePipeline() : mel_filter_bank_(buildMelFilterBank()) {
    analysis_window_.resize(stft_window_samples_);
    for (std::size_t i = 0; i < stft_window_samples_; ++i) {
        analysis_window_[i] =
            static_cast<float>(0.5 - 0.5 * std::cos((2.0 * kPi * static_cast<double>(i)) / (stft_window_samples_ - 1)));
    }
}

void FeaturePipeline::pushMonoFrame(const AudioFrame& frame) {
    if (frame.channels != 1 || frame.sample_rate != sample_rate_ || frame.data.empty()) {
        return;
    }

    std::scoped_lock lock(mutex_);

    rolling_mono_.insert(rolling_mono_.end(), frame.data.begin(), frame.data.end());
    total_samples_seen_ += frame.data.size();
    ++metrics_.frames_received;

    const auto keep_from_sample = total_samples_seen_ > max_buffer_samples_
        ? total_samples_seen_ - max_buffer_samples_
        : 0;
    const auto buffered_start_sample = total_samples_seen_ - rolling_mono_.size();
    if (keep_from_sample > buffered_start_sample) {
        const auto discard = static_cast<std::size_t>(keep_from_sample - buffered_start_sample);
        if (discard < rolling_mono_.size()) {
            rolling_mono_.erase(rolling_mono_.begin(), rolling_mono_.begin() + discard);
        } else {
            rolling_mono_.clear();
        }
    }

    metrics_.samples_buffered = rolling_mono_.size();
    produceAvailableTensors(frame);
}

std::optional<FeatureTensor> FeaturePipeline::getNextTensor() {
    return ready_tensors_.pop();
}

FeaturePipelineMetrics FeaturePipeline::getMetrics() const {
    std::scoped_lock lock(mutex_);
    return metrics_;
}

void FeaturePipeline::produceAvailableTensors(const AudioFrame& frame) {
    const auto buffered_start_sample = total_samples_seen_ - rolling_mono_.size();
    if (next_window_start_sample_ < buffered_start_sample) {
        next_window_start_sample_ = buffered_start_sample;
    }

    while (next_window_start_sample_ + model_window_samples_ <= total_samples_seen_) {
        const auto window_offset = static_cast<std::size_t>(next_window_start_sample_ - buffered_start_sample);
        FeatureTensor tensor = buildTensor(window_offset, frame.timestamp_ms);
        if (ready_tensors_.push(std::move(tensor))) {
            ++metrics_.tensors_produced;
        } else {
            ++metrics_.tensors_dropped;
        }
        next_window_start_sample_ += model_hop_samples_;
    }

    metrics_.samples_buffered = rolling_mono_.size();
}

FeatureTensor FeaturePipeline::buildTensor(std::size_t window_offset, std::uint64_t frame_timestamp_ms) const {
    FeatureTensor tensor;
    tensor.sample_rate = sample_rate_;
    tensor.mel_bins = mel_bins_;
    tensor.time_steps = time_steps_;
    tensor.data.resize(static_cast<std::size_t>(mel_bins_) * time_steps_);

    const auto buffered_start_sample = total_samples_seen_ - rolling_mono_.size();
    const auto window_start_sample = buffered_start_sample + window_offset;
    tensor.window_start_ms = (window_start_sample * 1000ULL) / sample_rate_;
    tensor.window_end_ms = tensor.window_start_ms + (model_window_samples_ * 1000ULL) / sample_rate_;

    for (std::size_t frame_index = 0; frame_index < time_steps_; ++frame_index) {
        const auto stft_offset = window_offset + frame_index * stft_hop_samples_;
        std::vector<float> windowed(fft_size_, 0.0f);

        for (std::size_t i = 0; i < stft_window_samples_; ++i) {
            windowed[i] = rolling_mono_[stft_offset + i] * analysis_window_[i];
        }

        const auto power_spectrum = computePowerSpectrum(windowed);
        const auto mel_energies = computeMelEnergies(power_spectrum);
        for (std::size_t mel = 0; mel < mel_energies.size(); ++mel) {
            tensor.data[mel * time_steps_ + frame_index] = std::log(mel_energies[mel] + kEpsilon);
        }
    }

    tensor.window_end_ms = std::max(tensor.window_end_ms, frame_timestamp_ms);
    return tensor;
}

std::vector<float> FeaturePipeline::computePowerSpectrum(const std::vector<float>& windowed) const {
    const std::size_t bins = (fft_size_ / 2) + 1;
    std::vector<float> power(bins, 0.0f);
    std::vector<std::complex<float>> spectrum(fft_size_);

    for (std::size_t i = 0; i < fft_size_; ++i) {
        spectrum[i] = std::complex<float>(windowed[i], 0.0f);
    }

    for (std::size_t i = 1, j = 0; i < fft_size_; ++i) {
        std::size_t bit = fft_size_ >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(spectrum[i], spectrum[j]);
        }
    }

    for (std::size_t len = 2; len <= fft_size_; len <<= 1) {
        const float angle = static_cast<float>(-2.0 * kPi / static_cast<double>(len));
        const std::complex<float> wlen(std::cos(angle), std::sin(angle));

        for (std::size_t i = 0; i < fft_size_; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            const std::size_t half = len >> 1;
            for (std::size_t j = 0; j < half; ++j) {
                const std::complex<float> u = spectrum[i + j];
                const std::complex<float> v = spectrum[i + j + half] * w;
                spectrum[i + j] = u + v;
                spectrum[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    for (std::size_t k = 0; k < bins; ++k) {
        power[k] = std::norm(spectrum[k]);
    }

    return power;
}

std::vector<float> FeaturePipeline::computeMelEnergies(const std::vector<float>& power_spectrum) const {
    std::vector<float> mel_energies(mel_bins_, 0.0f);
    for (std::size_t mel = 0; mel < mel_filter_bank_.size(); ++mel) {
        double energy = 0.0;
        for (std::size_t bin = 0; bin < power_spectrum.size(); ++bin) {
            energy += static_cast<double>(power_spectrum[bin]) * mel_filter_bank_[mel][bin];
        }
        mel_energies[mel] = static_cast<float>(energy);
    }
    return mel_energies;
}

std::vector<std::vector<float>> FeaturePipeline::buildMelFilterBank() const {
    const std::size_t spectrum_bins = (fft_size_ / 2) + 1;
    std::vector<std::vector<float>> bank(mel_bins_, std::vector<float>(spectrum_bins, 0.0f));

    const double min_mel = hzToMel(0.0);
    const double max_mel = hzToMel(static_cast<double>(sample_rate_) / 2.0);
    std::vector<double> mel_points(mel_bins_ + 2, 0.0);
    for (std::size_t i = 0; i < mel_points.size(); ++i) {
        mel_points[i] = min_mel + (max_mel - min_mel) * static_cast<double>(i) / static_cast<double>(mel_points.size() - 1);
    }

    std::vector<std::size_t> fft_bins(mel_bins_ + 2, 0);
    for (std::size_t i = 0; i < fft_bins.size(); ++i) {
        const double hz = melToHz(mel_points[i]);
        fft_bins[i] = std::min<std::size_t>(
            spectrum_bins - 1,
            static_cast<std::size_t>(std::floor((fft_size_ + 1) * hz / static_cast<double>(sample_rate_))));
    }

    for (std::size_t mel = 0; mel < mel_bins_; ++mel) {
        const auto left = fft_bins[mel];
        const auto center = fft_bins[mel + 1];
        const auto right = fft_bins[mel + 2];

        if (left >= center || center >= right) {
            continue;
        }

        for (std::size_t bin = left; bin < center; ++bin) {
            bank[mel][bin] = static_cast<float>(bin - left) / static_cast<float>(center - left);
        }
        for (std::size_t bin = center; bin < right; ++bin) {
            bank[mel][bin] = static_cast<float>(right - bin) / static_cast<float>(right - center);
        }
    }

    return bank;
}

}  // namespace audio_assist
