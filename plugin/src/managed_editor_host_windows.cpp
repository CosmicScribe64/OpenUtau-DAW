#include "openutau_vst/managed_editor_host.hpp"

#include <juce_gui_basics/juce_gui_basics.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>

namespace {

using hostfxr_handle = void*;
using init_runtime_fn = int32_t(__cdecl *)(const wchar_t*, const void*, hostfxr_handle*);
using get_delegate_fn = int32_t(__cdecl *)(hostfxr_handle, int32_t, void**);
using close_runtime_fn = int32_t(__cdecl *)(hostfxr_handle);
using load_assembly_fn = int32_t(__cdecl *)(const wchar_t*, const wchar_t*,
                                            const wchar_t*, const wchar_t*,
                                            void*, void**);
using create_editor_fn = void* (__cdecl *)(int32_t, int32_t);
using destroy_editor_fn = void(__cdecl *)(void*);
using get_error_fn = const char* (__cdecl *)();
using get_revision_fn = int64_t(__cdecl *)();
using copy_state_fn = int32_t(__cdecl *)(std::uint8_t*, int32_t);
using set_state_fn = int32_t(__cdecl *)(const std::uint8_t*, int32_t);
using set_host_transport_fn = int32_t(__cdecl *)(
    int64_t, double, double, double, int32_t, int32_t, int32_t);
using get_preview_state_fn = int32_t(__cdecl *)();
using copy_preview_fn = int32_t(__cdecl *)(float*, int32_t);

struct hostfxr_initialize_parameters {
  std::size_t size;
  const wchar_t* hostPath;
  const wchar_t* dotnetRoot;
};

constexpr int32_t loadAssemblyAndGetFunctionPointerDelegate = 5;
const wchar_t* const unmanagedCallersOnly = reinterpret_cast<const wchar_t*>(-1);

void moduleAnchor() {}

juce::File currentPluginModule() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&moduleAnchor), &module) == 0) {
    return {};
  }
  std::array<wchar_t, 32768> path{};
  const auto length = GetModuleFileNameW(
      module, path.data(), static_cast<DWORD>(path.size()));
  return length > 0 ? juce::File(juce::String(path.data())) : juce::File{};
}

juce::File findHostFxr(const juce::File& dotnetDirectory) {
  const auto fxrDirectory = dotnetDirectory.getChildFile("host").getChildFile("fxr");
  for (const auto& version : fxrDirectory.findChildFiles(
           juce::File::findDirectories, false)) {
    const auto candidate = version.getChildFile("hostfxr.dll");
    if (candidate.existsAsFile()) return candidate;
  }
  return {};
}

struct Runtime final {
  std::mutex mutex;
  HMODULE library{};
  void* editorOwner{};
  create_editor_fn create{};
  destroy_editor_fn destroy{};
  get_error_fn getError{};
  get_revision_fn getRevision{};
  copy_state_fn copyState{};
  set_state_fn setState{};
  set_host_transport_fn setHostTransport{};
  get_preview_state_fn getPreviewState{};
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
    SetEnvironmentVariableW(
        L"OPENUTAU_VST_HOST_NAME",
        hostExecutable.getFileNameWithoutExtension().toWideCharPointer());

    auto directory = juce::File(juce::SystemStats::getEnvironmentVariable(
        "OPENUTAU_VST_EMBEDDED_EDITOR_DIR", {}));
    if (directory == juce::File{}) {
      directory = currentPluginModule().getParentDirectory().getParentDirectory()
          .getChildFile("Resources").getChildFile("EmbeddedEditor");
    }
    const auto dotnetDirectory = directory.getChildFile("dotnet");
    const auto hostfxr = findHostFxr(dotnetDirectory);
    const auto appDirectory = directory.getChildFile("app");
    const auto runtimeConfig = appDirectory.getChildFile(
        "OpenUtau.Vst.EditorHost.runtimeconfig.json");
    const auto assembly = appDirectory.getChildFile("OpenUtau.Vst.EditorHost.dll");
    if (!hostfxr.existsAsFile() || !runtimeConfig.existsAsFile()
        || !assembly.existsAsFile()) {
      error = "Embedded editor payload is missing from " + directory.getFullPathName();
      return false;
    }

    library = LoadLibraryW(hostfxr.getFullPathName().toWideCharPointer());
    if (library == nullptr) {
      error = "Unable to load packaged hostfxr (Windows error "
          + juce::String(static_cast<int>(GetLastError())) + ").";
      return false;
    }
    const auto initialize = reinterpret_cast<init_runtime_fn>(
        GetProcAddress(library, "hostfxr_initialize_for_runtime_config"));
    const auto getDelegate = reinterpret_cast<get_delegate_fn>(
        GetProcAddress(library, "hostfxr_get_runtime_delegate"));
    const auto close = reinterpret_cast<close_runtime_fn>(
        GetProcAddress(library, "hostfxr_close"));
    if (initialize == nullptr || getDelegate == nullptr || close == nullptr) {
      error = "The packaged hostfxr API is incomplete.";
      return false;
    }

    hostfxr_handle context{};
    const auto configPath = runtimeConfig.getFullPathName();
    const auto hostPath = currentPluginModule().getFullPathName();
    const auto dotnetRoot = dotnetDirectory.getFullPathName();
    const hostfxr_initialize_parameters parameters{
        sizeof(hostfxr_initialize_parameters), hostPath.toWideCharPointer(),
        dotnetRoot.toWideCharPointer()};
    const auto initializeResult = initialize(
        configPath.toWideCharPointer(), &parameters, &context);
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
    const auto assemblyPath = assembly.getFullPathName();
    constexpr auto typeName =
        L"OpenUtau.Vst.EditorHost.EntryPoints, OpenUtau.Vst.EditorHost";
    void* rawCreate{};
    void* rawDestroy{};
    void* rawGetError{};
    void* rawGetRevision{};
    void* rawCopyState{};
    void* rawSetState{};
    void* rawSetHostTransport{};
    void* rawGetPreviewState{};
    void* rawCopyPreview{};
    const auto createResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"Create",
        unmanagedCallersOnly, nullptr, &rawCreate);
    const auto destroyResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"Destroy",
        unmanagedCallersOnly, nullptr, &rawDestroy);
    const auto errorResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"GetLastError",
        unmanagedCallersOnly, nullptr, &rawGetError);
    const auto revisionResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"GetRevision",
        unmanagedCallersOnly, nullptr, &rawGetRevision);
    const auto copyResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"CopyState",
        unmanagedCallersOnly, nullptr, &rawCopyState);
    const auto setResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"SetState",
        unmanagedCallersOnly, nullptr, &rawSetState);
    const auto transportResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"SetHostTransport",
        unmanagedCallersOnly, nullptr, &rawSetHostTransport);
    const auto previewStateResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"GetPreviewState",
        unmanagedCallersOnly, nullptr, &rawGetPreviewState);
    const auto previewCopyResult = loadAssembly(
        assemblyPath.toWideCharPointer(), typeName, L"CopyPreview",
        unmanagedCallersOnly, nullptr, &rawCopyPreview);
    if (createResult != 0 || destroyResult != 0 || errorResult != 0
        || revisionResult != 0 || copyResult != 0 || setResult != 0
        || transportResult != 0 || previewStateResult != 0
        || previewCopyResult != 0 || rawCreate == nullptr
        || rawDestroy == nullptr || rawGetError == nullptr
        || rawGetRevision == nullptr || rawCopyState == nullptr
        || rawSetState == nullptr || rawSetHostTransport == nullptr
        || rawGetPreviewState == nullptr || rawCopyPreview == nullptr) {
      error = "Unable to bind the managed editor entry points.";
      return false;
    }
    create = reinterpret_cast<create_editor_fn>(rawCreate);
    destroy = reinterpret_cast<destroy_editor_fn>(rawDestroy);
    getError = reinterpret_cast<get_error_fn>(rawGetError);
    getRevision = reinterpret_cast<get_revision_fn>(rawGetRevision);
    copyState = reinterpret_cast<copy_state_fn>(rawCopyState);
    setState = reinterpret_cast<set_state_fn>(rawSetState);
    setHostTransport = reinterpret_cast<set_host_transport_fn>(rawSetHostTransport);
    getPreviewState = reinterpret_cast<get_preview_state_fn>(rawGetPreviewState);
    copyPreview = reinterpret_cast<copy_preview_fn>(rawCopyPreview);
    return true;
  }
};

Runtime& runtime() {
  static Runtime instance;
  return instance;
}

} // namespace

namespace openutau::vst {

ManagedEditorHost::~ManagedEditorHost() { destroy(); }

void* ManagedEditorHost::create(const int width, const int height) {
  destroy();
  auto& shared = runtime();
  if (!shared.initialize()) {
    lastError_ = shared.error;
    return nullptr;
  }
  if (!shared.claimEditor(this)) {
    lastError_ = "Another OpenUtau editor is already open. Close it before "
                 "opening this instance; both instances continue rendering independently.";
    return nullptr;
  }
  nativeView_ = shared.create(width, height);
  if (nativeView_ == nullptr) {
    const auto* managedError = shared.getError != nullptr ? shared.getError() : nullptr;
    lastError_ = managedError != nullptr
        ? juce::String::fromUTF8(managedError)
        : "Avalonia did not create an embedded HWND.";
    shared.releaseEditor(this);
  } else {
    lastError_.clear();
  }
  return nativeView_;
}

void ManagedEditorHost::destroy() {
  auto& shared = runtime();
  if (nativeView_ != nullptr) {
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
    std::size_t& copiedFrames, bool& active) {
  copiedFrames = 0;
  active = false;
  if (nativeView_ == nullptr || runtime().getPreviewState == nullptr
      || runtime().copyPreview == nullptr) return false;
  auto& shared = runtime();
  active = shared.getPreviewState() != 0;
  if (!active || interleaved == nullptr || capacityFrames == 0) return true;
  const auto bounded = std::min<std::size_t>(
      capacityFrames, static_cast<std::size_t>(std::numeric_limits<int32_t>::max()));
  const auto copied = shared.copyPreview(interleaved, static_cast<int32_t>(bounded));
  if (copied < -1 || copied > static_cast<int32_t>(bounded)) return false;
  if (copied >= 0) copiedFrames = static_cast<std::size_t>(copied);
  active = shared.getPreviewState() != 0 || copiedFrames > 0;
  return true;
}

} // namespace openutau::vst
