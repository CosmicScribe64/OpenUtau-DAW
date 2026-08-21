#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#if JUCE_MAC || JUCE_WINDOWS
#include "openutau_vst/managed_editor_host.hpp"
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#endif

namespace openutau::vst {

class PluginProcessor;

class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer {
public:
  explicit PluginEditor(PluginProcessor&);
  ~PluginEditor() override;
  void paint(juce::Graphics&) override;
  void resized() override;
  bool synchroniseProjectState(bool notifyHost = true);

private:
  void timerCallback() override;
  PluginProcessor& processor_;
  juce::Label title_;
  juce::Label status_;
  juce::TextButton openEditor_{"Open OpenUtau editor"};
  juce::TextButton loadProject_{"Load USTX..."};
  juce::TextButton restartEngine_{"Restart engine"};
  std::unique_ptr<juce::FileChooser> fileChooser_;
#if JUCE_MAC || JUCE_WINDOWS
  ManagedEditorHost managedEditor_;
#if JUCE_MAC
  juce::NSViewComponent embeddedView_;
#else
  juce::HWNDComponent embeddedView_;
#endif
  static constexpr std::size_t previewScratchFrames = 1u << 14;
  std::array<float, previewScratchFrames * 2> previewScratch_{};
  std::uint64_t previewRevision_{};
  std::uint64_t toneRevision_{};
  unsigned timerTick_{};
  bool editorFocusEstablished_{};
  bool embeddedActive_{};
#endif

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

} // namespace openutau::vst
