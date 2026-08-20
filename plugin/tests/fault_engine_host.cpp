#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

bool readAll(juce::StreamingSocket& socket, void* data, const int size) {
  auto* bytes = static_cast<char*>(data);
  int received = 0;
  while (received < size) {
    if (socket.waitUntilReady(true, 10000) != 1) return false;
    const auto count = socket.read(bytes + received, size - received, false);
    if (count <= 0) return false;
    received += count;
  }
  return true;
}

bool writeAll(juce::StreamingSocket& socket, const void* data, const int size) {
  const auto* bytes = static_cast<const char*>(data);
  int written = 0;
  while (written < size) {
    if (socket.waitUntilReady(false, 10000) != 1) return false;
    const auto count = socket.write(bytes + written, size - written);
    if (count <= 0) return false;
    written += count;
  }
  return true;
}

bool readFrame(juce::StreamingSocket& socket, juce::String& json) {
  std::uint8_t header[4]{};
  if (!readAll(socket, header, static_cast<int>(sizeof(header)))) return false;
  const auto length = static_cast<int>(juce::ByteOrder::littleEndianInt(header));
  if (length < 0 || length > 256 * 1024 * 1024) return false;
  juce::MemoryBlock payload(static_cast<std::size_t>(length) + 1, true);
  if (!readAll(socket, payload.getData(), length)) return false;
  json = juce::String::fromUTF8(static_cast<const char*>(payload.getData()), length);
  return true;
}

bool writeFrame(juce::StreamingSocket& socket, const juce::String& json) {
  const auto* utf8 = json.toRawUTF8();
  const auto length = static_cast<int>(std::strlen(utf8));
  const auto littleEndianLength = juce::ByteOrder::swapIfBigEndian(
      static_cast<std::uint32_t>(length));
  std::uint8_t header[4]{};
  std::memcpy(header, &littleEndianLength, sizeof(littleEndianLength));
  return writeAll(socket, header, static_cast<int>(sizeof(header)))
      && writeAll(socket, utf8, length);
}

bool consumeFirstFault() {
  const auto sentinel = juce::SystemStats::getEnvironmentVariable(
      "OPENUTAU_VST_FAULT_SENTINEL", {});
  if (sentinel.isEmpty()) return false;
  const juce::File file(sentinel);
  if (file.existsAsFile()) return false;
  file.getParentDirectory().createDirectory();
  return file.create().wasOk();
}

juce::String responseJson(const juce::int64 requestId, const juce::String& kind,
                          const juce::String& payload) {
  auto response = std::make_unique<juce::DynamicObject>();
  response->setProperty("requestId", requestId);
  response->setProperty("kind", kind);
  if (payload.isNotEmpty()) response->setProperty("payload", payload);
  return juce::JSON::toString(juce::var(response.release()));
}

} // namespace

int main(int argc, char** argv) {
  int port = -1;
  juce::String token;
  for (int index = 1; index < argc; ++index) {
    const juce::String argument(argv[index]);
    if (argument == "--port" && index + 1 < argc) {
      port = juce::String(argv[++index]).getIntValue();
    } else if (argument == "--token" && index + 1 < argc) {
      token = argv[++index];
    }
  }
  if (port < 0 || port > 65535 || token.isEmpty()) return 2;

  juce::StreamingSocket listener;
  if (!listener.createListener(port, "127.0.0.1")) return 3;
  std::unique_ptr<juce::StreamingSocket> client(listener.waitForNextConnection());
  if (client == nullptr) return 4;

  auto state = juce::Base64::toBase64(
      juce::String("name: Fault recovery fixture\nustx_version: 0.6\n"));
  for (;;) {
    juce::String requestText;
    if (!readFrame(*client, requestText)) return 0;
    const auto request = juce::JSON::parse(requestText);
    if (!request.isObject()) return 5;
    const auto requestId = static_cast<juce::int64>(
        request.getProperty("requestId", -1));
    const auto kind = request.getProperty("kind", {}).toString();
    const auto payload = request.getProperty("payload", {}).toString();

    juce::String responseKind{"ok"};
    juce::String responsePayload;
    if (kind == "hello") {
      if (payload != token) {
        responseKind = "error";
        responsePayload = "Authentication failed.";
      } else {
        responsePayload = "5";
      }
    } else if (kind == "getState") {
      responsePayload = state;
    } else if (kind == "setState") {
      state = payload;
      responsePayload = "1";
    } else if (kind == "setHostTiming") {
      responsePayload = "1";
    } else if (kind == "render") {
      const auto mode = juce::SystemStats::getEnvironmentVariable(
          "OPENUTAU_VST_FAULT_MODE", {});
      if (mode.isNotEmpty() && consumeFirstFault()) {
        if (mode == "exit") {
          client->close();
          std::_Exit(86);
        }
        if (mode == "hang") {
          const auto delay = std::clamp(
              juce::SystemStats::getEnvironmentVariable(
                  "OPENUTAU_VST_FAULT_DELAY_MS", "500").getIntValue(),
              100, 10000);
          juce::Thread::sleep(delay);
        }
      }
      const auto render = juce::JSON::parse(payload);
      const auto frameCount = render.isObject()
          ? static_cast<int>(render.getProperty("frameCount", -1))
          : -1;
      if (frameCount < 0 || frameCount > (1 << 20)) {
        responseKind = "error";
        responsePayload = "Invalid render frame count.";
      } else {
        const std::vector<float> samples(
            static_cast<std::size_t>(frameCount) * 2, 0.0f);
        responsePayload = juce::Base64::toBase64(
            samples.data(), samples.size() * sizeof(float));
      }
    } else {
      responseKind = "error";
      responsePayload = "Unknown request.";
    }

    if (!writeFrame(*client, responseJson(requestId, responseKind, responsePayload))) {
      return 0;
    }
  }
}
