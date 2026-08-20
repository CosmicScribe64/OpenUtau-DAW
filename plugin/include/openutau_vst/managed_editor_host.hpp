#pragma once

#include <juce_core/juce_core.h>

namespace openutau::vst {

// Loads the self-contained managed editor component through hostfxr. This is
// used only from the JUCE message thread; the audio callback never touches the
// managed runtime.
class ManagedEditorHost final {
public:
  ManagedEditorHost() = default;
  ~ManagedEditorHost();

  void* create(int width, int height);
  void destroy();
  bool setProjectState(const void* data, std::size_t size);
  bool pullProjectState(juce::MemoryBlock& destination);
  bool setHostTransport(std::int64_t projectSample, double sampleRate,
                        double quarterNotePosition, double bpm,
                        int timeSignatureNumerator,
                        int timeSignatureDenominator, bool playing);
  bool pullPreview(float* interleaved, std::size_t capacityFrames,
                   std::size_t& copiedFrames, bool& active);
  [[nodiscard]] juce::String lastError() const { return lastError_; }

private:
  void* nativeView_{};
  void* nativeViewOriginalClass_{};
  std::int64_t lastRevision_{-1};
  juce::String lastError_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ManagedEditorHost)
};

} // namespace openutau::vst
