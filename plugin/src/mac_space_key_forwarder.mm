#include "openutau_vst/mac_space_key_forwarder.hpp"

#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace {

bool isHostTransportSpace(NSEvent* const event) {
  const auto relevant = event.modifierFlags
      & (NSEventModifierFlagCommand
         | NSEventModifierFlagControl
         | NSEventModifierFlagOption
         | NSEventModifierFlagShift);
  // FL Studio uses Space for stop/rewind and Command+Space on macOS for
  // pause/resume. Accept Control+Space as well because FL also documents that
  // spelling and existing shortcut profiles may retain it.
  return event.keyCode == 49
      && (relevant == 0
          || relevant == NSEventModifierFlagControl
          || relevant == NSEventModifierFlagCommand);
}

void callOriginalKeyMethod(
    id self, const SEL selector, NSEvent* const event) {
  Class const currentClass = object_getClass(self);
  Class const nullableSuperclass = currentClass == Nil
      ? Nil : class_getSuperclass(currentClass);
  if (nullableSuperclass == Nil) return;
  Class _Nonnull const superclass = nullableSuperclass;
  struct objc_super superclassCall {
      self, superclass};
  using message_fn = void (*)(struct objc_super*, SEL, NSEvent*);
  reinterpret_cast<message_fn>(objc_msgSendSuper)(
      &superclassCall, selector, event);
}

BOOL callOriginalKeyEquivalent(
    id self, const SEL selector, NSEvent* const event) {
  Class const currentClass = object_getClass(self);
  Class const nullableSuperclass = currentClass == Nil
      ? Nil : class_getSuperclass(currentClass);
  if (nullableSuperclass == Nil) return NO;
  Class _Nonnull const superclass = nullableSuperclass;
  struct objc_super superclassCall {
      self, superclass};
  using message_fn = BOOL (*)(struct objc_super*, SEL, NSEvent*);
  return reinterpret_cast<message_fn>(objc_msgSendSuper)(
      &superclassCall, selector, event);
}

void forwardKeyDown(id self, const SEL selector, NSEvent* const event) {
  if (!isHostTransportSpace(event)) {
    callOriginalKeyMethod(self, selector, event);
    return;
  }
  // Do not let key-repeat toggle the DAW transport back and forth.
  if (event.isARepeat) return;
  if (NSResponder* const responder = [self nextResponder]) {
    [responder keyDown:event];
  }
}

void forwardKeyUp(id self, const SEL selector, NSEvent* const event) {
  if (!isHostTransportSpace(event)) {
    callOriginalKeyMethod(self, selector, event);
    return;
  }
  if (NSResponder* const responder = [self nextResponder]) {
    [responder keyUp:event];
  }
}

BOOL forwardKeyEquivalent(
    id self, const SEL selector, NSEvent* const event) {
  // Modified Space can enter Cocoa's key-equivalent path before keyDown:.
  // Route FL's pause shortcut through the same local responder handoff as
  // plain Space, and claim it so Avalonia cannot swallow it.
  if (isHostTransportSpace(event)) {
    if (event.isARepeat) return YES;
    if (NSResponder* const responder = [self nextResponder]) {
      [responder keyDown:event];
      return YES;
    }
    return NO;
  }

  // Cocoa normally resolves Command-key equivalents through the host window's
  // menu before keyDown: reaches the embedded Avalonia view. Avalonia's macOS
  // backend also consumes an injected Command event as a native key equivalent
  // without routing it to the managed KeyDown handlers. Remap Command to the
  // VST-only Control shortcut path before delivering keyDown:, then claim the
  // event so FL Studio cannot execute the same shortcut.
  const auto shortcutModifiers = event.modifierFlags
      & (NSEventModifierFlagCommand | NSEventModifierFlagControl);
  if (shortcutModifiers == 0) {
    return callOriginalKeyEquivalent(self, selector, event);
  }

  NSEvent* editorEvent = event;
  if ((shortcutModifiers & NSEventModifierFlagCommand) != 0) {
    const auto editorModifiers =
        (event.modifierFlags & ~NSEventModifierFlagCommand)
        | NSEventModifierFlagControl;
    NSString* _Nonnull const characters = event.characters != nil
        ? event.characters : @"";
    NSString* _Nonnull const charactersIgnoringModifiers =
        event.charactersIgnoringModifiers != nil
        ? event.charactersIgnoringModifiers : @"";
    editorEvent = [NSEvent keyEventWithType:event.type
        location:event.locationInWindow
        modifierFlags:editorModifiers
        timestamp:event.timestamp
        windowNumber:event.windowNumber
        context:nil
        characters:characters
        charactersIgnoringModifiers:charactersIgnoringModifiers
        isARepeat:event.isARepeat
        keyCode:event.keyCode];
  }

  // Command-key dispatch temporarily puts focus back on the host window. Put
  // the embedded Avalonia view back on the first-responder path before routing
  // the translated event through its original keyDown: implementation.
  if (NSWindow* const window = [self window]) {
    [window makeFirstResponder:self];
    [window sendEvent:editorEvent];
  }
  return YES;
}

Class forwardingSubclass(Class const originalClass) {
  static std::mutex mutex;
  static std::unordered_map<Class, Class> subclasses;
  const std::scoped_lock lock(mutex);
  if (const auto found = subclasses.find(originalClass);
      found != subclasses.end()) {
    return found->second;
  }

  char name[96]{};
  std::snprintf(name, sizeof(name), "OpenUtauSpaceForwarder_%llx",
      static_cast<unsigned long long>(
          reinterpret_cast<std::uintptr_t>(originalClass)));
  Class subclass = objc_getClass(name);
  if (subclass == Nil) {
    subclass = objc_allocateClassPair(originalClass, name, 0);
    if (subclass == Nil
        || !class_addMethod(subclass, @selector(keyDown:),
              reinterpret_cast<IMP>(forwardKeyDown), "v@:@")
        || !class_addMethod(subclass, @selector(keyUp:),
              reinterpret_cast<IMP>(forwardKeyUp), "v@:@")
        || !class_addMethod(subclass, @selector(performKeyEquivalent:),
              reinterpret_cast<IMP>(forwardKeyEquivalent), "B@:@")) {
      if (subclass != Nil) objc_disposeClassPair(subclass);
      return Nil;
    }
    objc_registerClassPair(subclass);
  }
  subclasses.emplace(originalClass, subclass);
  return subclass;
}

} // namespace

namespace openutau::vst {

bool installMacSpaceKeyForwarder(
    void* const nativeView, void*& originalClass) {
  originalClass = nullptr;
  if (nativeView == nullptr) return false;
  id const view = static_cast<id>(nativeView);
  Class const base = object_getClass(view);
  Class const subclass = forwardingSubclass(base);
  if (subclass == Nil) return false;
  originalClass = static_cast<void*>(base);
  object_setClass(view, subclass);
  return true;
}

void uninstallMacSpaceKeyForwarder(
    void* const nativeView, void*& originalClass) {
  if (nativeView != nullptr && originalClass != nullptr) {
    object_setClass(static_cast<id>(nativeView),
        static_cast<Class>(originalClass));
  }
  originalClass = nullptr;
}

} // namespace openutau::vst
