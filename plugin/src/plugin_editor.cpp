#include "openutau_vst/plugin_editor.hpp"
#include "openutau_vst/plugin_processor.hpp"

#include <algorithm>

namespace openutau::vst {

PluginEditor::PluginEditor(PluginProcessor& owner)
    : AudioProcessorEditor(owner), processor_(owner) {
  setName("OpenUtau DAW editor");
  title_.setText("OpenUtau DAW", juce::dontSendNotification);
  title_.setFont(juce::FontOptions(26.0f, juce::Font::bold));
  status_.setComponentID("openutau-editor-status");
  status_.setJustificationType(juce::Justification::centredLeft);
  openEditor_.setEnabled(false);
  openEditor_.setTooltip("The managed editor bridge is starting.");
  openEditor_.onClick = [this] { processor_.openFullEditor(); };
  loadProject_.onClick = [this] {
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Load an OpenUtau project", juce::File{}, "*.ustx;*.ust");
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser) {
          const auto file = chooser.getResult();
          if (file != juce::File{} && !processor_.loadProjectFile(file)) {
            status_.setText("Unable to load " + file.getFileName(), juce::dontSendNotification);
          }
          fileChooser_.reset();
        });
  };
  restartEngine_.onClick = [this] { processor_.restartEngine(); };
  addAndMakeVisible(title_);
  addAndMakeVisible(status_);
  addAndMakeVisible(openEditor_);
  addAndMakeVisible(loadProject_);
  addAndMakeVisible(restartEngine_);
  setResizable(true, true);
  setResizeLimits(420, 180, 1600, 1000);
  setSize(640, 240);
#if JUCE_MAC || JUCE_WINDOWS
  if (auto* view = managedEditor_.create(1136, 768)) {
#if JUCE_MAC
    embeddedView_.setView(view);
#else
    embeddedView_.setHWND(view);
#endif
    addAndMakeVisible(embeddedView_);
    title_.setVisible(false);
    status_.setVisible(false);
    openEditor_.setVisible(false);
    loadProject_.setVisible(false);
    restartEngine_.setVisible(false);
    embeddedActive_ = true;
    const auto state = processor_.projectStateSnapshot();
    if (!state.isEmpty()) {
      managedEditor_.setProjectState(state.getData(), state.getSize());
    } else {
      juce::MemoryBlock initialState;
      if (managedEditor_.pullProjectState(initialState)) {
        processor_.setEmbeddedProjectState(
            initialState.getData(), initialState.getSize());
      }
    }
    setResizeLimits(800, 540, 2400, 1600);
    setSize(1136, 768);
  } else if (managedEditor_.lastError().isNotEmpty()) {
    setName("Embedded editor unavailable — " + managedEditor_.lastError());
    status_.setText("Embedded editor unavailable — " + managedEditor_.lastError(),
                    juce::dontSendNotification);
  }
#endif
  // Transport animation must be visibly smooth, but all communication stays
  // on the host message thread and never enters the audio callback.
  startTimerHz(30);
}

PluginEditor::~PluginEditor() {
#if JUCE_MAC || JUCE_WINDOWS
  if (embeddedActive_) {
    synchroniseProjectState();
    processor_.updateEditorPreview(nullptr, 0, false);
  }
#if JUCE_MAC
  embeddedView_.setView(nullptr);
#else
  embeddedView_.setHWND(nullptr);
#endif
  managedEditor_.destroy();
#endif
}

bool PluginEditor::synchroniseProjectState(const bool notifyHost) {
#if JUCE_MAC || JUCE_WINDOWS
  if (embeddedActive_) {
    juce::MemoryBlock state;
    if (managedEditor_.pullProjectState(state)) {
      processor_.setEmbeddedProjectState(
          state.getData(), state.getSize(), notifyHost);
      return true;
    }
  }
#else
  juce::ignoreUnused(notifyHost);
#endif
  return false;
}

void PluginEditor::paint(juce::Graphics& graphics) {
  graphics.fillAll(juce::Colour(0xff17191f));
  graphics.setColour(juce::Colour(0xff36c2b4));
  graphics.fillRect(0, 0, 7, getHeight());
}

void PluginEditor::resized() {
#if JUCE_MAC || JUCE_WINDOWS
  if (embeddedActive_) {
    embeddedView_.setBounds(getLocalBounds());
    return;
  }
#endif
  auto area = getLocalBounds().reduced(28);
  title_.setBounds(area.removeFromTop(44));
  area.removeFromTop(14);
  status_.setBounds(area.removeFromTop(32));
  area.removeFromTop(14);
  auto buttons = area.removeFromTop(36);
  openEditor_.setBounds(buttons.removeFromLeft(210));
  buttons.removeFromLeft(12);
  loadProject_.setBounds(buttons.removeFromLeft(130));
  buttons.removeFromLeft(12);
  restartEngine_.setBounds(buttons.removeFromLeft(140));
}

void PluginEditor::timerCallback() {
#if JUCE_MAC || JUCE_WINDOWS
  if (embeddedActive_) {
    const auto transport = processor_.hostTransportSnapshot();
    managedEditor_.setHostTransport(
        transport.projectSample, transport.sampleRate,
        transport.quarterNotePosition, transport.bpm,
        transport.timeSignatureNumerator,
        transport.timeSignatureDenominator, transport.playing);
    std::size_t copiedPreviewFrames = 0;
    bool previewActive = false;
    const auto previewCapacity = std::min(
        previewScratchFrames, processor_.editorPreviewWritableFrames());
    if (managedEditor_.pullPreview(
            previewScratch_.data(), previewCapacity,
            copiedPreviewFrames, previewActive)) {
      processor_.updateEditorPreview(
          previewScratch_.data(), copiedPreviewFrames, previewActive);
    }
    synchroniseProjectState();
    return;
  }
  if (!embeddedActive_ && managedEditor_.lastError().isNotEmpty()) {
    status_.setText("Embedded editor unavailable — " + managedEditor_.lastError(),
                    juce::dontSendNotification);
    return;
  }
#endif
  if (processor_.engineConnected()) {
    status_.setText("Engine connected — underruns: "
        + juce::String(processor_.underrunCount()), juce::dontSendNotification);
    openEditor_.setEnabled(true);
  } else {
    const auto error = processor_.engineError();
    status_.setText(error.isNotEmpty()
        ? "Engine unavailable — " + error
        : "Engine starting — audio output is safely muted", juce::dontSendNotification);
    openEditor_.setEnabled(false);
  }
}

} // namespace openutau::vst
