#include <juce_audio_processors/juce_audio_processors.h>
#include "openutau_vst/engine_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void writeLittleEndian(std::ostream& stream, std::uint32_t value, int bytes) {
  for (int byte = 0; byte < bytes; ++byte) {
    stream.put(static_cast<char>((value >> (byte * 8)) & 0xff));
  }
}

void writeFloatWav(const std::filesystem::path& path,
                   const std::vector<float>& interleaved,
                   const int sampleRate) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  require(stream.good(), "Could not create realtime E2E capture.");
  const auto dataBytes = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
  stream.write("RIFF", 4);
  writeLittleEndian(stream, 36u + dataBytes, 4);
  stream.write("WAVEfmt ", 8);
  writeLittleEndian(stream, 16, 4);
  writeLittleEndian(stream, 3, 2); // IEEE float
  writeLittleEndian(stream, 2, 2);
  writeLittleEndian(stream, static_cast<std::uint32_t>(sampleRate), 4);
  writeLittleEndian(stream, static_cast<std::uint32_t>(sampleRate * 2 * sizeof(float)), 4);
  writeLittleEndian(stream, 2 * sizeof(float), 2);
  writeLittleEndian(stream, 8 * sizeof(float), 2);
  stream.write("data", 4);
  writeLittleEndian(stream, dataBytes, 4);
  stream.write(reinterpret_cast<const char*>(interleaved.data()), dataBytes);
  require(stream.good(), "Could not finish realtime E2E capture.");
}

class TestPlayHead final : public juce::AudioPlayHead {
public:
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo info;
    info.setTimeInSamples(samplePosition);
    info.setTimeInSeconds(static_cast<double>(samplePosition) / sampleRate);
    info.setBpm(120.0);
    info.setTimeSignature(TimeSignature{4, 4});
    info.setPpqPosition(static_cast<double>(samplePosition) / sampleRate * 2.0);
    info.setPpqPositionOfLastBarStart(0.0);
    info.setBarCount(static_cast<juce::int64>(0));
    info.setIsPlaying(playing);
    info.setIsRecording(false);
    info.setIsLooping(false);
    return info;
  }

  void advance(int frames) { samplePosition += frames; }
  double sampleRate{48000.0};
  juce::int64 samplePosition{0};
  bool playing{false};
};

juce::MemoryBlock makeProcessorState(const juce::MemoryBlock& ustx) {
  juce::MemoryBlock processorState;
  juce::MemoryOutputStream stream(processorState, false);
  stream.writeIntBigEndian(static_cast<int>(0x4f555633));
  stream.writeIntBigEndian(1);
  stream.writeInt64BigEndian(static_cast<juce::int64>(ustx.getSize()));
  stream.write(ustx.getData(), ustx.getSize());
  return processorState;
}

juce::MemoryBlock makeHostedVst3State(const juce::MemoryBlock& defaultHostState,
                                      const juce::MemoryBlock& ustx) {
  auto xml = juce::AudioProcessor::getXmlFromBinary(
      defaultHostState.getData(), static_cast<int>(defaultHostState.getSize()));
  require(xml != nullptr && xml->hasTagName("VST3PluginState"),
          "Could not decode JUCE VST3 host state.");
  auto* component = xml->getChildByName("IComponent");
  require(component != nullptr, "VST3 host state has no component payload.");
  component->deleteAllTextElements();
  component->addTextElement(makeProcessorState(ustx).toBase64Encoding());
  juce::MemoryBlock result;
  juce::AudioProcessor::copyXmlToBinary(*xml, result);
  return result;
}

std::vector<float> renderDirectReference(const juce::MemoryBlock& ustx,
                                         const double sampleRate,
                                         const int frameCount) {
  openutau::vst::EngineClient direct;
  juce::String error;
  const auto host = juce::SystemStats::getEnvironmentVariable(
      "OPENUTAU_VST_ENGINE_HOST", {});
  const auto runtime = juce::SystemStats::getEnvironmentVariable(
      "OPENUTAU_VST_DOTNET", host.endsWithIgnoreCase(".dll") ? "dotnet" : "");
  require(direct.launch(runtime, host, error), error.toRawUTF8());
  require(direct.setState(ustx.getData(), ustx.getSize(), error), error.toRawUTF8());
  std::vector<float> result;
  require(direct.render(0, 0, frameCount, sampleRate, true, result, error),
          error.toRawUTF8());
  const auto peak = std::ranges::max(
      result | std::views::transform([](float sample) { return std::abs(sample); }));
  require(peak > 1e-5f, "Direct sidecar preflight produced no audible fixture output.");
  return result;
}

juce::String extractUstx(const juce::MemoryBlock& hostedState) {
  auto xml = juce::AudioProcessor::getXmlFromBinary(
      hostedState.getData(), static_cast<int>(hostedState.getSize()));
  require(xml != nullptr, "VST3 returned an invalid state envelope.");
  const auto* component = xml->getChildByName("IComponent");
  require(component != nullptr, "VST3 returned no component state.");
  juce::MemoryBlock processor;
  require(processor.fromBase64Encoding(component->getAllSubText()),
          "VST3 returned invalid component state encoding.");
  juce::MemoryInputStream stream(processor, false);
  require(static_cast<std::uint32_t>(stream.readIntBigEndian()) == 0x4f555633,
          "VST3 returned the wrong state magic.");
  require(stream.readIntBigEndian() == 1, "VST3 returned the wrong state version.");
  const auto bytes = stream.readInt64BigEndian();
  require(bytes > 0 && bytes <= stream.getNumBytesRemaining(),
          "VST3 lost its USTX state payload.");
  juce::MemoryBlock ustx(static_cast<std::size_t>(bytes));
  require(stream.read(ustx.getData(), static_cast<int>(bytes)) == bytes,
          "VST3 returned truncated USTX state.");
  return juce::String::fromUTF8(
      static_cast<const char*>(ustx.getData()), static_cast<int>(ustx.getSize()));
}

struct Quality final {
  double correlation{};
  double activeCoverage{};
  std::uint64_t expectedActiveFrames{};
};

Quality compareRealtime(const std::vector<float>& expected,
                        const std::vector<float>& actual,
                        const int latency) {
  double dot = 0.0;
  double expectedEnergy = 0.0;
  double actualEnergy = 0.0;
  std::uint64_t expectedActiveFrames = 0;
  std::uint64_t capturedActiveFrames = 0;
  const auto liveFrames = actual.size() / 2;
  const auto directFrames = expected.size() / 2;
  for (std::size_t hostFrame = static_cast<std::size_t>(latency + 256);
       hostFrame < liveFrames; ++hostFrame) {
    const auto projectFrame = hostFrame - static_cast<std::size_t>(latency);
    if (projectFrame >= directFrames) break;
    const auto expectedLeft = expected[projectFrame * 2];
    const auto expectedRight = expected[projectFrame * 2 + 1];
    const auto actualLeft = actual[hostFrame * 2];
    const auto actualRight = actual[hostFrame * 2 + 1];
    const auto expectedPeak = std::max(std::abs(expectedLeft), std::abs(expectedRight));
    const auto actualPeak = std::max(std::abs(actualLeft), std::abs(actualRight));
    if (expectedPeak > 1e-5f) {
      ++expectedActiveFrames;
      if (actualPeak > 1e-5f) ++capturedActiveFrames;
    }
    dot += expectedLeft * actualLeft + expectedRight * actualRight;
    expectedEnergy += expectedLeft * expectedLeft + expectedRight * expectedRight;
    actualEnergy += actualLeft * actualLeft + actualRight * actualRight;
  }
  Quality quality;
  quality.expectedActiveFrames = expectedActiveFrames;
  quality.correlation = dot / std::sqrt(expectedEnergy * actualEnergy);
  quality.activeCoverage = expectedActiveFrames == 0 ? 0.0
      : static_cast<double>(capturedActiveFrames)
          / static_cast<double>(expectedActiveFrames);
  return quality;
}

void verifyHostCallbackMatrix(juce::VST3PluginFormat& format,
                              const juce::PluginDescription& description,
                              const juce::MemoryBlock& ustx) {
  // This is a real VST3 host callback matrix, not an Engine-only resampler
  // check. It covers the host shapes that most often reveal callback slicing,
  // transport discontinuity, or sample-rate conversion mistakes.
  constexpr std::array<double, 3> sampleRates{44100.0, 48000.0, 96000.0};
  constexpr std::array<int, 6> blockSizes{64, 128, 256, 512, 1024, 2048};
  // A render-ahead refill is 8,192 source frames. At 96 kHz it can cover a
  // noticeable fraction of a short assertion window on a busy CI worker, so
  // measure long enough to distinguish one bounded refill from sustained
  // callback starvation.
  constexpr double playbackSeconds = 1.5;
  for (const auto sampleRate : sampleRates) {
    const auto direct = renderDirectReference(
        ustx, sampleRate, static_cast<int>(std::ceil(sampleRate * 2.0)));
    juce::String error;
    auto instance = format.createInstanceFromDescription(
        description, sampleRate, blockSizes.front(), error);
    require(instance != nullptr, error.toRawUTF8());
    juce::MemoryBlock defaultHostState;
    instance->getStateInformation(defaultHostState);
    const auto state = makeHostedVst3State(defaultHostState, ustx);
    instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (const auto blockSize : blockSizes) {
      TestPlayHead playHead;
      playHead.sampleRate = sampleRate;
      instance->setNonRealtime(false);
      instance->releaseResources();
      instance->setPlayConfigDetails(0, 2, sampleRate, blockSize);
      instance->setPlayHead(&playHead);
      instance->prepareToPlay(sampleRate, blockSize);
      juce::AudioBuffer<float> audio(2, blockSize);
      juce::MidiBuffer midi;

      // The bridge renders ahead while the host is stopped. Give it a bounded
      // amount of genuine callback time before making the live assertion.
      for (int prewarm = 0; prewarm < 200; ++prewarm) {
        audio.clear();
        instance->processBlock(audio, midi);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }

      playHead.playing = true;
      const auto blocks = static_cast<int>(std::ceil(
          sampleRate * playbackSeconds / static_cast<double>(blockSize)));
      std::vector<float> actual;
      actual.reserve(static_cast<std::size_t>(blocks * blockSize * 2));
      bool heard = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      for (int block = 0; block < blocks; ++block) {
        require(std::chrono::steady_clock::now() < deadline,
                "Host callback matrix did not complete within its deadline.");
        audio.clear();
        instance->processBlock(audio, midi);
        playHead.advance(blockSize);
        heard = heard || audio.getMagnitude(0, 0, blockSize) > 1e-5f
            || audio.getMagnitude(1, 0, blockSize) > 1e-5f;
        for (int frame = 0; frame < blockSize; ++frame) {
          actual.push_back(audio.getSample(0, frame));
          actual.push_back(audio.getSample(1, frame));
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(
            static_cast<double>(blockSize) / sampleRate));
      }
      const auto quality = compareRealtime(direct, actual, instance->getLatencySamples());
      std::cout << "Host matrix " << sampleRate << " Hz / " << blockSize
                << " frames: correlation=" << quality.correlation
                << ", active coverage=" << quality.activeCoverage << '\n';
      require(heard, "VST3 host callback matrix produced silence.");
      require(std::ranges::all_of(actual, [](const float sample) {
                return std::isfinite(sample);
              }),
              "VST3 host callback matrix produced a non-finite sample.");
      require(quality.expectedActiveFrames > 0 && quality.activeCoverage > 0.90,
              "VST3 host callback matrix dropped audible frames.");
      require(std::isfinite(quality.correlation) && quality.correlation > 0.90,
              "VST3 host callback matrix diverged from the direct singing reference.");
    }
    instance->releaseResources();
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 4, "Expected VST3 module and two USTX fixture paths.");
    juce::ScopedJuceInitialiser_GUI juce;
    const auto modulePath = std::filesystem::path(argv[1]);
    const auto bundlePath = modulePath.parent_path().parent_path().parent_path();
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile(descriptions, bundlePath.string());
    require(descriptions.size() == 1, "Audible E2E could not discover VST3.");

    juce::String error;
    // FL Studio commonly processes substantially larger live buffers than the
    // 256-frame slices used internally by the processor. Exercise that host
    // shape so an underrun in the first slice cannot make the remaining host
    // block look like a transport seek on the next callback.
    constexpr int hostBlockFrames = 2048;
    auto instance = format.createInstanceFromDescription(
        *descriptions[0], 48000.0, hostBlockFrames, error);
    require(instance != nullptr, error.toRawUTF8());
    auto alternateInstance = format.createInstanceFromDescription(
        *descriptions[0], 48000.0, hostBlockFrames, error);
    require(alternateInstance != nullptr, error.toRawUTF8());
    juce::MemoryBlock ustx;
    require(juce::File(argv[2]).loadFileAsData(ustx), "Could not load USTX fixture.");
    juce::MemoryBlock alternateUstx;
    require(juce::File(argv[3]).loadFileAsData(alternateUstx),
            "Could not load alternate USTX fixture.");
    const auto directAudio = renderDirectReference(ustx, 48000.0, 144000);
    const auto directAlternateAudio = renderDirectReference(alternateUstx, 48000.0, 144000);
    juce::MemoryBlock defaultHostState;
    instance->getStateInformation(defaultHostState);
    const auto state = makeHostedVst3State(defaultHostState, ustx);
    instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    juce::MemoryBlock alternateDefaultState;
    alternateInstance->getStateInformation(alternateDefaultState);
    const auto alternateState = makeHostedVst3State(
        alternateDefaultState, alternateUstx);
    alternateInstance->setStateInformation(
        alternateState.getData(), static_cast<int>(alternateState.getSize()));

    TestPlayHead playHead;
    TestPlayHead alternatePlayHead;
    instance->setPlayHead(&playHead);
    alternateInstance->setPlayHead(&alternatePlayHead);
    instance->setPlayConfigDetails(0, 2, 48000.0, hostBlockFrames);
    alternateInstance->setPlayConfigDetails(0, 2, 48000.0, hostBlockFrames);
    instance->prepareToPlay(48000.0, hostBlockFrames);
    alternateInstance->prepareToPlay(48000.0, hostBlockFrames);

    juce::AudioBuffer<float> audio(2, hostBlockFrames);
    juce::AudioBuffer<float> alternateAudio(2, hostBlockFrames);
    juce::MidiBuffer midi;
    // Give the sidecar time to initialize and apply state while transport is
    // stopped. All process calls still exercise the VST callback contract.
    for (int block = 0; block < 200; ++block) {
      audio.clear();
      instance->processBlock(audio, midi);
      alternateAudio.clear();
      alternateInstance->processBlock(alternateAudio, midi);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    juce::MemoryBlock roundTripState;
    instance->getStateInformation(roundTripState);
    require(roundTripState.getSize() >= 16, "VST3 returned an invalid state header.");
    const auto roundTripUstx = extractUstx(roundTripState);
    require(roundTripUstx.contains("singer:")
                && roundTripUstx.contains("VST audible E2E fixture"),
            "VST3 state no longer contains its singer assignment.");
    juce::MemoryBlock alternateRoundTripState;
    alternateInstance->getStateInformation(alternateRoundTripState);
    const auto alternateRoundTripUstx = extractUstx(alternateRoundTripState);
    require(alternateRoundTripUstx.contains("VST alternate instance fixture")
                && !alternateRoundTripUstx.contains("VST audible E2E fixture"),
            "Two loaded VST3 instances exchanged their USTX state.");

    // Realtime playback exercises the lock-free render-ahead path used by FL
    // Studio. Stopped-transport prewarming must make project sample zero ready
    // without shifting the host timeline.
    playHead.playing = true;
    alternatePlayHead.playing = true;
    bool heardRealtime = false;
    bool heardAlternateRealtime = false;
    std::vector<float> realtimeAudio;
    std::vector<float> alternateRealtimeAudio;
    constexpr int realtimeBlocks = 71; // Just over three seconds at 48 kHz.
    realtimeAudio.reserve(realtimeBlocks * hostBlockFrames * 2);
    alternateRealtimeAudio.reserve(realtimeBlocks * hostBlockFrames * 2);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    for (int block = 0; block < realtimeBlocks; ++block) {
      require(std::chrono::steady_clock::now() < deadline,
              "Realtime VST3 playback did not complete within 12 seconds.");
      audio.clear();
      instance->processBlock(audio, midi);
      alternateAudio.clear();
      alternateInstance->processBlock(alternateAudio, midi);
      playHead.advance(audio.getNumSamples());
      alternatePlayHead.advance(alternateAudio.getNumSamples());
      if (audio.getMagnitude(0, 0, audio.getNumSamples()) > 1e-5f
          || audio.getMagnitude(1, 0, audio.getNumSamples()) > 1e-5f) {
        heardRealtime = true;
      }
      if (alternateAudio.getMagnitude(0, 0, alternateAudio.getNumSamples()) > 1e-5f
          || alternateAudio.getMagnitude(1, 0, alternateAudio.getNumSamples()) > 1e-5f) {
        heardAlternateRealtime = true;
      }
      for (int frame = 0; frame < audio.getNumSamples(); ++frame) {
        realtimeAudio.push_back(audio.getSample(0, frame));
        realtimeAudio.push_back(audio.getSample(1, frame));
        alternateRealtimeAudio.push_back(alternateAudio.getSample(0, frame));
        alternateRealtimeAudio.push_back(alternateAudio.getSample(1, frame));
      }
      std::this_thread::sleep_for(
          std::chrono::duration<double>(static_cast<double>(hostBlockFrames) / 48000.0));
    }
    require(heardRealtime,
            "Realtime VST3 render-ahead chain produced no audible output.");
    require(heardAlternateRealtime,
            "Second realtime VST3 instance produced no audible output.");

    // A nonzero fragment is not enough: missing render-ahead slices sound
    // slowed, gated, or distorted in a DAW. Compare the complete live output
    // against the same sidecar's direct reference at the plugin-reported
    // latency (zero in normal use), tolerating only its short startup ramp.
    const auto quality = compareRealtime(
        directAudio, realtimeAudio, instance->getLatencySamples());
    const auto alternateQuality = compareRealtime(
        directAlternateAudio, alternateRealtimeAudio,
        alternateInstance->getLatencySamples());
    std::cout << "Realtime quality: correlation=" << quality.correlation
              << ", active coverage=" << quality.activeCoverage << '\n';
    std::cout << "Second-instance quality: correlation="
              << alternateQuality.correlation << ", active coverage="
              << alternateQuality.activeCoverage << '\n';
    const auto capturePath = juce::SystemStats::getEnvironmentVariable(
        "OPENUTAU_VST_CAPTURE_WAV", {});
    if (capturePath.isNotEmpty()) {
      writeFloatWav(capturePath.toStdString(), realtimeAudio, 48000);
      writeFloatWav((capturePath + ".direct.wav").toStdString(), directAudio, 48000);
    }
    require(quality.expectedActiveFrames > 0,
            "Direct singing reference had no active frames.");
    require(quality.activeCoverage > 0.95,
            "Realtime VST3 output dropped audible singing frames.");
    require(std::isfinite(quality.correlation) && quality.correlation > 0.95,
            "Realtime VST3 output did not match the clean singing reference.");
    require(alternateQuality.expectedActiveFrames > 0,
            "Second direct singing reference had no active frames.");
    require(alternateQuality.activeCoverage > 0.95,
            "Second VST3 instance dropped audible singing frames.");
    require(std::isfinite(alternateQuality.correlation)
                && alternateQuality.correlation > 0.95,
            "Second VST3 instance did not match its independent project reference.");

    alternatePlayHead.playing = false;
    alternateInstance->releaseResources();

    verifyHostCallbackMatrix(format, *descriptions[0], ustx);
    instance->setPlayHead(&playHead);
    playHead.sampleRate = 48000.0;

    // FL Studio releases and immediately re-prepares the processor when it
    // switches from live playback to a bounce. The external singing engine
    // must survive that lifecycle transition or the complete export can run
    // before its replacement process has initialized.
    instance->releaseResources();
    instance->prepareToPlay(48000.0, hostBlockFrames);

    // Offline export invokes the sidecar synchronously at the host's exact
    // project position.
    playHead.playing = false;
    instance->setNonRealtime(true);
    playHead.samplePosition = instance->getLatencySamples();
    // Match FL Studio's bounce behavior: the processor is in non-realtime
    // mode, but the playhead can report that live transport is not playing.
    playHead.playing = false;
    bool heardOffline = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    // Process a complete two-second bounce, not merely the first audible
    // block. This catches hosts such as FL Studio that request hundreds of
    // small offline blocks and requires the bridge's contiguous render cache.
    for (int block = 0; block < 48; ++block) {
      require(std::chrono::steady_clock::now() < deadline,
              "Offline VST3 export did not complete within 30 seconds.");
      audio.clear();
      instance->processBlock(audio, midi);
      playHead.advance(audio.getNumSamples());
      heardOffline = heardOffline
          || audio.getMagnitude(0, 0, audio.getNumSamples()) > 1e-5f
          || audio.getMagnitude(1, 0, audio.getNumSamples()) > 1e-5f;
    }
    require(heardOffline, "Offline VST3 export chain produced no audible output.");

    juce::MemoryBlock saved;
    instance->getStateInformation(saved);
    require(saved.getSize() > 16, "Audible E2E did not preserve USTX host state.");
    instance->releaseResources();
    std::cout << "Realtime and offline VST3-to-sidecar Worldline E2E passed.\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
