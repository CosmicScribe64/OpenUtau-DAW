#include "openutau_vst/engine_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace openutau::vst {
namespace {

constexpr int maximumFrameBytes = 256 * 1024 * 1024;

int remainingMilliseconds(const double deadline) {
  return std::max(0, static_cast<int>(
      std::ceil(deadline - juce::Time::getMillisecondCounterHiRes())));
}

bool writeAll(juce::StreamingSocket& socket, const void* data, int size,
              const double deadline) {
  const auto* bytes = static_cast<const char*>(data);
  int written = 0;
  while (written < size) {
    const auto remaining = remainingMilliseconds(deadline);
    if (remaining == 0 || socket.waitUntilReady(false, remaining) != 1) return false;
    const auto count = socket.write(bytes + written, size - written);
    if (count <= 0) return false;
    written += count;
  }
  return true;
}

bool readAll(juce::StreamingSocket& socket, void* data, int size,
             const double deadline) {
  auto* bytes = static_cast<char*>(data);
  int received = 0;
  while (received < size) {
    const auto remaining = remainingMilliseconds(deadline);
    if (remaining == 0 || socket.waitUntilReady(true, remaining) != 1) return false;
    const auto count = socket.read(bytes + received, size - received, false);
    if (count <= 0) return false;
    received += count;
  }
  return true;
}

} // namespace

EngineClient::~EngineClient() { disconnect(); }

bool EngineClient::launch(const juce::String& runtime, const juce::String& engineHost,
                          juce::String& error, const juce::String& editorHost) {
  disconnect();
  if (!juce::File(engineHost).existsAsFile()) {
    error = "Engine host does not exist: " + engineHost;
    return false;
  }
  token_ = juce::Uuid().toString() + juce::Uuid().toString();
  juce::StreamingSocket portProbe;
  if (!portProbe.createListener(0, "127.0.0.1")) {
    error = "Could not reserve a loopback port for the engine.";
    return false;
  }
  const auto port = portProbe.getBoundPort();
  portProbe.close();
  juce::StringArray arguments;
  if (runtime.isNotEmpty()) arguments.add(runtime);
  arguments.add(engineHost);
  arguments.addArray({"--port", juce::String(port), "--token", token_});
  if (editorHost.isNotEmpty()) arguments.addArray({"--editor", editorHost});
  if (!process_.start(arguments, 0)) {
    error = "Could not start the OpenUtau engine host.";
    return false;
  }
  const auto deadline = juce::Time::getMillisecondCounter() + 10000u;
  while (juce::Time::getMillisecondCounter() < deadline && process_.isRunning()) {
    socket_ = std::make_unique<juce::StreamingSocket>();
    if (socket_->connect("127.0.0.1", port, 250)) break;
    socket_.reset();
    juce::Thread::sleep(25);
  }
  if (!socket_) {
    error = "Could not connect to the OpenUtau engine host.";
    disconnect();
    return false;
  }
  juce::String protocol;
  if (!request("hello", token_, protocol, error) || protocol != "5") {
    if (error.isEmpty()) error = "Engine protocol version mismatch.";
    disconnect();
    return false;
  }
  return true;
}

void EngineClient::disconnect() noexcept {
  if (socket_) socket_->close();
  socket_.reset();
  if (process_.isRunning()) process_.kill();
  token_.clear();
  nextRequestId_ = 1;
}

bool EngineClient::connected() const noexcept {
  return socket_ != nullptr && process_.isRunning();
}

bool EngineClient::getState(juce::MemoryBlock& ustx, juce::String& error) {
  juce::String payload;
  if (!request("getState", {}, payload, error)) return false;
  juce::MemoryOutputStream decoded;
  if (!juce::Base64::convertFromBase64(decoded, payload)) {
    error = "Engine returned invalid state encoding.";
    return false;
  }
  ustx = decoded.getMemoryBlock();
  return true;
}

bool EngineClient::setState(const void* data, const std::size_t size, juce::String& error) {
  juce::String ignored;
  return request("setState", juce::Base64::toBase64(data, size), ignored, error);
}

bool EngineClient::setHostTiming(const double quarterNotePosition, const double tempo,
                                 const std::int64_t barPosition, const int numerator,
                                 const int denominator,
                                 juce::String& error) {
  auto timing = std::make_unique<juce::DynamicObject>();
  timing->setProperty("quarterNotePosition", quarterNotePosition);
  timing->setProperty("tempo", tempo);
  timing->setProperty("barPosition", static_cast<juce::int64>(barPosition));
  timing->setProperty("timeSignatureNumerator", numerator);
  timing->setProperty("timeSignatureDenominator", denominator);
  juce::String ignored;
  return request("setHostTiming",
                 juce::JSON::toString(juce::var(timing.release())), ignored, error);
}

bool EngineClient::openEditor(juce::String& projectPath, juce::String& error) {
  return request("openEditor", {}, projectPath, error);
}

bool EngineClient::render(const std::int64_t epoch, const std::int64_t startSample,
                          const int frameCount, const double sampleRate, const bool offline,
                          std::vector<float>& samples, juce::String& error,
                          const int timeoutMs) {
  auto renderObject = std::make_unique<juce::DynamicObject>();
  renderObject->setProperty("epoch", static_cast<juce::int64>(epoch));
  renderObject->setProperty("startSample", static_cast<juce::int64>(startSample));
  renderObject->setProperty("frameCount", frameCount);
  renderObject->setProperty("sampleRate", sampleRate);
  renderObject->setProperty("offline", offline);
  juce::String encoded;
  if (!request("render", juce::JSON::toString(juce::var(renderObject.release())),
               encoded, error,
               timeoutMs > 0 ? timeoutMs : (offline ? 300000 : 30000))) {
    return false;
  }
  juce::MemoryOutputStream decoded;
  if (!juce::Base64::convertFromBase64(decoded, encoded)) {
    error = "Engine returned invalid audio encoding.";
    return false;
  }
  const auto bytes = decoded.getDataSize();
  const auto expected = static_cast<std::size_t>(frameCount) * 2 * sizeof(float);
  if (bytes != expected) {
    error = "Engine returned an unexpected audio block size.";
    return false;
  }
  samples.resize(static_cast<std::size_t>(frameCount) * 2);
  std::memcpy(samples.data(), decoded.getData(), bytes);
  return true;
}

bool EngineClient::request(const juce::String& kind, const juce::String& payload,
                           juce::String& responsePayload, juce::String& error,
                           const int timeoutMs) {
  if (!socket_) {
    error = "Engine is not connected.";
    return false;
  }
  const auto id = nextRequestId_++;
  auto object = std::make_unique<juce::DynamicObject>();
  object->setProperty("requestId", static_cast<juce::int64>(id));
  object->setProperty("kind", kind);
  if (payload.isNotEmpty()) object->setProperty("payload", payload);
  if (!writeFrame(juce::JSON::toString(juce::var(object.release())), error, timeoutMs)) return false;
  juce::String responseJson;
  if (!readFrame(responseJson, error, timeoutMs)) return false;
  const auto response = juce::JSON::parse(responseJson);
  if (!response.isObject()) {
    error = "Engine returned malformed JSON.";
    return false;
  }
  const auto responseId = static_cast<juce::int64>(response.getProperty("requestId", -1));
  const auto responseKind = response.getProperty("kind", {}).toString();
  responsePayload = response.getProperty("payload", {}).toString();
  if (responseId != id) {
    error = "Engine response ID mismatch.";
    return false;
  }
  if (responseKind != "ok") {
    error = responsePayload.isNotEmpty() ? responsePayload : "Engine request failed.";
    return false;
  }
  return true;
}

bool EngineClient::writeFrame(const juce::String& json, juce::String& error,
                              const int timeoutMs) {
  const auto utf8 = json.toRawUTF8();
  const auto length = static_cast<int>(std::strlen(utf8));
  if (length < 0 || length > maximumFrameBytes) {
    error = "Control frame is too large.";
    return false;
  }
  std::uint8_t header[4];
  const auto littleEndianLength = juce::ByteOrder::swapIfBigEndian(
      static_cast<std::uint32_t>(length));
  std::memcpy(header, &littleEndianLength, sizeof(littleEndianLength));
  const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
  if (!writeAll(*socket_, header, 4, deadline)
      || !writeAll(*socket_, utf8, length, deadline)) {
    error = "Timed out or disconnected while writing to engine.";
    return false;
  }
  return true;
}

bool EngineClient::readFrame(juce::String& json, juce::String& error,
                             const int timeoutMs) {
  std::uint8_t header[4]{};
  const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
  if (!readAll(*socket_, header, 4, deadline)) {
    error = "Engine timed out or disconnected while reading a frame header.";
    return false;
  }
  const auto length = static_cast<int>(juce::ByteOrder::littleEndianInt(header));
  if (length < 0 || length > maximumFrameBytes) {
    error = "Engine frame size is invalid.";
    return false;
  }
  juce::MemoryBlock payload(static_cast<std::size_t>(length) + 1, true);
  if (!readAll(*socket_, payload.getData(), length, deadline)) {
    error = "Engine timed out or disconnected while reading a frame.";
    return false;
  }
  json = juce::String::fromUTF8(static_cast<const char*>(payload.getData()), length);
  return true;
}

} // namespace openutau::vst
