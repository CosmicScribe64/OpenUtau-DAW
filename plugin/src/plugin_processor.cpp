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
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

PluginProcessor::~PluginProcessor() = default;

void PluginProcessor::prepareToPlay(const double sampleRate, const int) {
  sampleRate_ = sampleRate;
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

  // Editor-local audition is produced off-thread and is audible only while
  // the DAW transport is stopped. This keeps a click on OpenUtau's Play button
  // inside the normal VST output/mixer path without opening a second hardware
  // device or doubling host-controlled playback.
  if (!playing && editorPreviewActive_.load(std::memory_order_acquire)) {
    const auto epoch = editorPreviewEpoch_.load(std::memory_order_acquire);
    if (editorPreviewObservedEpoch_ != epoch) {
      editorPreviewObservedEpoch_ = epoch;
      editorPreviewReadFrame_ = 0;
      editorPreviewFadeRemaining_ = 128;
    }
    std::size_t previewOffset = 0;
    while (previewOffset < frames) {
      const auto wanted = std::min(chunkFrames, frames - previewOffset);
      const auto read = editorPreviewRing_.readTagged(
          scratch.data(), wanted, epoch, editorPreviewReadFrame_);
      for (std::size_t frame = 0; frame < read; ++frame) {
        auto gain = 1.0f;
        if (editorPreviewFadeRemaining_ > 0) {
          gain = static_cast<float>(128 - editorPreviewFadeRemaining_)
              / 128.0f;
          --editorPreviewFadeRemaining_;
        }
        buffer.addSample(0, static_cast<int>(previewOffset + frame),
                         scratch[frame * 2] * gain);
        buffer.addSample(1, static_cast<int>(previewOffset + frame),
                         scratch[frame * 2 + 1] * gain);
      }
      editorPreviewReadFrame_ += static_cast<std::int64_t>(read);
      previewOffset += wanted;
      if (read < wanted) break;
    }
  }
}

std::size_t PluginProcessor::editorPreviewWritableFrames() const noexcept {
  return editorPreviewRing_.writableFrames();
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
