#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Expected the built VST3 module path.");
    juce::ScopedJuceInitialiser_GUI juce;

    auto modulePath = std::filesystem::path(argv[1]);
    const auto bundlePath = modulePath.parent_path().parent_path().parent_path();
    require(std::filesystem::exists(bundlePath), "VST3 bundle does not exist.");

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundlePath.string());
    require(descriptions.size() == 1, "Expected exactly one VST3 class.");
    require(descriptions[0]->isInstrument, "VST3 was not classified as an instrument.");
    require(descriptions[0]->name == "OpenUtau DAW", "Unexpected plugin name.");

    juce::String error;
    auto instance = format.createInstanceFromDescription(*descriptions[0], 48000.0, 512, error);
    require(instance != nullptr, error.toRawUTF8());
    require(instance->getTotalNumInputChannels() == 0, "Instrument exposed audio inputs.");
    require(instance->getTotalNumOutputChannels() == 2, "Instrument is not stereo.");

    instance->setPlayConfigDetails(0, 2, 48000.0, 512);
    instance->prepareToPlay(48000.0, 512);
    require(instance->getLatencySamples() == 0,
            "The VST3 unexpectedly delayed the host timeline.");
    juce::AudioBuffer<float> audio(2, 512);
    audio.clear();
    juce::MidiBuffer midi;
    instance->processBlock(audio, midi);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
      require(audio.getMagnitude(channel, 0, audio.getNumSamples()) == 0.0f,
              "Disconnected engine did not produce safe silence.");
    }

    if (juce::SystemStats::getEnvironmentVariable(
            "OPENUTAU_VST_TEST_CREATE_EDITOR", {}) == "1") {
      std::unique_ptr<juce::AudioProcessorEditor> editor(
          instance->createEditorAndMakeActive());
      require(editor != nullptr, "Packaged VST3 did not create its editor.");
      if (editor->getWidth() < 800 || editor->getHeight() < 540) {
        auto diagnostic = "Packaged editor remained at fallback size "
            + std::to_string(editor->getWidth()) + "x"
            + std::to_string(editor->getHeight());
        if (auto* status = dynamic_cast<juce::Label*>(
                editor->findChildWithID("openutau-editor-status"))) {
          diagnostic += "; status: " + status->getText().toStdString();
        }
        diagnostic += "; editor: " + editor->getName().toStdString();
        throw std::runtime_error(diagnostic);
      }
    }

    juce::MemoryBlock state;
    instance->getStateInformation(state);
    require(state.getSize() >= 16, "State chunk header is missing.");
    instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    instance->releaseResources();
    std::cout << "VST3 discovery, instantiation, processing, and state tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
