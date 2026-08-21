#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace openutau::vst {

class PreviewTone final {
public:
  void setSampleRate(const double sampleRate) noexcept {
    sampleRate_ = std::max(1.0, sampleRate);
  }

  void start(const double frequency) noexcept {
    frequency_ = std::max(1.0, frequency);
    phase_ = 0.0;
    envelope_ = 0.0f;
    autoReleaseFrames_ = 0;
    gated_ = true;
  }

  void startOneShot(const double frequency) noexcept {
    start(frequency);
    autoReleaseFrames_ = static_cast<std::size_t>(
        std::max(1.0, sampleRate_ * 0.050));
  }

  void release() noexcept {
    autoReleaseFrames_ = 0;
    gated_ = false;
  }

  void reset() noexcept {
    phase_ = 0.0;
    envelope_ = 0.0f;
    autoReleaseFrames_ = 0;
    gated_ = false;
  }

  [[nodiscard]] float nextSample() noexcept {
    if (autoReleaseFrames_ > 0 && --autoReleaseFrames_ == 0) gated_ = false;
    const auto envelopeStep = static_cast<float>(
        1.0 / std::max(1.0, sampleRate_ * 0.025));
    envelope_ = gated_
        ? std::min(1.0f, envelope_ + envelopeStep)
        : std::max(0.0f, envelope_ - envelopeStep);
    const auto sample = static_cast<float>(std::sin(phase_))
        * 0.16f * envelope_;
    constexpr double twoPi = 6.283185307179586476925286766559;
    phase_ += twoPi * frequency_ / sampleRate_;
    if (phase_ >= twoPi) phase_ -= twoPi;
    return sample;
  }

  [[nodiscard]] float envelope() const noexcept { return envelope_; }

private:
  double sampleRate_{44100.0};
  double frequency_{440.0};
  double phase_{};
  std::size_t autoReleaseFrames_{};
  float envelope_{};
  bool gated_{};
};

} // namespace openutau::vst
