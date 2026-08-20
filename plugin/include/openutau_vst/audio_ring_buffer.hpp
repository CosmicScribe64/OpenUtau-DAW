#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace openutau::vst {

// Single-producer/single-consumer interleaved float ring buffer. Capacity must
// be a power of two. The producer is the render worker and the consumer is the
// VST audio callback. Neither operation allocates or locks.
class AudioRingBuffer final {
public:
  AudioRingBuffer(std::size_t capacityFrames, std::size_t channels);

  AudioRingBuffer(const AudioRingBuffer&) = delete;
  AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

  [[nodiscard]] std::size_t capacityFrames() const noexcept;
  [[nodiscard]] std::size_t channels() const noexcept;
  [[nodiscard]] std::size_t readableFrames() const noexcept;
  [[nodiscard]] std::size_t writableFrames() const noexcept;

  std::size_t write(const float* interleaved, std::size_t frames) noexcept;
  std::size_t read(float* interleaved, std::size_t frames) noexcept;
  std::size_t writeTagged(const float* interleaved, std::size_t frames,
                          std::uint64_t epoch, std::int64_t startFrame) noexcept;
  std::size_t readTagged(float* interleaved, std::size_t frames,
                         std::uint64_t epoch, std::int64_t startFrame) noexcept;
  void clear() noexcept;

private:
  const std::size_t capacityFrames_;
  const std::size_t channels_;
  const std::size_t mask_;
  std::unique_ptr<float[]> samples_;
  std::unique_ptr<std::uint64_t[]> epochs_;
  std::unique_ptr<std::int64_t[]> positions_;
  alignas(64) std::atomic<std::uint64_t> writePosition_{0};
  alignas(64) std::atomic<std::uint64_t> readPosition_{0};
};

} // namespace openutau::vst
