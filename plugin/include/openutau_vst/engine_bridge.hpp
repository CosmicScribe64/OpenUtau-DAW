#pragma once

#include "openutau_vst/audio_ring_buffer.hpp"
#include "openutau_vst/engine_client.hpp"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace openutau::vst {

class EngineBridge final : private juce::Thread {
public:
  explicit EngineBridge(int realtimeRenderTimeoutMs = 30000);
  ~EngineBridge() override;

  void start(const juce::String& runtime, const juce::String& engineHost,
             const juce::String& editorHost = {});
  void stop();
  void setProjectState(const void* data, std::size_t size);
  juce::MemoryBlock cachedProjectState() const;

  // Called once per DAW callback before any sliced reads. This keeps host
  // callback boundaries distinct from the processor's 256-frame scratch
  // slices and lets the renderer prewarm while transport is stopped.
  void beginBlock(std::int64_t hostStartSample, std::size_t frames,
                  double sampleRate, bool playing) noexcept;
  // Called only by the DAW audio thread after beginBlock. Returns rendered
  // frame count; the caller leaves the remainder silent on a cache miss.
  std::size_t read(float* interleaved, std::size_t frames,
                   std::int64_t hostStartSample) noexcept;
  bool renderOffline(std::int64_t startSample, int frameCount, double sampleRate,
                     std::vector<float>& interleaved, int timeoutMs = 300000);
  void requestEditor() noexcept;
  void requestRestart() noexcept;
  void updateHostTiming(double quarterNotePosition, double tempo,
                        std::int64_t barPosition, int numerator,
                        int denominator) noexcept;
  [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
  [[nodiscard]] juce::String lastError() const;
  [[nodiscard]] std::uint64_t underruns() const noexcept { return underruns_.load(); }
  [[nodiscard]] std::uint64_t engineStarts() const noexcept { return engineStarts_.load(); }

private:
  void run() override;
  void setError(const juce::String& error);

  static constexpr std::size_t ringCapacityFrames = 1u << 18;
  static constexpr int renderBlockFrames = 8192;
  AudioRingBuffer ring_{ringCapacityFrames, 2};
  EngineClient client_;

  juce::String runtime_;
  juce::String engineHost_;
  juce::String editorHost_;
  int realtimeRenderTimeoutMs_{30000};
  mutable juce::CriticalSection stateMutex_;
  juce::MemoryBlock projectState_;
  juce::String error_;
  std::atomic<std::uint64_t> stateRevision_{0};

  std::atomic<bool> connected_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> transportReady_{false};
  std::atomic<std::uint64_t> transportEpoch_{1};
  std::atomic<std::int64_t> transportStart_{0};
  std::atomic<std::int64_t> nextReadSample_{0};
  std::atomic<double> sampleRate_{44100.0};
  std::atomic<std::uint64_t> underruns_{0};
  std::atomic<std::uint64_t> engineStarts_{0};
  std::atomic<bool> editorRequested_{false};
  std::atomic<bool> editorOpen_{false};
  std::atomic<bool> restartRequested_{false};
  std::atomic<double> hostQuarterNote_{0.0};
  std::atomic<double> hostTempo_{120.0};
  std::atomic<std::int64_t> hostBarPosition_{-1};
  std::atomic<int> hostNumerator_{4};
  std::atomic<int> hostDenominator_{4};
  std::atomic<std::uint64_t> hostTimingRevision_{0};
  // Audio-thread-owned transport history.
  std::int64_t expectedNextSample_{-1};
  std::int64_t stoppedSample_{-1};
  double previousSampleRate_{0.0};
  bool previouslyPlaying_{false};
  bool transportInitialized_{false};
  std::uint64_t audioObservedEpoch_{0};
  static constexpr std::size_t declickFrames = 128;
  std::size_t fadeInRemaining_{declickFrames};

  mutable juce::CriticalSection offlineMutex_;
  juce::WaitableEvent offlineComplete_;
  bool offlinePending_{false};
  bool offlineSucceeded_{false};
  std::int64_t offlineStart_{0};
  int offlineFrames_{0};
  double offlineSampleRate_{44100.0};
  std::vector<float> offlineResult_;
  juce::String offlineError_;
  static constexpr int offlineChunkFrames = 1 << 18;
  std::int64_t offlineCacheStart_{-1};
  double offlineCacheSampleRate_{0.0};
  std::uint64_t offlineCacheStateRevision_{0};
  std::uint64_t offlineCacheTimingRevision_{0};
  std::vector<float> offlineCache_;
};

} // namespace openutau::vst
