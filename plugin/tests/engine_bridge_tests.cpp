#include "openutau_vst/engine_bridge.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void setEnvironment(const char* name, const juce::String& value) {
#if defined(_WIN32)
  _putenv_s(name, value.toRawUTF8());
#else
  setenv(name, value.toRawUTF8(), 1);
#endif
}

void clearEnvironment(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

class ScopedFault final {
public:
  explicit ScopedFault(const juce::String& mode)
      : directory_(juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("OpenUtauVstFault-" + juce::Uuid().toString())),
        sentinel_(directory_.getChildFile("consumed")) {
    directory_.createDirectory();
    setEnvironment("OPENUTAU_VST_FAULT_MODE", mode);
    setEnvironment("OPENUTAU_VST_FAULT_SENTINEL", sentinel_.getFullPathName());
    setEnvironment("OPENUTAU_VST_FAULT_DELAY_MS", "500");
  }

  ~ScopedFault() {
    clearEnvironment("OPENUTAU_VST_FAULT_MODE");
    clearEnvironment("OPENUTAU_VST_FAULT_SENTINEL");
    clearEnvironment("OPENUTAU_VST_FAULT_DELAY_MS");
    directory_.deleteRecursively();
  }

  [[nodiscard]] bool consumed() const { return sentinel_.existsAsFile(); }

private:
  juce::File directory_;
  juce::File sentinel_;
};

bool waitForAudio(openutau::vst::EngineBridge& bridge,
                  const std::uint64_t minimumStarts,
                  const std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<float, 512> audio{};
  std::int64_t position = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    bridge.beginBlock(position, 256, 48000.0, true);
    if (bridge.read(audio.data(), 256, position) == 256
        && bridge.engineStarts() >= minimumStarts) {
      return true;
    }
    position += 256;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool verifyAutomaticFaultRecovery(const juce::String& faultHost,
                                  const juce::String& mode) {
  ScopedFault fault(mode);
  openutau::vst::EngineBridge bridge(100);
  bridge.start({}, faultHost);
  const auto recovered = waitForAudio(bridge, 2, std::chrono::seconds(15));
  const auto error = bridge.lastError();
  const auto starts = bridge.engineStarts();
  bridge.stop();
  if (!fault.consumed() || !recovered || starts < 2) {
    std::cerr << "Engine did not recover automatically after " << mode
              << " fault (starts=" << starts << "): " << error << '\n';
    return false;
  }
  return true;
}

bool verifyMissingHostRecovery(const juce::File& faultHost) {
  const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
      .getChildFile("OpenUtauVstMissing-" + juce::Uuid().toString());
  if (directory.createDirectory().failed()) return false;
  const auto target = directory.getChildFile(faultHost.getFileName());
  const auto staging = directory.getChildFile("staging-" + faultHost.getFileName());

  openutau::vst::EngineBridge bridge(100);
  bridge.start({}, target.getFullPathName());
  const auto missingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  std::array<float, 512> audio{};
  bool safelySilent = false;
  while (std::chrono::steady_clock::now() < missingDeadline) {
    bridge.beginBlock(0, 256, 48000.0, true);
    safelySilent = bridge.read(audio.data(), 256, 0) == 0
        && bridge.lastError().containsIgnoreCase("does not exist");
    if (safelySilent) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const auto staged = faultHost.copyFileTo(staging)
      && staging.setExecutePermission(true)
      && staging.moveFileTo(target);
  const auto recovered = staged
      && waitForAudio(bridge, 1, std::chrono::seconds(15));
  const auto error = bridge.lastError();
  bridge.stop();
  directory.deleteRecursively();
  if (!safelySilent || !recovered) {
    std::cerr << "Missing engine host did not fail silent and recover when restored: "
              << error << '\n';
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 1;
  openutau::vst::EngineBridge bridge;
  bridge.start(argv[1], argv[2]);
  bridge.updateHostTiming(4.0, 150.0, 2, 7, 8);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  std::array<float, 512> audio{};
  std::size_t received = 0;
  std::int64_t position = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    bridge.beginBlock(position, 256, 48000.0, true);
    received = bridge.read(audio.data(), 256, position);
    if (received == 256) break;
    position += 256;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!bridge.connected() || received != 256) {
    std::cerr << "Render bridge failed: " << bridge.lastError() << '\n';
    return 1;
  }
  const auto cachedState = bridge.cachedProjectState().toString();
  if (!cachedState.contains("bpm: 150")) {
    std::cerr << "DAW timing mutation was not cached for host state.\n";
    return 1;
  }
  const auto initialStarts = bridge.engineStarts();
  bridge.requestRestart();
  const auto restartDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < restartDeadline
         && bridge.engineStarts() == initialStarts) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!bridge.connected() || bridge.engineStarts() != initialStarts + 1) {
    std::cerr << "Engine did not relaunch after restart request: "
              << bridge.lastError() << '\n';
    return 1;
  }
  received = 0;
  position = 8192;
  while (std::chrono::steady_clock::now() < restartDeadline) {
    bridge.beginBlock(position, 256, 48000.0, true);
    received = bridge.read(audio.data(), 256, position);
    if (received == 256) break;
    position += 256;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (received != 256) {
    std::cerr << "Rendering did not recover after engine relaunch: "
              << bridge.lastError() << '\n';
    return 1;
  }
  std::vector<float> offline;
  if (!bridge.renderOffline(0, 128, 96000.0, offline, 10000)
      || offline.size() != 256) {
    std::cerr << "Offline render failed: " << bridge.lastError() << '\n';
    return 1;
  }
  bridge.beginBlock(4096, 256, 48000.0, true); // seek/epoch change
  bridge.read(audio.data(), 256, 4096);
  bridge.beginBlock(4352, 256, 48000.0, false); // stop
  bridge.read(audio.data(), 256, 4352);
  bridge.stop();

  const juce::File faultHost(argv[3]);
  if (!verifyAutomaticFaultRecovery(faultHost.getFullPathName(), "exit")
      || !verifyAutomaticFaultRecovery(faultHost.getFullPathName(), "hang")
      || !verifyMissingHostRecovery(faultHost)) {
    return 1;
  }
  std::cout << "Transport, crash, hang, and missing-host recovery E2E passed.\n";
  return 0;
}
