using Avalonia.Input;
using Avalonia.Interactivity;

namespace OpenUtau.Vst.EditorHost;

/// <summary>
/// Reconnects window-level OpenUtau shortcuts after MainWindow.Content is
/// moved beneath an EmbeddableControlRoot for VST hosting.
/// </summary>
public static class EmbeddedShortcutRouter {
    public static void Attach(
            InputElement root, EventHandler<KeyEventArgs> handler) {
        ArgumentNullException.ThrowIfNull(root);
        ArgumentNullException.ThrowIfNull(handler);
        root.AddHandler(
            InputElement.KeyDownEvent,
            handler,
            RoutingStrategies.Tunnel);
    }
}
