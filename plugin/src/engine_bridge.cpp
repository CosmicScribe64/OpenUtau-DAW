#include "openutau_vst/engine_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace openutau::vst {

EngineBridge::EngineBridge(const int realtimeRenderTimeoutMs)
    : Thread("OpenUtau render bridge"),
      realtimeRenderTimeoutMs_(std::clamp(realtimeRenderTimeoutMs, 10, 300000)) {}
EngineBridge::~EngineBridge() { stop(); }

void EngineBridge::start(const juce::String& runtime, const juce::String& engineHost,
                         const juce::String& editorHost) {
  if (isThreadRunning()) return;
  runtime_ = runtime;
  engineHost_ = engineHost;
  editorHost_ = editorHost;
  startThread(juce::Thread::Priority::normal);
}

void EngineBridge::stop() {
  signalThreadShouldExit();
  notify();
  stopThread(5000);
  client_.disconnect();
  connected_.store(false, std::memory_order_release);
  offlineComplete_.signal();
}

bool EngineBridge::renderOffline(const std::int64_t startSample, const int frameCount,
                                 const double sampleRate, std::vector<float>& interleaved,
                                 const int timeoutMs) {
  if (!connected() || startSample < 0 || frameCount < 0 || frameCount > 1 << 20
      || !std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 384000.0) {
    return false;
  }
  const auto stateRevision = stateRevision_.load(std::memory_order_acquire);
  const auto timingRevision = hostTimingRevision_.load(std::memory_order_acquire);
  {
    const juce::ScopedLock lock(offlineMutex_);
    const auto cacheOffset = startSample - offlineCacheStart_;
    const auto cachedFrames = static_cast<std::int64_t>(offlineCache_.size() / 2);
    if (offlineCacheStart_ >= 0 && cacheOffset >= 0
        && cacheOffset + frameCount <= cachedFrames
        && std::abs(offlineCacheSampleRate_ - sampleRate) < 0.001
        && offlineCacheStateRevision_ == stateRevision
        && offlineCacheTimingRevision_ == timingRevision) {
      const auto begin = static_cast<std::size_t>(cacheOffset) * 2;
      const auto end = begin + static_cast<std::size_t>(frameCount) * 2;
      interleaved.assign(offlineCache_.begin() + static_cast<std::ptrdiff_t>(begin),
                         offlineCache_.begin() + static_cast<std::ptrdiff_t>(end));
      return true;
    }
    if (offlinePending_) return false;
    offlineStart_ = startSample;
    offlineFrames_ = std::max(frameCount, offlineChunkFrames);
    offlineSampleRate_ = sampleRate;
    offlineSucceeded_ = false;
    offlineResult_.clear();
    offlineError_.clear();
    offlinePending_ = true;
    offlineComplete_.reset();
  }
  notify();
  if (!offlineComplete_.wait(timeoutMs)) {
    setError("Timed out waiting for offline OpenUtau render.");
    return false;
  }
  const juce::ScopedLock lock(offlineMutex_);
  if (!offlineSucceeded_) {
    if (offlineError_.isNotEmpty()) setError(offlineError_);
    return false;
  }
  offlineCacheStart_ = startSample;
  offlineCacheSampleRate_ = sampleRate;
  offlineCacheStateRevision_ = stateRevision;
  offlineCacheTimingRevision_ = timingRevision;
  offlineCache_ = std::move(offlineResult_);
  interleaved.assign(offlineCache_.begin(),
                     offlineCache_.begin() + static_cast<std::ptrdiff_t>(frameCount) * 2);
  return true;
}

void EngineBridge::requestEditor() noexcept {
  editorRequested_.store(true, std::memory_order_release);
  notify();
}

void EngineBridge::requestRestart() noexcept {
  restartRequested_.store(true, std::memory_order_release);
  notify();
}

void EngineBridge::updateHostTiming(const double quarterNotePosition, const double tempo,
                                    const std::int64_t barPosition, const int numerator,
                                    const int denominator) noexcept {
  hostQuarterNote_.store(quarterNotePosition, std::memory_order_relaxed);
  hostTempo_.store(tempo, std::memory_order_relaxed);
  hostBarPosition_.store(barPosition, std::memory_order_relaxed);
  hostNumerator_.store(numerator, std::memory_order_relaxed);
  hostDenominator_.store(denominator, std::memory_order_relaxed);
  hostTimingRevision_.fetch_add(1, std::memory_order_release);
  notify();
}

void EngineBridge::setProjectState(const void* data, const std::size_t size) {
  const juce::ScopedLock lock(stateMutex_);
  projectState_.replaceAll(data, size);
  stateRevision_.fetch_add(1, std::memory_order_release);
}

juce::MemoryBlock EngineBridge::cachedProjectState() const {
  const juce::ScopedLock lock(stateMutex_);
  return projectState_;
}

void EngineBridge::beginBlock(const std::int64_t hostStartSample,
                              const std::size_t frames,
                              const double sampleRate,
                              const bool playing) noexcept {
  const auto currentEpoch = transportEpoch_.load(std::memory_order_acquire);
  if (audioObservedEpoch_ != currentEpoch) {
    ring_.clear();
    audioObservedEpoch_ = currentEpoch;
    fadeInRemaining_ = declickFrames;
  }
  const auto expectedStart = previouslyPlaying_ ? expectedNextSample_ : stoppedSample_;
  const auto discontinuity = !transportInitialized_
      || expectedStart != hostStartSample
      || std::abs(previousSampleRate_ - sampleRate) > 0.001;
  if (discontinuity) {
    transportStart_.store(std::max<std::int64_t>(0, hostStartSample), std::memory_order_release);
    sampleRate_.store(sampleRate, std::memory_order_release);
    audioObservedEpoch_ = transportEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    ring_.clear();
    fadeInRemaining_ = declickFrames;
  }
  playing_.store(playing, std::memory_order_release);
  transportReady_.store(true, std::memory_order_release);
  transportInitialized_ = true;
  previouslyPlaying_ = playing;
  previousSampleRate_ = sampleRate;
  expectedNextSample_ = playing
      ? hostStartSample + static_cast<std::int64_t>(frames)
      : hostStartSample;
  stoppedSample_ = hostStartSample;
  nextReadSample_.store(std::max<std::int64_t>(0, expectedNextSample_),
                        std::memory_order_release);
  notify();
}

std::size_t EngineBridge::read(float* const interleaved, const std::size_t frames,
                               const std::int64_t hostStartSample) noexcept {
  const auto playing = playing_.load(std::memory_order_acquire);
  if (!playing || hostStartSample < 0) return 0;

  const auto epoch = transportEpoch_.load(std::memory_order_acquire);
  const auto count = ring_.readTagged(interleaved, frames, epoch, hostStartSample);
  const auto faded = std::min(count, fadeInRemaining_);
  const auto fadeOffset = declickFrames - fadeInRemaining_;
  for (std::size_t frame = 0; frame < faded; ++frame) {
    const auto gain = static_cast<float>(fadeOffset + frame) /
        static_cast<float>(declickFrames);
    interleaved[frame * 2] *= gain;
    interleaved[frame * 2 + 1] *= gain;
  }
  fadeInRemaining_ -= faded;
  if (count != frames) {
    underruns_.fetch_add(1, std::memory_order_relaxed);
    fadeInRemaining_ = declickFrames;
  }
  return count;
}

juce::String EngineBridge::lastError() const {
  const juce::ScopedLock lock(stateMutex_);
  return error_;
}

void EngineBridge::setError(const juce::String& error) {
  const juce::ScopedLock lock(stateMutex_);
  error_ = error;
}

void EngineBridge::run() {
  while (!threadShouldExit()) {
    juce::String error;
    if (!client_.launch(runtime_, engineHost_, error, editorHost_)) {
      setError(error);
      wait(500);
      continue;
    }
    connected_.store(true, std::memory_order_release);
    engineStarts_.fetch_add(1, std::memory_order_release);
    setError({});
    // Invalidate every frame produced by the previous process. The audio
    // callback will establish the exact new cursor on its next read.
    transportEpoch_.fetch_add(1, std::memory_order_acq_rel);

    // The sentinel forces cached state to be applied even when its revision is
    // zero (for example, state captured from a companion editor).
    std::uint64_t appliedStateRevision = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t appliedTimingRevision = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t renderEpoch = 0;
    std::int64_t renderCursor = 0;
    std::vector<float> rendered;
    auto lastEditorSync = juce::Time::getMillisecondCounter();
    while (!threadShouldExit()) {
    if (restartRequested_.exchange(false, std::memory_order_acq_rel)) break;
    if (editorRequested_.exchange(false, std::memory_order_acq_rel)) {
      juce::String projectPath;
      if (!client_.openEditor(projectPath, error)) {
        setError(error);
      } else {
        editorOpen_.store(true, std::memory_order_release);
      }
    }
    const auto now = juce::Time::getMillisecondCounter();
    if (editorOpen_.load(std::memory_order_acquire)
        && now - lastEditorSync >= 1000u) {
      juce::MemoryBlock state;
      if (client_.getState(state, error)) {
        const juce::ScopedLock lock(stateMutex_);
        projectState_ = std::move(state);
      } else {
        setError(error);
      }
      lastEditorSync = now;
    }
    bool handleOffline = false;
    std::int64_t offlineStart = 0;
    int offlineFrames = 0;
    double offlineRate = 44100.0;
    {
      const juce::ScopedLock lock(offlineMutex_);
      if (offlinePending_) {
        handleOffline = true;
        offlineStart = offlineStart_;
        offlineFrames = offlineFrames_;
        offlineRate = offlineSampleRate_;
        offlinePending_ = false;
      }
    }
    if (handleOffline) {
      std::vector<float> result;
      const auto succeeded = client_.render(0, offlineStart, offlineFrames,
                                            offlineRate, true, result, error);
      {
        const juce::ScopedLock lock(offlineMutex_);
        offlineSucceeded_ = succeeded;
        offlineResult_ = std::move(result);
        offlineError_ = succeeded ? juce::String{} : error;
      }
      offlineComplete_.signal();
      if (!succeeded && !client_.connected()) break;
      continue;
    }
    bool engineStateChanged = false;
    const auto revision = stateRevision_.load(std::memory_order_acquire);
    if (revision != appliedStateRevision) {
      juce::MemoryBlock state;
      {
        const juce::ScopedLock lock(stateMutex_);
        state = projectState_;
      }
      if (state.getSize() > 0 && !client_.setState(state.getData(), state.getSize(), error)) {
        setError(error);
        break;
      }
      appliedStateRevision = revision;
      engineStateChanged = true;
    }
    const auto timingRevision = hostTimingRevision_.load(std::memory_order_acquire);
    if (timingRevision > 0 && timingRevision != appliedTimingRevision) {
      if (!client_.setHostTiming(
              hostQuarterNote_.load(std::memory_order_relaxed),
              hostTempo_.load(std::memory_order_relaxed),
              hostBarPosition_.load(std::memory_order_relaxed),
              hostNumerator_.load(std::memory_order_relaxed),
              hostDenominator_.load(std::memory_order_relaxed), error)) {
        setError(error);
        break;
      }
      appliedTimingRevision = timingRevision;
      engineStateChanged = true;
    }
    if (engineStateChanged) {
      juce::MemoryBlock state;
      if (!client_.getState(state, error)) {
        setError(error);
        break;
      }
      const juce::ScopedLock lock(stateMutex_);
      projectState_ = std::move(state);
      // Drop audio rendered from the previous project revision and resume at
      // the callback's current project cursor. This covers tempo edits and
      // companion-editor saves.
      transportStart_.store(nextReadSample_.load(std::memory_order_acquire),
                            std::memory_order_release);
      transportEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    if (!transportReady_.load(std::memory_order_acquire)) {
      wait(5);
      continue;
    }
    const auto epoch = transportEpoch_.load(std::memory_order_acquire);
    if (epoch != renderEpoch) {
      renderEpoch = epoch;
      renderCursor = transportStart_.load(std::memory_order_acquire);
    }
    if (ring_.writableFrames() < static_cast<std::size_t>(renderBlockFrames)) {
      wait(2);
      continue;
    }
    const auto rate = sampleRate_.load(std::memory_order_acquire);
    if (!client_.render(static_cast<std::int64_t>(epoch), renderCursor,
                        renderBlockFrames, rate, false, rendered, error,
                        realtimeRenderTimeoutMs_)) {
      setError(error);
      break;
    }
    if (transportEpoch_.load(std::memory_order_acquire) != epoch) continue;
    const auto written = ring_.writeTagged(rendered.data(), renderBlockFrames, epoch, renderCursor);
    renderCursor += static_cast<std::int64_t>(written);
    }
    connected_.store(false, std::memory_order_release);
    transportEpoch_.fetch_add(1, std::memory_order_acq_rel);
    {
      const juce::ScopedLock lock(offlineMutex_);
      offlinePending_ = false;
      offlineSucceeded_ = false;
      if (offlineError_.isEmpty()) offlineError_ = "Engine stopped during offline render.";
    }
    offlineComplete_.signal();
    client_.disconnect();
    if (!threadShouldExit()) wait(250);
  }
}

} // namespace openutau::vst
