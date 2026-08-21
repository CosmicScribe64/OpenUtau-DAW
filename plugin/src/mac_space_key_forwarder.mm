#include "openutau_vst/mac_space_key_forwarder.hpp"

#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <cstdint>
#include <cstdio>
#include <memory>
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

bool isHostWindowShortcut(NSEvent* const event) {
  const auto relevant = event.modifierFlags
      & (NSEventModifierFlagCommand
         | NSEventModifierFlagControl
         | NSEventModifierFlagOption
         | NSEventModifierFlagShift);
  // macOS virtual key 111 is F12, FL Studio's "close all plug-in windows"
  // shortcut. It remains a host command while the embedded editor is open.
  return relevant == 0 && event.keyCode == 111;
}

bool isHostShortcut(NSEvent* const event) {
  return isHostTransportSpace(event) || isHostWindowShortcut(event);
}

bool isEditorCommandEquivalent(NSEvent* const event) {
  const auto modifiers = event.modifierFlags;
  if ((modifiers & (NSEventModifierFlagCommand
                    | NSEventModifierFlagControl)) == 0
      || (modifiers & NSEventModifierFlagOption) != 0) {
    return false;
  }
  NSString* const characters =
      [event.charactersIgnoringModifiers lowercaseString];
  if (characters.length != 1) return false;
  switch ([characters characterAtIndex:0]) {
    case 'a':
    case 'c':
    case 'n':
    case 'o':
    case 's':
    case 'v':
    case 'x':
    case 'y':
    case 'z':
      return true;
    default:
      return false;
  }
}

bool isEditorDelete(NSEvent* const event) {
  const auto relevant = event.modifierFlags
      & (NSEventModifierFlagCommand
         | NSEventModifierFlagControl
         | NSEventModifierFlagOption
         | NSEventModifierFlagShift);
  return relevant == 0 && (event.keyCode == 51 || event.keyCode == 117);
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

BOOL acceptsEditorFirstResponder(id, SEL) {
  return YES;
}

NSEvent* editorShortcutEvent(NSEvent* const event) {
  if ((event.modifierFlags & NSEventModifierFlagCommand) == 0) return event;
  const auto editorModifiers =
      (event.modifierFlags & ~NSEventModifierFlagCommand)
      | NSEventModifierFlagControl;
  NSString* _Nonnull const characters = event.characters != nil
      ? event.characters : @"";
  NSString* _Nonnull const charactersIgnoringModifiers =
      event.charactersIgnoringModifiers != nil
      ? event.charactersIgnoringModifiers : @"";
  return [NSEvent keyEventWithType:event.type
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

bool dispatchEditorShortcut(
    NSView* const view, NSEvent* const event) {
  NSWindow* const window = view.window;
  if (window == nil) return false;
  [window makeFirstResponder:view];
  [view keyDown:editorShortcutEvent(event)];
  return true;
}

bool pointerEventBelongsToEditor(
    NSView* const view, NSEvent* const event) {
  NSWindow* const window = view.window;
  if (window == nil || event.window != window
      || view.isHiddenOrHasHiddenAncestor) {
    return false;
  }
  const NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
  return [view hitTest:point] != nil;
}

void forwardKeyDown(id self, const SEL selector, NSEvent* const event) {
  if (!isHostShortcut(event)) {
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
  if (!isHostShortcut(event)) {
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
  if (isHostShortcut(event)) {
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
  if (shortcutModifiers == 0 || !isEditorCommandEquivalent(event)) {
    return callOriginalKeyEquivalent(self, selector, event);
  }

  // Command-key dispatch temporarily puts focus back on the host window. Put
  // the embedded Avalonia view back on the first-responder path before routing
  // the translated event through its original keyDown: implementation.
  return dispatchEditorShortcut(static_cast<NSView*>(self), event) ? YES : NO;
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
        || !class_addMethod(subclass, @selector(acceptsFirstResponder),
              reinterpret_cast<IMP>(acceptsEditorFirstResponder), "B@:")
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

struct ForwarderInstallation final {
  NSView* view{};
  Class originalClass{};
  NSWindow* mouseWindow{};
  BOOL originalAcceptsMouseMovedEvents{};
  id eventMonitor{};
  openutau::vst::MacEditorUndoHandler undoHandler{};
  openutau::vst::MacEditorDeleteHandler deleteHandler{};
};

bool installOnEditorView(
    NSView* const view, ForwarderInstallation& installation) {
  const auto originalClass = object_getClass(view);
  const auto subclass = forwardingSubclass(originalClass);
  if (subclass == Nil) return false;
  object_setClass(view, subclass);
  installation.view = view;
  installation.originalClass = originalClass;
  return true;
}

void uninstallEditorView(ForwarderInstallation& installation) {
  if (installation.eventMonitor != nil) {
    [NSEvent removeMonitor:installation.eventMonitor];
    installation.eventMonitor = nil;
  }
  if (installation.mouseWindow != nil) {
    installation.mouseWindow.acceptsMouseMovedEvents =
        installation.originalAcceptsMouseMovedEvents;
    installation.mouseWindow = nil;
  }
  if (installation.view != nil && installation.originalClass != Nil) {
    object_setClass(installation.view, installation.originalClass);
  }
  installation.view = nil;
  installation.originalClass = Nil;
}

} // namespace

namespace openutau::vst {

bool installMacSpaceKeyForwarder(
    void* const nativeView, void*& originalClass,
    const MacEditorUndoHandler undoHandler,
    const MacEditorDeleteHandler deleteHandler) {
  originalClass = nullptr;
  if (nativeView == nullptr) return false;
  auto installation = std::make_unique<ForwarderInstallation>();
  if (!installOnEditorView(
          static_cast<NSView*>(nativeView), *installation)) {
    uninstallEditorView(*installation);
    return false;
  }
  NSView* const editorView = static_cast<NSView*>(nativeView);
  installation->undoHandler = undoHandler;
  installation->deleteHandler = deleteHandler;
  ForwarderInstallation* const installed = installation.get();
  installation->eventMonitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:(
          NSEventMaskKeyDown | NSEventMaskMouseMoved | NSEventMaskCursorUpdate)
      handler:^NSEvent* _Nullable(NSEvent* _Nonnull event) {
        if (event.type == NSEventTypeMouseMoved
            || event.type == NSEventTypeCursorUpdate) {
          if (!pointerEventBelongsToEditor(editorView, event)) return event;
          if (event.type == NSEventTypeCursorUpdate) {
            [editorView cursorUpdate:event];
          } else {
            [editorView mouseMoved:event];
          }
          return nil;
        }
        NSWindow* const window = editorView.window;
        const auto commandEquivalent = isEditorCommandEquivalent(event);
        const auto deleteKey = isEditorDelete(event);
        if (window == nil || event.window != window
            || editorView.isHiddenOrHasHiddenAncestor) {
          return event;
        }
        // Space belongs to the DAW transport. Unrecognised Command shortcuts
        // likewise remain available to FL Studio (for example Command+Q).
        if (isHostShortcut(event)
            || ((event.modifierFlags & NSEventModifierFlagCommand) != 0
                && !commandEquivalent)) {
          return event;
        }
        if (deleteKey) {
          if (installed->deleteHandler != nullptr
              && installed->deleteHandler(event.keyCode == 117)) {
            return nil;
          }
          // A managed text control owns Backspace/Delete while it has focus.
          // Deliver the original event to Avalonia instead of returning it to
          // FL Studio, whose wrapper consumes it before the text box sees it.
          dispatchEditorShortcut(editorView, event);
          return nil;
        }
        NSString* const characters =
            [event.charactersIgnoringModifiers lowercaseString];
        if (characters.length == 1) {
          const auto key = [characters characterAtIndex:0];
          const auto redo = key == 'y'
              || (key == 'z'
                  && (event.modifierFlags & NSEventModifierFlagShift) != 0);
          if ((key == 'z' || key == 'y')
              && installed->undoHandler != nullptr
              && installed->undoHandler(redo)) {
            return nil;
          }
        }
        // FL's wrapper can remain the native first responder even after an
        // Avalonia text control receives logical focus. Route the rest of the
        // editor's keystrokes directly to the embedded view so text entry,
        // navigation, and OpenUtau shortcuts do not depend on host focus.
        dispatchEditorShortcut(editorView, event);
        return nil;
      }];
  originalClass = installation.release();
  return true;
}

void uninstallMacSpaceKeyForwarder(
    void* const nativeView, void*& originalClass) {
  if (nativeView != nullptr && originalClass != nullptr) {
    auto* const installation =
        static_cast<ForwarderInstallation*>(originalClass);
    uninstallEditorView(*installation);
    delete installation;
  }
  originalClass = nullptr;
}

bool focusMacEditor(
    void* const nativeView, void* const opaqueInstallation) {
  if (nativeView == nullptr || opaqueInstallation == nullptr) return false;
  NSView* const view = static_cast<NSView*>(nativeView);
  NSWindow* const window = view.window;
  if (window == nil) return false;
  auto* const installation =
      static_cast<ForwarderInstallation*>(opaqueInstallation);
  if (installation->mouseWindow == nil) {
    installation->mouseWindow = window;
    installation->originalAcceptsMouseMovedEvents =
        window.acceptsMouseMovedEvents;
    window.acceptsMouseMovedEvents = YES;
  }
  return [window makeFirstResponder:view] == YES;
}

} // namespace openutau::vst
