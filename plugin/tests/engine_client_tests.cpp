#include "openutau_vst/engine_client.hpp"

#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Expected runtime and engine host paths.\n";
    return 1;
  }
  openutau::vst::EngineClient client;
  juce::String error;
  if (!client.launch(argv[1], argv[2], error)) {
    std::cerr << error << '\n';
    return 1;
  }
  juce::MemoryBlock state;
  if (!client.getState(state, error) || state.getSize() == 0) {
    std::cerr << (error.isNotEmpty() ? error : "Empty state") << '\n';
    return 1;
  }
  if (!client.setState(state.getData(), state.getSize(), error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (!client.setHostTiming(4.0, 155.0, 2, 7, 8, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  juce::MemoryBlock timedState;
  if (!client.getState(timedState, error)
      || !timedState.toString().contains("bpm: 155")
      || !timedState.toString().contains("beat_per_bar: 7")) {
    std::cerr << (error.isNotEmpty() ? error : "Host timing was not persisted") << '\n';
    return 1;
  }
  juce::String editorProject;
  if (!client.openEditor(editorProject, error) || editorProject.isEmpty()) {
    std::cerr << (error.isNotEmpty() ? error : "Editor did not return a project path") << '\n';
    return 1;
  }
  juce::Thread::sleep(100);
  juce::MemoryBlock editedState;
  if (!client.getState(editedState, error)
      || !editedState.toString().contains("Edited in companion")) {
    std::cerr << (error.isNotEmpty() ? error : "Companion editor state was not synchronized") << '\n';
    return 1;
  }
  std::vector<float> audio;
  if (client.render(1, 0, 128, 48000.0, false, audio, error, 50)
      || !error.containsIgnoreCase("timed out")) {
    std::cerr << "Hung render did not meet its deadline: " << error << '\n';
    return 1;
  }
  std::cout << "Native-to-managed engine process and hang deadline E2E test passed.\n";
  return 0;
}
