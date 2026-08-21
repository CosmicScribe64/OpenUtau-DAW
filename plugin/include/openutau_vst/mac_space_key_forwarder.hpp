#pragma once

namespace openutau::vst {

using MacEditorUndoHandler = bool (*)(bool redo);
using MacEditorDeleteHandler = bool (*)(bool forwardDelete);

// Installs an instance-local Objective-C subclass on the embedded Avalonia
// NSView. FL transport Space variants are redirected to the enclosing
// JUCE/host responder. Other Cocoa Command-key equivalents are retained by
// OpenUtau instead of being resolved by the host application's menu.
bool installMacSpaceKeyForwarder(void* nativeView, void*& originalClass,
                                 MacEditorUndoHandler undoHandler,
                                 MacEditorDeleteHandler deleteHandler);
void uninstallMacSpaceKeyForwarder(void* nativeView, void*& originalClass);
bool focusMacEditor(void* nativeView, void* installation);

} // namespace openutau::vst
