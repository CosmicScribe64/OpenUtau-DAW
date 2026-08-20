#pragma once

#include "openutau_vst/engine_bridge.hpp"
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <limits>

namespace openutau::vst {

class PluginProcessor final : public juce::AudioProcessor {
public:
  struct HostTransportSnapshot final {
    std::int64_t projectSample{};
    double sampleRate{44100.0};
    double quarterNotePosition{std::numeric_limits<double>::quiet_NaN()};
    double bpm{std::numeric_limits<double>::quiet_NaN()};
    int timeSignatureNumerator{};
    int timeSignatureDenominator{};
    bool playing{};
  };

  PluginProcessor();
  ~PluginProcessor() override;

  void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
  void releaseResources() override;
  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  void processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) override {
    buffer.clear();
  }

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }
  const juce::String getName() const override { return JucePlugin_Name; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 0.0; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock&) override;
  void setStateInformation(const void*, int) override;

  [[nodiscard]] bool engineConnected() const noexcept { return engineBridge_.connected(); }
  [[nodiscard]] std::uint64_t underrunCount() const noexcept { return engineBridge_.underruns(); }
  [[nodiscard]] juce::String engineError() const { return engineBridge_.lastError(); }
  [[nodiscard]] HostTransportSnapshot hostTransportSnapshot() const noexcept;
  [[nodiscard]] std::size_t editorPreviewWritableFrames() const noexcept;
  void updateEditorPreview(const float* interleaved, std::size_t frames,
                           bool active) noexcept;
  bool loadProjectFile(const juce::File& file);
  [[nodiscard]] juce::MemoryBlock projectStateSnapshot() const;
  void setEmbeddedProjectState(const void* data, std::size_t size,
                               bool notifyHost = true);
  void openFullEditor() { engineBridge_.requestEditor(); }
  void restartEngine() { engineBridge_.requestRestart(); }

private:
  static constexpr std::uint32_t stateMagic = 0x4f555633; // OUV3
  static constexpr std::uint32_t stateVersion = 1;
  EngineBridge engineBridge_;
  juce::MemoryBlock projectState_;
  mutable juce::CriticalSection stateMutex_;
  double sampleRate_{44100.0};
  std::int64_t fallbackSamplePosition_{0};
  int renderLatencySamples_{0};
  double lastHostTempo_{std::numeric_limits<double>::quiet_NaN()};
  int lastHostNumerator_{0};
  int lastHostDenominator_{0};
  std::atomic<std::int64_t> hostTransportSample_{0};
  std::atomic<double> hostTransportSampleRate_{44100.0};
  std::atomic<double> hostTransportQuarterNote_{
      std::numeric_limits<double>::quiet_NaN()};
  std::atomic<double> hostTransportBpm_{
      std::numeric_limits<double>::quiet_NaN()};
  std::atomic<int> hostTransportNumerator_{0};
  std::atomic<int> hostTransportDenominator_{0};
  std::atomic<bool> hostTransportPlaying_{false};

  // The managed editor produces preview audio on a background thread. Its UI
  // copies into this native SPSC ring; only the DAW audio callback consumes it.
  static constexpr std::size_t editorPreviewCapacityFrames = 1u << 18;
  AudioRingBuffer editorPreviewRing_{editorPreviewCapacityFrames, 2};
  std::atomic<std::uint64_t> editorPreviewEpoch_{1};
  std::atomic<bool> editorPreviewActive_{false};
  std::uint64_t editorPreviewWriterEpoch_{1};
  std::int64_t editorPreviewWriteFrame_{0};
  bool editorPreviewWriterActive_{false};
  std::uint64_t editorPreviewObservedEpoch_{0};
  std::int64_t editorPreviewReadFrame_{0};
  std::size_t editorPreviewFadeRemaining_{128};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace openutau::vst
