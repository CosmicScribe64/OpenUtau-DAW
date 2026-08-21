#include "openutau_vst/preview_tone.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

using openutau::vst::PreviewTone;

namespace {

void require(const bool condition) {
  if (!condition) std::abort();
}

} // namespace

int main() {
  PreviewTone tone;
  tone.setSampleRate(48000.0);
  tone.start(440.0);

  // Let the short attack finish before checking host-sized blocks. Once the
  // note is established, every block should carry a steady, continuous tone.
  for (auto frame = 0; frame < 1200; ++frame) {
    static_cast<void>(tone.nextSample());
  }
  require(tone.envelope() > 0.999f);

  constexpr std::array<int, 6> blockSizes{64, 128, 256, 512, 2048, 4096};
  for (const auto blockSize : blockSizes) {
    double energy = 0.0;
    for (auto frame = 0; frame < blockSize; ++frame) {
      const auto sample = tone.nextSample();
      energy += static_cast<double>(sample) * sample;
    }
    const auto rms = std::sqrt(energy / blockSize);
    require(rms > 0.04);
  }

  tone.release();
  for (auto frame = 0; frame < 1200; ++frame) {
    static_cast<void>(tone.nextSample());
  }
  require(tone.envelope() < 0.001f);

  tone.start(880.0);
  float peak = 0.0f;
  for (auto frame = 0; frame < 2400; ++frame) {
    peak = std::max(peak, std::abs(tone.nextSample()));
  }
  require(peak > 0.15f && peak <= 0.1601f);

  tone.startOneShot(330.0);
  peak = 0.0f;
  for (auto frame = 0; frame < 4800; ++frame) {
    peak = std::max(peak, std::abs(tone.nextSample()));
  }
  require(peak > 0.15f && tone.envelope() < 0.001f);
}
