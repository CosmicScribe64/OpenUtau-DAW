#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace openutau::vst {

class EngineClient final {
public:
  EngineClient() = default;
  ~EngineClient();
  EngineClient(const EngineClient&) = delete;
  EngineClient& operator=(const EngineClient&) = delete;

  bool launch(const juce::String& runtime, const juce::String& engineHost,
              juce::String& error, const juce::String& editorHost = {});
  void disconnect() noexcept;
  [[nodiscard]] bool connected() const noexcept;
  bool getState(juce::MemoryBlock& ustx, juce::String& error);
  bool setState(const void* data, std::size_t size, juce::String& error);
  bool setHostTiming(double quarterNotePosition, double tempo,
                     std::int64_t barPosition, int numerator, int denominator,
                     juce::String& error);
  bool openEditor(juce::String& projectPath, juce::String& error);
  bool render(std::int64_t epoch, std::int64_t startSample, int frameCount,
              double sampleRate, bool offline, std::vector<float>& samples,
              juce::String& error, int timeoutMs = 0);

private:
  bool request(const juce::String& kind, const juce::String& payload,
               juce::String& responsePayload, juce::String& error,
               int timeoutMs = 10000);
  bool writeFrame(const juce::String& json, juce::String& error, int timeoutMs);
  bool readFrame(juce::String& json, juce::String& error, int timeoutMs);

  juce::ChildProcess process_;
  std::unique_ptr<juce::StreamingSocket> socket_;
  juce::String token_;
  std::int64_t nextRequestId_{1};
};

} // namespace openutau::vst
