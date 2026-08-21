#include "openutau_vst/managed_editor_host.hpp"
#include "openutau_vst/mac_space_key_forwarder.hpp"

#include <algorithm>
#include <dlfcn.h>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace {

using hostfxr_handle = void*;
using init_runtime_fn = int32_t (*)(const char*, const void*, hostfxr_handle*);
using get_delegate_fn = int32_t (*)(hostfxr_handle, int32_t, void**);
using close_runtime_fn = int32_t (*)(hostfxr_handle);
using load_assembly_fn = int32_t (*)(const char*, const char*, const char*,
                                     const char*, void*, void**);
using create_editor_fn = void* (*)(int32_t, int32_t, int64_t);
using destroy_editor_fn = void (*)(void*);
using get_error_fn = const char* (*)();
using get_revision_fn = int64_t (*)();
using execute_undo_fn = int32_t (*)(int32_t);
using execute_delete_fn = int32_t (*)(int32_t);
using copy_state_fn = int32_t (*)(std::uint8_t*, int32_t);
using set_state_fn = int32_t (*)(const std::uint8_t*, int32_t);
using set_host_transport_fn = int32_t (*)(
    int64_t, double, double, double, int32_t, int32_t, int32_t);
using get_preview_state_fn = int32_t (*)();
using get_preview_revision_fn = int64_t (*)();
using get_preview_tone_fn = int32_t (*)(double*, int64_t*);
using copy_preview_fn = int32_t (*)(float*, int32_t);

struct hostfxr_initialize_parameters {
  std::size_t size;
  const char* hostPath;
  const char* dotnetRoot;
};

constexpr int32_t loadAssemblyAndGetFunctionPointerDelegate = 5;
const char* const unmanagedCallersOnly = reinterpret_cast<const char*>(-1);

juce::File currentPluginModule() {
  Dl_info info{};
  return dladdr(reinterpret_cast<void*>(&currentPluginModule), &info) != 0
          && info.dli_fname != nullptr
      ? juce::File(info.dli_fname)
      : juce::File{};
}

struct Runtime final {
  std::mutex mutex;
  void* library{};
  void* editorOwner{};
  create_editor_fn create{};
  destroy_editor_fn destroy{};
  get_error_fn getError{};
  get_revision_fn getRevision{};
  execute_undo_fn executeUndo{};
  execute_delete_fn executeDelete{};
  copy_state_fn copyState{};
  set_state_fn setState{};
  set_host_transport_fn setHostTransport{};
  get_preview_state_fn getPreviewState{};
  get_preview_revision_fn getPreviewRevision{};
  get_preview_tone_fn getPreviewTone{};
  copy_preview_fn copyPreview{};
  juce::String error;

  bool claimEditor(void* const owner) {
    std::scoped_lock lock(mutex);
    if (editorOwner != nullptr && editorOwner != owner) return false;
    editorOwner = owner;
    return true;
  }

  void releaseEditor(void* const owner) {
    std::scoped_lock lock(mutex);
    if (editorOwner == owner) editorOwner = nullptr;
  }

  bool initialize() {
    std::scoped_lock lock(mutex);
    if (create != nullptr && destroy != nullptr) return true;
    if (error.isNotEmpty()) return false;

    const auto hostExecutable = juce::File::getSpecialLocation(
        juce::File::hostApplicationPath);
    auto appBundle = hostExecutable;
    while (appBundle != juce::File{}
           && !appBundle.hasFileExtension("app")
           && appBundle.getParentDirectory() != appBundle) {
      appBundle = appBundle.getParentDirectory();
    }
    const auto hostName = appBundle.hasFileExtension("app")
        ? appBundle.getFileNameWithoutExtension()
        : hostExecutable.getFileNameWithoutExtension();
    setenv("OPENUTAU_VST_HOST_NAME", hostName.toRawUTF8(), 1);

    auto directory = juce::File(juce::SystemStats::getEnvironmentVariable(
        "OPENUTAU_VST_EMBEDDED_EDITOR_DIR", {}));
    if (directory == juce::File{}) {
      directory = currentPluginModule().getParentDirectory().getParentDirectory()
          .getChildFile("Resources").getChildFile("EmbeddedEditor");
    }
    const auto dotnetDirectory = directory.getChildFile("dotnet");
    const auto hostfxr = dotnetDirectory.getChildFile("host").getChildFile("fxr")
        .getChildFile("8.0.30").getChildFile("libhostfxr.dylib");
    const auto appDirectory = directory.getChildFile("app");
    const auto runtimeConfig = appDirectory.getChildFile(
        "OpenUtau.Vst.EditorHost.runtimeconfig.json");
    const auto assembly = appDirectory.getChildFile("OpenUtau.Vst.EditorHost.dll");
    if (!hostfxr.existsAsFile() || !runtimeConfig.existsAsFile()
        || !assembly.existsAsFile()) {
      error = "Embedded editor payload is missing from " + directory.getFullPathName();
      return false;
    }

    library = dlopen(hostfxr.getFullPathName().toRawUTF8(), RTLD_LAZY | RTLD_LOCAL);
    if (library == nullptr) {
      error = "Unable to load hostfxr: " + juce::String(dlerror());
      return false;
    }
    const auto initialize = reinterpret_cast<init_runtime_fn>(
        dlsym(library, "hostfxr_initialize_for_runtime_config"));
    const auto getDelegate = reinterpret_cast<get_delegate_fn>(
        dlsym(library, "hostfxr_get_runtime_delegate"));
    const auto close = reinterpret_cast<close_runtime_fn>(
        dlsym(library, "hostfxr_close"));
    if (initialize == nullptr || getDelegate == nullptr || close == nullptr) {
      error = "The packaged hostfxr API is incomplete.";
      return false;
    }

    hostfxr_handle context{};
    const auto configPath = runtimeConfig.getFullPathName().toStdString();
    const auto hostPath = currentPluginModule().getFullPathName().toStdString();
    const auto dotnetRoot = dotnetDirectory.getFullPathName().toStdString();
    const hostfxr_initialize_parameters parameters{
        sizeof(hostfxr_initialize_parameters), hostPath.c_str(), dotnetRoot.c_str()};
    const auto initializeResult = initialize(configPath.c_str(), &parameters, &context);
    if (initializeResult < 0 || context == nullptr) {
      error = "Unable to initialize the packaged .NET runtime (hostfxr "
          + juce::String::toHexString(initializeResult) + ").";
      return false;
    }
    void* rawLoader{};
    const auto delegateResult = getDelegate(
        context, loadAssemblyAndGetFunctionPointerDelegate, &rawLoader);
    close(context);
    if (delegateResult != 0 || rawLoader == nullptr) {
      error = "Unable to acquire the managed component loader.";
      return false;
    }

    const auto loadAssembly = reinterpret_cast<load_assembly_fn>(rawLoader);
    const auto assemblyPath = assembly.getFullPathName().toStdString();
    constexpr auto typeName =
        "OpenUtau.Vst.EditorHost.EntryPoints, OpenUtau.Vst.EditorHost";
    void* rawCreate{};
    void* rawDestroy{};
    void* rawGetError{};
    void* rawGetRevision{};
    void* rawExecuteUndo{};
    void* rawExecuteDelete{};
    void* rawCopyState{};
    void* rawSetState{};
    void* rawSetHostTransport{};
    void* rawGetPreviewState{};
    void* rawGetPreviewRevision{};
    void* rawGetPreviewTone{};
    void* rawCopyPreview{};
    const auto createResult = loadAssembly(
        assemblyPath.c_str(), typeName, "Create", unmanagedCallersOnly, nullptr,
        &rawCreate);
    const auto destroyResult = loadAssembly(
        assemblyPath.c_str(), typeName, "Destroy", unmanagedCallersOnly, nullptr,
        &rawDestroy);
    const auto errorResult = loadAssembly(
        assemblyPath.c_str(), typeName, "GetLastError", unmanagedCallersOnly, nullptr,
        &rawGetError);
    const auto revisionResult = loadAssembly(
        assemblyPath.c_str(), typeName, "GetRevision", unmanagedCallersOnly, nullptr,
        &rawGetRevision);
    const auto undoResult = loadAssembly(
        assemblyPath.c_str(), typeName, "ExecuteUndo", unmanagedCallersOnly, nullptr,
        &rawExecuteUndo);
    const auto deleteResult = loadAssembly(
        assemblyPath.c_str(), typeName, "ExecuteDelete", unmanagedCallersOnly,
        nullptr, &rawExecuteDelete);
    const auto copyResult = loadAssembly(
        assemblyPath.c_str(), typeName, "CopyState", unmanagedCallersOnly, nullptr,
        &rawCopyState);
    const auto setResult = loadAssembly(
        assemblyPath.c_str(), typeName, "SetState", unmanagedCallersOnly, nullptr,
        &rawSetState);
    const auto transportResult = loadAssembly(
        assemblyPath.c_str(), typeName, "SetHostTransport", unmanagedCallersOnly,
        nullptr, &rawSetHostTransport);
    const auto previewStateResult = loadAssembly(
        assemblyPath.c_str(), typeName, "GetPreviewState", unmanagedCallersOnly,
        nullptr, &rawGetPreviewState);
    const auto previewRevisionResult = loadAssembly(
        assemblyPath.c_str(), typeName, "GetPreviewRevision", unmanagedCallersOnly,
        nullptr, &rawGetPreviewRevision);
    const auto previewToneResult = loadAssembly(
        assemblyPath.c_str(), typeName, "GetPreviewTone", unmanagedCallersOnly,
        nullptr, &rawGetPreviewTone);
    const auto previewCopyResult = loadAssembly(
        assemblyPath.c_str(), typeName, "CopyPreview", unmanagedCallersOnly,
        nullptr, &rawCopyPreview);
    if (createResult != 0 || destroyResult != 0 || rawCreate == nullptr
        || errorResult != 0 || revisionResult != 0 || undoResult != 0
        || deleteResult != 0
        || copyResult != 0
        || setResult != 0 || transportResult != 0
        || previewStateResult != 0 || previewRevisionResult != 0
        || previewToneResult != 0
        || previewCopyResult != 0
        || rawDestroy == nullptr
        || rawGetError == nullptr
        || rawGetRevision == nullptr || rawExecuteUndo == nullptr
        || rawExecuteDelete == nullptr
        || rawCopyState == nullptr
        || rawSetState == nullptr || rawSetHostTransport == nullptr
        || rawGetPreviewState == nullptr || rawGetPreviewRevision == nullptr
        || rawGetPreviewTone == nullptr
        || rawCopyPreview == nullptr) {
      error = "Unable to bind the managed editor entry points.";
      return false;
    }
    create = reinterpret_cast<create_editor_fn>(rawCreate);
    destroy = reinterpret_cast<destroy_editor_fn>(rawDestroy);
    getError = reinterpret_cast<get_error_fn>(rawGetError);
    getRevision = reinterpret_cast<get_revision_fn>(rawGetRevision);
    executeUndo = reinterpret_cast<execute_undo_fn>(rawExecuteUndo);
    executeDelete = reinterpret_cast<execute_delete_fn>(rawExecuteDelete);
    copyState = reinterpret_cast<copy_state_fn>(rawCopyState);
    setState = reinterpret_cast<set_state_fn>(rawSetState);
    setHostTransport = reinterpret_cast<set_host_transport_fn>(rawSetHostTransport);
    getPreviewState = reinterpret_cast<get_preview_state_fn>(rawGetPreviewState);
    getPreviewRevision = reinterpret_cast<get_preview_revision_fn>(
        rawGetPreviewRevision);
    getPreviewTone = reinterpret_cast<get_preview_tone_fn>(rawGetPreviewTone);
    copyPreview = reinterpret_cast<copy_preview_fn>(rawCopyPreview);
    return true;
  }
};

Runtime& runtime() {
  static Runtime instance;
  return instance;
}

bool executeManagedUndo(const bool redo) {
  auto& shared = runtime();
  return shared.executeUndo != nullptr && shared.executeUndo(redo ? 1 : 0) == 0;
}

bool executeManagedDelete(const bool forwardDelete) {
  auto& shared = runtime();
  return shared.executeDelete != nullptr
      && shared.executeDelete(forwardDelete ? 1 : 0) == 0;
}

} // namespace

namespace openutau::vst {

ManagedEditorHost::~ManagedEditorHost() { destroy(); }

void* ManagedEditorHost::create(
    const int width, const int height, const std::uint64_t instanceId) {
  destroy();
  auto& shared = runtime();
  if (!shared.initialize()) {
    lastError_ = shared.error;
    return nullptr;
  }
  // OpenUtau's desktop editor model is intentionally singleton-based. The
  // VST's renderer is isolated per processor, but creating two managed editor
  // roots in the same DAW process would make both views address the same
  // DocManager and silently exchange project state. Refuse the second view
  // explicitly instead. Hosts normally keep one channel editor visible; once
  // it closes, another instance can acquire this lease immediately.
  if (!shared.claimEditor(this)) {
    lastError_ = "Another OpenUtau editor is already open. Close it before "
                 "opening this instance; both instances continue rendering independently.";
    return nullptr;
  }
  nativeView_ = shared.create(
      width, height, static_cast<std::int64_t>(instanceId));
  if (nativeView_ == nullptr) {
    const auto* managedError = shared.getError != nullptr ? shared.getError() : nullptr;
    lastError_ = managedError != nullptr
        ? juce::String::fromUTF8(managedError)
        : "Avalonia did not create an embedded NSView.";
    shared.releaseEditor(this);
  } else {
    installMacSpaceKeyForwarder(
        nativeView_, nativeViewOriginalClass_, executeManagedUndo,
        executeManagedDelete);
    lastError_.clear();
  }
  return nativeView_;
}

void ManagedEditorHost::destroy() {
  auto& shared = runtime();
  if (nativeView_ != nullptr) {
    uninstallMacSpaceKeyForwarder(nativeView_, nativeViewOriginalClass_);
    if (shared.destroy != nullptr) shared.destroy(nativeView_);
    nativeView_ = nullptr;
  }
  shared.releaseEditor(this);
}

bool ManagedEditorHost::setProjectState(const void* data, const std::size_t size) {
  if (nativeView_ == nullptr || data == nullptr || size == 0
      || size > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) return false;
  auto& shared = runtime();
  if (shared.setState(reinterpret_cast<const std::uint8_t*>(data),
                      static_cast<int32_t>(size)) != 0) return false;
  lastRevision_ = shared.getRevision();
  return true;
}

bool ManagedEditorHost::pullProjectState(juce::MemoryBlock& destination) {
  if (nativeView_ == nullptr) return false;
  auto& shared = runtime();
  const auto revision = shared.getRevision();
  if (revision == lastRevision_) return false;
  const auto required = shared.copyState(nullptr, 0);
  if (required <= 0 || required > 256 * 1024 * 1024) return false;
  destination.setSize(static_cast<std::size_t>(required), false);
  const auto copied = shared.copyState(
      static_cast<std::uint8_t*>(destination.getData()), required);
  if (copied != required) {
    destination.reset();
    return false;
  }
  lastRevision_ = revision;
  return true;
}

bool ManagedEditorHost::setHostTransport(
    const std::int64_t projectSample, const double sampleRate,
    const double quarterNotePosition, const double bpm,
    const int timeSignatureNumerator, const int timeSignatureDenominator,
    const bool playing) {
  if (nativeView_ == nullptr || runtime().setHostTransport == nullptr) return false;
  return runtime().setHostTransport(
      projectSample, sampleRate, quarterNotePosition, bpm,
      timeSignatureNumerator, timeSignatureDenominator,
      playing ? 1 : 0) == 0;
}

bool ManagedEditorHost::pullPreview(
    float* const interleaved, const std::size_t capacityFrames,
    std::size_t& copiedFrames, bool& active, std::uint64_t& revision) {
  copiedFrames = 0;
  active = false;
  revision = 0;
  if (nativeView_ == nullptr || runtime().getPreviewState == nullptr
      || runtime().getPreviewRevision == nullptr
      || runtime().copyPreview == nullptr) return false;
  auto& shared = runtime();
  active = shared.getPreviewState() != 0;
  revision = static_cast<std::uint64_t>(shared.getPreviewRevision());
  if (!active || interleaved == nullptr || capacityFrames == 0) return true;
  const auto bounded = std::min<std::size_t>(
      capacityFrames, static_cast<std::size_t>(std::numeric_limits<int32_t>::max()));
  const auto copied = shared.copyPreview(
      interleaved, static_cast<int32_t>(bounded));
  if (copied < -1 || copied > static_cast<int32_t>(bounded)) return false;
  if (copied >= 0) copiedFrames = static_cast<std::size_t>(copied);
  active = shared.getPreviewState() != 0 || copiedFrames > 0;
  return true;
}

int ManagedEditorHost::previewToneState(
    double& frequency, std::uint64_t& revision) {
  frequency = 0.0;
  revision = 0;
  if (nativeView_ == nullptr || runtime().getPreviewTone == nullptr) return -1;
  int64_t managedRevision = 0;
  const auto state = runtime().getPreviewTone(&frequency, &managedRevision);
  revision = static_cast<std::uint64_t>(managedRevision);
  return state >= -1 && state <= 1 ? state : -1;
}

bool ManagedEditorHost::focus() {
  return focusMacEditor(nativeView_, nativeViewOriginalClass_);
}

} // namespace openutau::vst
