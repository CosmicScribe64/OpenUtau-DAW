#pragma once

namespace openutau::vst {

// Installs an instance-local Objective-C subclass on the embedded Avalonia
// NSView. FL transport Space variants are redirected to the enclosing
// JUCE/host responder. Other Cocoa Command-key equivalents are retained by
// OpenUtau instead of being resolved by the host application's menu.
bool installMacSpaceKeyForwarder(void* nativeView, void*& originalClass);
void uninstallMacSpaceKeyForwarder(void* nativeView, void*& originalClass);

} // namespace openutau::vst
