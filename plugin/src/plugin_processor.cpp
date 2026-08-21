#include "openutau_vst/plugin_processor.hpp"
#include "openutau_vst/plugin_editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

void moduleAnchor() {}

std::uint64_t nextPluginInstanceId() {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

juce::File currentPluginModule() {
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&moduleAnchor), &module) == 0) return {};
  std::array<char, 32768> path{};
  const auto length = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
  return length > 0 ? juce::File(juce::String::fromUTF8(path.data(), static_cast<int>(length)))
                    : juce::File{};
#else
  Dl_info info{};
  return dladdr(reinterpret_cast<void*>(&moduleAnchor), &info) != 0 && info.dli_fname != nullptr
      ? juce::File(info.dli_fname) : juce::File{};
#endif
}

} // namespace

namespace openutau::vst {

PluginProcessor::PluginProcessor()
    : AudioProcessor(BusesProperties().withOutput(
          "Output", juce::AudioChannelSet::stereo(), true)),
      instanceId_(nextPluginInstanceId()) {}

PluginProcessor::~PluginProcessor() = default;

void PluginProcessor::prepareToPlay(
    const double sampleRate, const int maximumExpectedSamplesPerBlock) {
  sampleRate_ = sampleRate;
  editorTone_.setSampleRate(sampleRate);
  editorPreviewBlockFrames_.store(
      static_cast<std::size_t>(std::max(1, maximumExpectedSamplesPerBlock)),
      std::memory_order_relaxed);
  fallbackSamplePosition_ = 0;
  lastHostTempo_ = std::numeric_limits<double>::quiet_NaN();
  lastHostNumerator_ = 0;
  lastHostDenominator_ = 0;
  // Do not delay the host timeline to cover synthesis time. The bridge renders
  // ahead while transport is stopped, so sample zero is already cached when
  // playback starts. A reported pre-roll proved unreliable in FL Studio: it
  // delayed this generator without compensating the rest of the arrangement.
  renderLatencySamples_ = 0;
  setLatencySamples(0);
  auto engineHost = juce::SystemStats::getEnvironmentVariable(
      "OPENUTAU_VST_ENGINE_HOST", {});
  auto editorHost = juce::SystemStats::getEnvironmentVariable(
      "OPENUTAU_VST_EDITOR", {});
  auto runtime = juce::SystemStats::getEnvironmentVariable(
      "OPENUTAU_VST_DOTNET", {});
  if (engineHost.isEmpty()) {
    const auto module = currentPluginModule();
    const auto resources = module.getParentDirectory().getParentDirectory()
        .getChildFile("Resources");
#if defined(_WIN32)
    const auto nativeEngine = resources.getChildFile("Engine").getChildFile(
        "OpenUtau.Vst.Engine.Host.exe");
    const auto managedEngine = resources.getChildFile("Engine").getChildFile(
        "OpenUtau.Vst.Engine.Host.dll");
    const auto packagedRuntime = resources.getChildFile("EmbeddedEditor")
        .getChildFile("dotnet").getChildFile("dotnet.exe");
    if (nativeEngine.existsAsFile()) {
      engineHost = nativeEngine.getFullPathName();
    } else if (managedEngine.existsAsFile() && packagedRuntime.existsAsFile()) {
      engineHost = managedEngine.getFullPathName();
      runtime = packagedRuntime.getFullPathName();
    }
    const auto companion = resources.getChildFile("Editor").getChildFile("OpenUtau.exe");
    if (companion.existsAsFile()) editorHost = companion.getFullPathName();
#else
    const auto nativeEngine = resources.getChildFile("Engine").getChildFile(
        "OpenUtau.Vst.Engine.Host");
    const auto managedEngine = resources.getChildFile("Engine").getChildFile(
        "OpenUtau.Vst.Engine.Host.dll");
    const auto packagedRuntime = resources.getChildFile("EmbeddedEditor")
        .getChildFile("dotnet").getChildFile("dotnet");
    if (nativeEngine.existsAsFile()) {
      engineHost = nativeEngine.getFullPathName();
    } else if (managedEngine.existsAsFile() && packagedRuntime.existsAsFile()) {
      engineHost = managedEngine.getFullPathName();
      runtime = packagedRuntime.getFullPathName();
    }
    const auto companion = resources.getChildFile("Editor").getChildFile("OpenUtau");
    if (companion.existsAsFile()) editorHost = companion.getFullPathName();
#endif
  } else if (runtime.isEmpty() && engineHost.endsWithIgnoreCase(".dll")) {
    runtime = "dotnet";
  }
  if (juce::File(engineHost).existsAsFile()) {
    engineBridge_.start(runtime, engineHost, editorHost);
  }
}

void PluginProcessor::releaseResources() {
  // Hosts such as FL Studio call releaseResources() immediately before
  // re-preparing a processor for offline export. The bridge owns an external
  // renderer whose startup and singer initialization are intentionally
  // asynchronous, so tearing it down here makes the first (and often entire)
  // bounce silent. EngineBridge's destructor still performs the real process
  // shutdown when the plugin instance itself is destroyed.
}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  return layouts.getMainInputChannelSet().isDisabled()
      && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
  juce::ScopedNoDenormals noDenormals;
  const auto frames = static_cast<std::size_t>(buffer.getNumSamples());
  auto observedBlock = editorPreviewBlockFrames_.load(std::memory_order_relaxed);
  while (frames > observedBlock
         && !editorPreviewBlockFrames_.compare_exchange_weak(
             observedBlock, frames, std::memory_order_relaxed)) {
  }
  buffer.clear();
  std::int64_t hostStartSample = fallbackSamplePosition_;
  bool playing = false;
  double hostPpq = std::numeric_limits<double>::quiet_NaN();
  double hostBpm = std::numeric_limits<double>::quiet_NaN();
  int hostNumerator = 0;
  int hostDenominator = 0;
  if (auto* hostPlayHead = getPlayHead()) {
    if (const auto position = hostPlayHead->getPosition()) {
      if (const auto time = position->getTimeInSamples()) hostStartSample = *time;
      playing = position->getIsPlaying();
      const auto bpm = position->getBpm();
      const auto ppq = position->getPpqPosition();
      const auto signature = position->getTimeSignature();
      const auto barPosition = position->getBarCount();
      if (bpm) hostBpm = *bpm;
      if (ppq) hostPpq = *ppq;
      if (signature) {
        hostNumerator = signature->numerator;
        hostDenominator = signature->denominator;
      }
      if (bpm && ppq && signature
          && (std::abs(*bpm - lastHostTempo_) > 1e-9
              || signature->numerator != lastHostNumerator_
              || signature->denominator != lastHostDenominator_)) {
        engineBridge_.updateHostTiming(
            std::max(0.0, *ppq), *bpm,
            barPosition ? *barPosition : -1,
            signature->numerator, signature->denominator);
        lastHostTempo_ = *bpm;
        lastHostNumerator_ = signature->numerator;
        lastHostDenominator_ = signature->denominator;
      }
    }
  }
  hostTransportSample_.store(hostStartSample, std::memory_order_relaxed);
  hostTransportSampleRate_.store(sampleRate_, std::memory_order_relaxed);
  hostTransportQuarterNote_.store(hostPpq, std::memory_order_relaxed);
  hostTransportBpm_.store(hostBpm, std::memory_order_relaxed);
  hostTransportNumerator_.store(hostNumerator, std::memory_order_relaxed);
  hostTransportDenominator_.store(hostDenominator, std::memory_order_relaxed);
  hostTransportPlaying_.store(playing, std::memory_order_release);
  fallbackSamplePosition_ = hostStartSample + static_cast<std::int64_t>(frames);

  // FL Studio's offline renderer can call the plugin with the non-realtime
  // flag set while its playhead does not report the normal live `playing`
  // state. The non-realtime processing mode itself is the authoritative
  // signal that this callback must synchronously produce export audio.
  if (isNonRealtime()) {
    std::vector<float> rendered;
    if (engineBridge_.renderOffline(std::max<std::int64_t>(0, hostStartSample),
                                    static_cast<int>(frames), sampleRate_, rendered)) {
      for (std::size_t frame = 0; frame < frames; ++frame) {
        buffer.setSample(0, static_cast<int>(frame), rendered[frame * 2]);
        buffer.setSample(1, static_cast<int>(frame), rendered[frame * 2 + 1]);
      }
    }
    return;
  }

  // The bridge stores interleaved audio. Stack scratch keeps the callback free
  // of allocation, locks, IPC, filesystem access, and process operations.
  const auto projectStartSample = hostStartSample;
  engineBridge_.beginBlock(projectStartSample, frames, sampleRate_, playing);
  constexpr std::size_t chunkFrames = 256;
  std::array<float, chunkFrames * 2> scratch{};
  std::size_t consumed = 0;
  while (consumed < frames) {
    const auto wanted = std::min(chunkFrames, frames - consumed);
    const auto read = engineBridge_.read(
        scratch.data(), wanted,
        projectStartSample + static_cast<std::int64_t>(consumed));
    for (std::size_t frame = 0; frame < read; ++frame) {
      buffer.setSample(0, static_cast<int>(consumed + frame), scratch[frame * 2]);
      buffer.setSample(1, static_cast<int>(consumed + frame), scratch[frame * 2 + 1]);
    }
    // Advance across the complete host block even when a render-ahead slice
    // is not ready. The buffer was cleared above, so missing frames remain
    // silent. Stopping at the first miss would leave EngineBridge's expected
    // cursor in the middle of a larger DAW callback; the next normal callback
    // would then be misclassified as a seek and invalidate the render cache on
    // every block (notably with FL Studio's 2048-frame live buffers).
    consumed += wanted;
  }

  const auto toneRevision = editorToneRevision_.load(std::memory_order_acquire);
  if (toneRevision != editorToneObservedRevision_) {
    editorToneObservedRevision_ = toneRevision;
    const auto previousToneState = editorToneObservedState_;
    editorToneObservedState_ = editorToneState_.load(std::memory_order_relaxed);
    if (editorToneObservedState_ > 0) {
      editorTone_.start(editorToneFrequency_.load(std::memory_order_relaxed));
    } else if (editorToneObservedState_ == 0) {
      // A quick click can begin and end between editor timer ticks. If no held
      // note was observed, keep that click audible as a short one-shot.
      if (previousToneState == 1) {
        editorTone_.release();
      } else {
        editorTone_.startOneShot(
            editorToneFrequency_.load(std::memory_order_relaxed));
      }
    } else if (editorToneObservedState_ < 0) {
      editorTone_.reset();
    }
  }

  // Piano-key audition is generated in the audio callback. Passing this
  // simple tone through the editor timer caused gaps with large host blocks.
  if (!playing && editorToneObservedState_ >= 0) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const auto sample = editorTone_.nextSample();
      buffer.addSample(0, static_cast<int>(frame), sample);
      buffer.addSample(1, static_cast<int>(frame), sample);
    }
    editorPreviewLastLeft_ = 0.0f;
    editorPreviewLastRight_ = 0.0f;
    editorPreviewFadeRemaining_ = 0;
  // OpenUtau playback still comes through the editor bridge and normal VST
  // outputs, without opening a second hardware device.
  } else if (!playing && editorPreviewActive_.load(std::memory_order_acquire)) {
    const auto epoch = editorPreviewEpoch_.load(std::memory_order_acquire);
    if (editorPreviewObservedEpoch_ != epoch) {
      editorPreviewObservedEpoch_ = epoch;
      editorPreviewReadFrame_ = 0;
      editorPreviewFadeRemaining_ = 128;
      editorPreviewFadeStartLeft_ = editorPreviewLastLeft_;
      editorPreviewFadeStartRight_ = editorPreviewLastRight_;
    }
    std::size_t previewOffset = 0;
    while (previewOffset < frames) {
      const auto wanted = std::min(chunkFrames, frames - previewOffset);
      const auto read = editorPreviewRing_.readTagged(
          scratch.data(), wanted, epoch, editorPreviewReadFrame_);
      for (std::size_t frame = 0; frame < wanted; ++frame) {
        if (frame >= read && editorPreviewFadeRemaining_ == 0) break;
        const auto nextLeft = frame < read ? scratch[frame * 2] : 0.0f;
        const auto nextRight = frame < read ? scratch[frame * 2 + 1] : 0.0f;
        auto left = nextLeft;
        auto right = nextRight;
        if (editorPreviewFadeRemaining_ > 0) {
          const auto mix = static_cast<float>(
              128 - editorPreviewFadeRemaining_ + 1) / 128.0f;
          left = editorPreviewFadeStartLeft_ * (1.0f - mix) + nextLeft * mix;
          right = editorPreviewFadeStartRight_ * (1.0f - mix) + nextRight * mix;
          --editorPreviewFadeRemaining_;
        }
        buffer.addSample(0, static_cast<int>(previewOffset + frame),
                         left);
        buffer.addSample(1, static_cast<int>(previewOffset + frame),
                         right);
        editorPreviewLastLeft_ = left;
        editorPreviewLastRight_ = right;
      }
      editorPreviewReadFrame_ += static_cast<std::int64_t>(read);
      previewOffset += wanted;
      if (read < wanted) break;
    }
  } else {
    editorPreviewLastLeft_ = 0.0f;
    editorPreviewLastRight_ = 0.0f;
    editorPreviewFadeRemaining_ = 0;
  }
}

std::size_t PluginProcessor::editorPreviewWritableFrames() const noexcept {
  // Hold enough audio to cover a stalled editor timer and FL Studio's larger
  // callback blocks. Note changes explicitly discard this queue.
  const auto sampleRate = hostTransportSampleRate_.load(std::memory_order_relaxed);
  const auto timedFrames = static_cast<std::size_t>(std::clamp(
      sampleRate * 0.250, 512.0,
      static_cast<double>(editorPreviewCapacityFrames)));
  const auto blockFrames = editorPreviewBlockFrames_.load(
      std::memory_order_relaxed);
  const auto guardedBlockFrames = blockFrames > editorPreviewCapacityFrames / 2
      ? editorPreviewCapacityFrames
      : blockFrames * 2;
  const auto latencyFrames = std::min(
      editorPreviewCapacityFrames,
      std::max(timedFrames, guardedBlockFrames));
  const auto readable = editorPreviewRing_.readableFrames();
  if (readable >= latencyFrames) return 0;
  return std::min(
      editorPreviewRing_.writableFrames(), latencyFrames - readable);
}

void PluginProcessor::updateEditorPreview(
    const float* const interleaved, const std::size_t frames,
    const bool active) noexcept {
  if (active != editorPreviewWriterActive_) {
    editorPreviewWriterActive_ = active;
    ++editorPreviewWriterEpoch_;
    editorPreviewWriteFrame_ = 0;
    editorPreviewEpoch_.store(editorPreviewWriterEpoch_, std::memory_order_release);
    editorPreviewActive_.store(active, std::memory_order_release);
  }
  if (!active || interleaved == nullptr || frames == 0) return;
  const auto written = editorPreviewRing_.writeTagged(
      interleaved, frames, editorPreviewWriterEpoch_, editorPreviewWriteFrame_);
  editorPreviewWriteFrame_ += static_cast<std::int64_t>(written);
}

void PluginProcessor::updateEditorTone(
    const double frequency, const int state,
    const std::uint64_t revision) noexcept {
  editorToneFrequency_.store(
      std::isfinite(frequency) && frequency > 0.0 ? frequency : 440.0,
      std::memory_order_relaxed);
  editorToneState_.store(std::clamp(state, -1, 1), std::memory_order_relaxed);
  editorToneRevision_.store(revision, std::memory_order_release);
}

PluginProcessor::HostTransportSnapshot PluginProcessor::hostTransportSnapshot() const noexcept {
  HostTransportSnapshot snapshot;
  // `playing` is published last by the audio thread. Acquire it first and
  // again after the remaining relaxed fields to avoid handing the UI a mixed
  // play/stop transition.
  for (;;) {
    const auto playing = hostTransportPlaying_.load(std::memory_order_acquire);
    snapshot.projectSample = hostTransportSample_.load(std::memory_order_relaxed);
    snapshot.sampleRate = hostTransportSampleRate_.load(std::memory_order_relaxed);
    snapshot.quarterNotePosition =
        hostTransportQuarterNote_.load(std::memory_order_relaxed);
    snapshot.bpm = hostTransportBpm_.load(std::memory_order_relaxed);
    snapshot.timeSignatureNumerator =
        hostTransportNumerator_.load(std::memory_order_relaxed);
    snapshot.timeSignatureDenominator =
        hostTransportDenominator_.load(std::memory_order_relaxed);
    if (playing == hostTransportPlaying_.load(std::memory_order_acquire)) {
      snapshot.playing = playing;
      return snapshot;
    }
  }
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destination) {
  bool editorSynchronised = false;
  if (auto* manager = juce::MessageManager::getInstanceWithoutCreating();
      manager != nullptr && manager->isThisTheMessageThread()) {
    if (auto* editor = dynamic_cast<PluginEditor*>(getActiveEditor())) {
      // DAWs commonly request state on the message thread when the user saves.
      // Pull once here so an edit made in the final 33 ms before Save cannot be
      // missed by the editor's normal 30 Hz synchronisation timer.
      editorSynchronised = editor->synchroniseProjectState(false);
    }
  }
  const auto engineState = engineBridge_.cachedProjectState();
  const juce::ScopedLock lock(stateMutex_);
  if (!editorSynchronised && engineState.getSize() > 0) projectState_ = engineState;
  juce::MemoryOutputStream stream(destination, false);
  stream.writeIntBigEndian(static_cast<int>(stateMagic));
  stream.writeIntBigEndian(static_cast<int>(stateVersion));
  stream.writeInt64BigEndian(static_cast<juce::int64>(projectState_.getSize()));
  stream.write(projectState_.getData(), projectState_.getSize());
}

void PluginProcessor::setStateInformation(const void* data, const int size) {
  if (data == nullptr || size < 16) return;
  juce::MemoryInputStream stream(data, static_cast<std::size_t>(size), false);
  if (static_cast<std::uint32_t>(stream.readIntBigEndian()) != stateMagic) return;
  if (static_cast<std::uint32_t>(stream.readIntBigEndian()) != stateVersion) return;
  const auto payloadSize = stream.readInt64BigEndian();
  if (payloadSize < 0 || payloadSize > stream.getNumBytesRemaining()) return;
  juce::MemoryBlock incoming;
  incoming.setSize(static_cast<std::size_t>(payloadSize));
  if (stream.read(incoming.getData(), static_cast<int>(payloadSize)) != payloadSize) return;
  juce::MemoryBlock bridgeState;
  {
    const juce::ScopedLock lock(stateMutex_);
    projectState_.swapWith(incoming);
    bridgeState = projectState_;
  }
  engineBridge_.setProjectState(bridgeState.getData(), bridgeState.getSize());
}

bool PluginProcessor::loadProjectFile(const juce::File& file) {
  juce::MemoryBlock incoming;
  if (!file.existsAsFile() || !file.loadFileAsData(incoming) || incoming.isEmpty()) return false;
  juce::MemoryBlock bridgeState;
  {
    const juce::ScopedLock lock(stateMutex_);
    projectState_.swapWith(incoming);
    bridgeState = projectState_;
  }
  engineBridge_.setProjectState(bridgeState.getData(), bridgeState.getSize());
  updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withNonParameterStateChanged(true));
  return true;
}

juce::MemoryBlock PluginProcessor::projectStateSnapshot() const {
  const auto engineState = engineBridge_.cachedProjectState();
  if (!engineState.isEmpty()) return engineState;
  const juce::ScopedLock lock(stateMutex_);
  return projectState_;
}

void PluginProcessor::setEmbeddedProjectState(
    const void* data, const std::size_t size, const bool notifyHost) {
  if (data == nullptr || size == 0) return;
  juce::MemoryBlock state(data, size);
  {
    const juce::ScopedLock lock(stateMutex_);
    projectState_ = state;
  }
  engineBridge_.setProjectState(state.getData(), state.getSize());
  if (notifyHost) {
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
                          .withNonParameterStateChanged(true));
  }
}

juce::AudioProcessorEditor* PluginProcessor::createEditor() { return new PluginEditor(*this); }

} // namespace openutau::vst

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new openutau::vst::PluginProcessor();
}
