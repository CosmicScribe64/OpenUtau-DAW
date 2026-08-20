#include "openutau_vst/audio_ring_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace openutau::vst {

AudioRingBuffer::AudioRingBuffer(const std::size_t capacityFrames,
                                 const std::size_t channels)
    : capacityFrames_(capacityFrames), channels_(channels),
      mask_(capacityFrames - 1),
      samples_(std::make_unique<float[]>(capacityFrames * channels)),
      epochs_(std::make_unique<std::uint64_t[]>(capacityFrames)),
      positions_(std::make_unique<std::int64_t[]>(capacityFrames)) {
  if (capacityFrames < 2 || (capacityFrames & (capacityFrames - 1)) != 0) {
    throw std::invalid_argument("capacityFrames must be a power of two >= 2");
  }
  if (channels == 0) {
    throw std::invalid_argument("channels must be greater than zero");
  }
}

std::size_t AudioRingBuffer::capacityFrames() const noexcept { return capacityFrames_; }
std::size_t AudioRingBuffer::channels() const noexcept { return channels_; }

std::size_t AudioRingBuffer::readableFrames() const noexcept {
  const auto write = writePosition_.load(std::memory_order_acquire);
  const auto read = readPosition_.load(std::memory_order_relaxed);
  return static_cast<std::size_t>(write - read);
}

std::size_t AudioRingBuffer::writableFrames() const noexcept {
  return capacityFrames_ - readableFrames();
}

std::size_t AudioRingBuffer::write(const float* const interleaved,
                                   const std::size_t frames) noexcept {
  const auto start = static_cast<std::int64_t>(
      writePosition_.load(std::memory_order_relaxed));
  return writeTagged(interleaved, frames, 0, start);
}

std::size_t AudioRingBuffer::writeTagged(const float* const interleaved,
                                         const std::size_t frames,
                                         const std::uint64_t epoch,
                                         const std::int64_t startFrame) noexcept {
  const auto write = writePosition_.load(std::memory_order_relaxed);
  const auto read = readPosition_.load(std::memory_order_acquire);
  const auto count = std::min(frames, capacityFrames_ - static_cast<std::size_t>(write - read));
  for (std::size_t frame = 0; frame < count; ++frame) {
    const auto destination = ((write + frame) & mask_) * channels_;
    std::copy_n(interleaved + frame * channels_, channels_, samples_.get() + destination);
    const auto slot = static_cast<std::size_t>((write + frame) & mask_);
    epochs_[slot] = epoch;
    positions_[slot] = startFrame + static_cast<std::int64_t>(frame);
  }
  writePosition_.store(write + count, std::memory_order_release);
  return count;
}

std::size_t AudioRingBuffer::read(float* const interleaved,
                                  const std::size_t frames) noexcept {
  const auto read = readPosition_.load(std::memory_order_relaxed);
  const auto write = writePosition_.load(std::memory_order_acquire);
  const auto count = std::min(frames, static_cast<std::size_t>(write - read));
  for (std::size_t frame = 0; frame < count; ++frame) {
    const auto source = ((read + frame) & mask_) * channels_;
    std::copy_n(samples_.get() + source, channels_, interleaved + frame * channels_);
  }
  readPosition_.store(read + count, std::memory_order_release);
  return count;
}

std::size_t AudioRingBuffer::readTagged(float* const interleaved,
                                        const std::size_t frames,
                                        const std::uint64_t epoch,
                                        const std::int64_t startFrame) noexcept {
  auto read = readPosition_.load(std::memory_order_relaxed);
  const auto write = writePosition_.load(std::memory_order_acquire);
  std::size_t copied = 0;
  while (read < write && copied < frames) {
    const auto slot = static_cast<std::size_t>(read & mask_);
    const auto slotEpoch = epochs_[slot];
    const auto expectedPosition = startFrame + static_cast<std::int64_t>(copied);
    if (slotEpoch < epoch || (slotEpoch == epoch && positions_[slot] < expectedPosition)) {
      ++read;
      continue;
    }
    if (slotEpoch != epoch || positions_[slot] != expectedPosition) break;
    const auto source = slot * channels_;
    std::copy_n(samples_.get() + source, channels_,
                interleaved + copied * channels_);
    ++read;
    ++copied;
  }
  readPosition_.store(read, std::memory_order_release);
  return copied;
}

void AudioRingBuffer::clear() noexcept {
  const auto write = writePosition_.load(std::memory_order_acquire);
  readPosition_.store(write, std::memory_order_release);
}

} // namespace openutau::vst
