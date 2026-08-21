using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Controls.Embedding;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Native;
using Avalonia.Platform;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Themes.Fluent;
using Avalonia.Styling;
using OpenUtau.App.Views;
using OpenUtau.Classic;
using OpenUtau.Core;
using OpenUtau.Core.Format;
using OpenUtau.Core.Ustx;
using OpenUtau.App.ViewModels;
using OpenUtau.Api;
using OpenUtau.Plugin.Builtin;
using Serilog;
using System.Text;

namespace OpenUtau.Vst.EditorHost;

/// <summary>
/// Native entry points for the embedded editor. The VST owns the parent native
/// view; Avalonia owns and renders the returned child view.
/// Calls must be made on the DAW/JUCE message thread.
/// </summary>
public static class EntryPoints {
    private static readonly object Gate = new();
    private static readonly Dictionary<nint, EmbeddableControlRoot> Roots = [];
    private static bool initialized;
    private static nint lastError;
    private static long revision;
    private static MainWindow? mainWindow;
    private static Control? embeddedContent;
    private static byte[]? currentSerializedState;
    private static long activeEditorInstance;
    private static long currentProjectInstance;
    private static readonly Dictionary<long, EditorViewState> EditorViewStates = [];
    private static int hostPlaying;
    private static readonly HostTransportViewportPolicy viewportPolicy = new();
    private static VstPreviewAudioOutput? previewAudioOutput;

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorCreate",
        CallConvs = [typeof(CallConvCdecl)])]
    public static nint Create(int width, int height, long instanceId) {
        EmbeddableControlRoot? root = null;
        var renderingStarted = false;
        try {
            EnsureInitialized();
            activeEditorInstance = instanceId;
            Log.Information("Creating VST embeddable root at {Width}x{Height}.",
                width, height);
            root = new EmbeddableControlRoot {
                Width = Math.Max(1, width),
                Height = Math.Max(1, height),
                Focusable = true,
            };
            Log.Information("Created VST embeddable root.");
            root.Content = BuildOpenUtauView();
            Log.Information("Attached OpenUtau content to VST embeddable root.");
            EmbeddedShortcutRouter.Attach(
                root,
                (sender, args) => mainWindow?.HandleEmbeddedKeyDown(sender!, args));
            root.Prepare();
            Log.Information("Prepared VST embeddable root.");
            var platformHandle = root.TryGetPlatformHandle();
            var handle = platformHandle?.Handle ?? nint.Zero;
            if (handle == nint.Zero) {
                throw new InvalidOperationException(
                    "Avalonia prepared the embedded editor without a native platform handle.");
            }
            Log.Information(
                "Prepared VST native child {Descriptor} 0x{Handle:X}.",
                platformHandle?.HandleDescriptor ?? "unknown", handle.ToInt64());
            // JUCE parents the returned child immediately after this call. Do
            // not force focus while the native view is still an unattached,
            // hidden HWND; the DAW owns activation and keyboard focus.
            root.StartRendering();
            renderingStarted = true;
            Log.Information("Started rendering VST native child 0x{Handle:X}.",
                handle.ToInt64());
            lock (Gate) {
                Roots.Add(handle, root);
            }
            return handle;
        } catch (Exception ex) {
            activeEditorInstance = 0;
            if (root is not null) TryDisposeRoot(root, renderingStarted);
            Log.Error(ex, "Failed to create the embedded VST editor.");
            SetLastError(ex.ToString());
            return nint.Zero;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorGetLastError",
        CallConvs = [typeof(CallConvCdecl)])]
    public static nint GetLastError() => lastError;

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorGetRevision",
        CallConvs = [typeof(CallConvCdecl)])]
    public static long GetRevision() => Interlocked.Read(ref revision);

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorExecuteUndo",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int ExecuteUndo(int redo) {
        try {
            if (EmbeddedTextInputFocused()) return 1;
            if (mainWindow?.DataContext is not MainWindowViewModel viewModel) {
                return -1;
            }
            if (redo != 0) {
                viewModel.Redo();
            } else {
                viewModel.Undo();
            }
            return 0;
        } catch (Exception ex) {
            SetLastError(ex.ToString());
            return -1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorExecuteDelete",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int ExecuteDelete(int forwardDelete) {
        try {
            if (FocusedTextBox() is TextBox textBox) {
                DeleteText(textBox, forwardDelete != 0);
                return 0;
            }
            var container = mainWindow?.FindControl<ContentControl>(
                "PianoRollContainer");
            if ((container?.Content as Control)?.DataContext
                    is PianoRollViewModel pianoRoll
                    && pianoRoll.NotesViewModel.Selection.Count > 0) {
                pianoRoll.Delete();
                return 0;
            }
            if (mainWindow?.DataContext is MainWindowViewModel viewModel) {
                viewModel.TracksViewModel.DeleteSelectedParts();
                return 0;
            }
            return -1;
        } catch (Exception ex) {
            SetLastError(ex.ToString());
            return -1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorIsTextInputFocused",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int IsTextInputFocused() {
        try {
            return EmbeddedTextInputFocused() ? 1 : 0;
        } catch (Exception ex) {
            SetLastError(ex.ToString());
            return 0;
        }
    }

    private static void DeleteText(TextBox textBox, bool forwardDelete) {
        var text = textBox.Text ?? string.Empty;
        var selectionStart = Math.Min(
            textBox.SelectionStart, textBox.SelectionEnd);
        var selectionEnd = Math.Max(
            textBox.SelectionStart, textBox.SelectionEnd);
        selectionStart = Math.Clamp(selectionStart, 0, text.Length);
        selectionEnd = Math.Clamp(selectionEnd, selectionStart, text.Length);

        if (selectionStart == selectionEnd) {
            if (forwardDelete) {
                if (selectionEnd >= text.Length) return;
                selectionEnd++;
                if (selectionEnd < text.Length
                        && char.IsHighSurrogate(text[selectionEnd - 1])
                        && char.IsLowSurrogate(text[selectionEnd])) {
                    selectionEnd++;
                }
            } else {
                if (selectionStart == 0) return;
                selectionStart--;
                if (selectionStart > 0
                        && char.IsLowSurrogate(text[selectionStart])
                        && char.IsHighSurrogate(text[selectionStart - 1])) {
                    selectionStart--;
                }
            }
        }

        textBox.Text = text.Remove(
            selectionStart, selectionEnd - selectionStart);
        textBox.CaretIndex = selectionStart;
        textBox.SelectionStart = selectionStart;
        textBox.SelectionEnd = selectionStart;
    }

    private static TextBox? FocusedTextBox() {
        EmbeddableControlRoot? root;
        lock (Gate) {
            root = Roots.Values.FirstOrDefault();
        }
        return root?.FocusManager?.GetFocusedElement() as TextBox;
    }

    private static bool EmbeddedTextInputFocused() {
        EmbeddableControlRoot? root;
        lock (Gate) {
            root = Roots.Values.FirstOrDefault();
        }
        var focused = root?.FocusManager?.GetFocusedElement();
        return focused is TextBox { IsEnabled: true, IsEffectivelyVisible: true }
            or ComboBox
            or ComboBoxItem;
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorCopyState",
        CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe int CopyState(byte* destination, int capacity) {
        try {
            var bytes = SaveProject();
            if (destination == null || capacity < bytes.Length) return bytes.Length;
            bytes.CopyTo(new Span<byte>(destination, capacity));
            return bytes.Length;
        } catch (Exception ex) {
            SetLastError(ex.ToString());
            return -1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorGetPreviewState",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int GetPreviewState() =>
        previewAudioOutput?.PlaybackState == NAudio.Wave.PlaybackState.Playing ? 1 : 0;

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorGetPreviewRevision",
        CallConvs = [typeof(CallConvCdecl)])]
    public static long GetPreviewRevision() =>
        previewAudioOutput?.BufferRevision ?? 0;

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorGetPreviewTone",
        CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe int GetPreviewTone(double* frequency, long* revision) {
        if (previewAudioOutput is null || frequency == null || revision == null) {
            return -1;
        }
        var state = previewAudioOutput.CopyToneState(
            out var currentFrequency, out var currentRevision);
        *frequency = currentFrequency;
        *revision = currentRevision;
        return state;
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorCopyPreview",
        CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe int CopyPreview(float* destination, int capacityFrames) {
        try {
            if (previewAudioOutput is null) return -1;
            if (destination == null || capacityFrames <= 0
                    || capacityFrames > 1 << 20) {
                return previewAudioOutput.PlaybackState
                    == NAudio.Wave.PlaybackState.Playing ? 0 : -1;
            }
            return previewAudioOutput.CopyTo(
                new Span<float>(destination, checked(capacityFrames * 2)));
        } catch (Exception ex) {
            SetLastError(ex.ToString());
            return -2;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorSetState",
        CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe int SetState(byte* source, int size) {
        try {
            if (source == null || size <= 0 || size > 256 * 1024 * 1024) return -1;
            var bytes = new ReadOnlySpan<byte>(source, size).ToArray();
            lock (Gate) {
                if (activeEditorInstance == currentProjectInstance
                        && embeddedContent is not null
                        && currentSerializedState is not null
                        && bytes.AsSpan().SequenceEqual(currentSerializedState)) {
                    return 0;
                }
            }
            var text = new UTF8Encoding(false, true).GetString(bytes);
            var project = Yaml.DefaultDeserializer.Deserialize<UProject>(text)
                ?? throw new InvalidDataException("USTX state did not contain a project.");
            Ustx.AddDefaultExpressions(project);
            project.AfterLoad();
            project.ValidateFull();
            project.FilePath = string.Empty;
            project.Saved = true;
            DocManager.Inst.ExecuteCmd(new LoadProjectNotification(project));
            currentProjectInstance = activeEditorInstance;
            RestoreEditorViewState(activeEditorInstance);
            lock (Gate) {
                currentSerializedState = bytes;
            }
            Interlocked.Increment(ref revision);
            return 0;
        } catch (Exception ex) {
            SetLastError(ex.ToString());
            return -1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorSetHostTransport",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int SetHostTransport(
            long projectSample, double sampleRate, double quarterNotePosition,
            double bpm, int timeSignatureNumerator,
            int timeSignatureDenominator, int playing) {
        try {
            EnsureInitialized();
            var isPlaying = playing != 0;
            var wasPlaying = Interlocked.Exchange(
                ref hostPlaying, isPlaying ? 1 : 0) != 0;
            previewAudioOutput?.SetSampleRate(sampleRate);

            // Embedded preview is useful while the DAW is stopped, but two
            // simultaneous clocks drift and can double the vocal. As soon as
            // the host starts, its transport becomes authoritative.
            if (isPlaying && PlaybackManager.Inst.PlayingMaster) {
                PlaybackManager.Inst.StopPlayback();
            }

            var project = DocManager.Inst.Project;
            var timingChanged = false;
            if (double.IsFinite(bpm) && bpm is >= 10 and <= 1000
                    && Math.Abs(project.tempos[0].bpm - bpm) > 1e-9) {
                project.tempos[0].bpm = bpm;
                timingChanged = true;
            }
            if (timeSignatureNumerator is >= 1 and <= 64
                    && timeSignatureDenominator is >= 1 and <= 64
                    && (timeSignatureDenominator & (timeSignatureDenominator - 1)) == 0) {
                var signature = project.timeSignatures[0];
                if (signature.beatPerBar != timeSignatureNumerator
                        || signature.beatUnit != timeSignatureDenominator) {
                    signature.beatPerBar = timeSignatureNumerator;
                    signature.beatUnit = timeSignatureDenominator;
                    timingChanged = true;
                }
            }
            if (timingChanged) {
                // Host timing is automation, not a user edit, so update the
                // embedded project without adding an undo-history entry.
                project.ValidateFull();
                Interlocked.Increment(ref revision);
            }
            int tick;
            if (double.IsFinite(quarterNotePosition)) {
                tick = (int)Math.Clamp(
                    Math.Round(quarterNotePosition * project.resolution),
                    0, int.MaxValue);
            } else if (double.IsFinite(sampleRate) && sampleRate > 0) {
                var milliseconds = Math.Max(0, projectSample) * 1000.0 / sampleRate;
                tick = Math.Max(0, project.timeAxis.MsPosToTickPos(milliseconds));
            } else {
                return -1;
            }
            // While the DAW is stopped, OpenUtau's preview player owns the
            // cursor. Re-applying the host's stationary position every 33 ms
            // would pin preview playback at its start. A stopped host may
            // still seek whenever preview itself is inactive.
            var positionChanged = DocManager.Inst.playPosTick != tick;
            var preserveViewport = viewportPolicy.PreserveViewport(
                wasPlaying, isPlaying, positionChanged);
            if (!PlaybackManager.Inst.PlayingMaster && positionChanged) {
                DocManager.Inst.ExecuteCmd(new SetPlayPosTickNotification(
                    tick,
                    pause: preserveViewport));
            }
            if (mainWindow?.DataContext is MainWindowViewModel viewModel) {
                if (timingChanged) {
                    viewModel.PlaybackViewModel.NotifyExternalTimingChanged();
                }
                viewModel.PlaybackViewModel.NotifyExternalTransportChanged();
            }
            return 0;
        } catch (Exception ex) {
            SetLastError(ex.ToString());
            return -1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "OpenUtauVstEditorDestroy",
        CallConvs = [typeof(CallConvCdecl)])]
    public static void Destroy(nint handle) {
        try {
            EmbeddableControlRoot? root = null;
            lock (Gate) {
                if (Roots.Remove(handle, out var found)) root = found;
            }
            if (root is null) return;
            CaptureEditorViewState(activeEditorInstance);
            activeEditorInstance = 0;
            TryDisposeRoot(root, renderingStarted: true);
        } catch (Exception ex) {
            // An exception must never escape an UnmanagedCallersOnly method
            // into a DAW host process.
            Log.Error(ex, "Failed to destroy embedded VST editor 0x{Handle:X}.",
                handle.ToInt64());
            SetLastError(ex.ToString());
        }
    }

    private static void TryDisposeRoot(
            EmbeddableControlRoot root, bool renderingStarted) {
        if (renderingStarted) {
            try {
                root.StopRendering();
            } catch (Exception ex) {
                Log.Warning(ex, "Failed to stop VST editor rendering during cleanup.");
            }
        }
        // Keep the OpenUtau control tree alive between DAW editor close/open
        // cycles. Reparenting it into the next embeddable root preserves the
        // piano-roll page, selection, zoom, scroll position, and pane sizes.
        if (ReferenceEquals(root.Content, embeddedContent)) {
            root.Content = null;
        }
        try {
            root.Dispose();
        } catch (Exception ex) {
            Log.Warning(ex, "Failed to dispose VST editor root during cleanup.");
        }
    }

    private static void EnsureInitialized() {
        lock (Gate) {
            if (initialized) return;
            Environment.SetEnvironmentVariable("OPENUTAU_VST_SESSION", "1");
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            OpenUtau.App.Program.InitLogging();
            var builder = OpenUtau.App.Program.BuildAvaloniaApp();
            if (OperatingSystem.IsMacOS()) {
                builder = builder.With(new MacOSPlatformOptions {
                    DisableAvaloniaAppDelegate = true,
                    DisableNativeMenus = true,
                });
            }
            builder.SetupWithoutStarting();
            if (OperatingSystem.IsMacOS() && Application.Current != null) {
                // The embedded Avalonia application must not rename the DAW's
                // global macOS menu from "FL Studio" to "OpenUtau".
                Application.Current.Name =
                    Environment.GetEnvironmentVariable("OPENUTAU_VST_HOST_NAME")
                    ?? "DAW Host";
            }
            var embeddedPopupStyle = new Style(x => x.OfType<Popup>());
            embeddedPopupStyle.Setters.Add(
                new Setter(Popup.ShouldUseOverlayLayerProperty, true));
            Application.Current?.Styles.Add(embeddedPopupStyle);
            var mainThread = Thread.CurrentThread;
            var scheduler = TaskScheduler.FromCurrentSynchronizationContext();
            ToolsManager.Inst.Initialize();
            SingerManager.Inst.Initialize();
            DocManager.Inst.Initialize(mainThread, scheduler);
            // A plug-in must never open the hardware audio device beside its
            // host. MiniAudioOutput is fixed at 44.1 kHz and creating it here
            // can make a 48 kHz DAW session (including every other instrument)
            // play slowly and distort when CoreAudio changes or contends for
            // the shared device. Editor preview renders into a memory ring and
            // leaves exclusively through the VST processor outputs.
            previewAudioOutput = new VstPreviewAudioOutput();
            PlaybackManager.Inst.AudioOutput = previewAudioOutput;
            // The editor is loaded through hostfxr inside a native DAW rather
            // than through OpenUtau.exe. Anchor built-in discovery in the
            // default load context so every bundled phonemizer is available
            // even when Assembly.LoadFrom resolves a duplicate dependency.
            foreach (var type in typeof(EnXSampaPhonemizer).Assembly.GetExportedTypes()) {
                if (!type.IsAbstract && type.IsSubclassOf(typeof(Phonemizer))) {
                    PhonemizerFactory.Get(type);
                }
            }
            PhonemizerFactory.BuildList();
            Log.Information("VST built-in phonemizer registration count: {Count}",
                PhonemizerFactory.GetAll().Length);
            DocManager.Inst.PostOnUIThread = action =>
                Avalonia.Threading.Dispatcher.UIThread.Post(action);
            DocManager.Inst.AddSubscriber(new StateSubscriber());
            initialized = true;
        }
    }

    private static Control BuildOpenUtauView() {
        Log.Information("Building VST editor with {Count} phonemizers",
            PhonemizerFactory.GetAll().Length);
        if (embeddedContent is not null) {
            Log.Information("Reusing the existing OpenUtau editor view.");
            return embeddedContent;
        }
        var window = new MainWindow();
        Log.Information("Created OpenUtau window shell for VST embedding.");
        var content = window.Content as Control
            ?? throw new InvalidOperationException("OpenUtau MainWindow has no control content.");
        window.Content = null;
        content.DataContext = window.DataContext;
        Log.Information("Detached OpenUtau content from its window shell.");
        // MainWindow.InitProject is the standalone application's command-line
        // and recovery bootstrap. In a plug-in process those arguments belong
        // to the DAW (and the Windows smoke host passes the VST3 module path),
        // so treating them as an OpenUtau project can fail editor creation.
        // The plug-in restores its project through OpenUtauVstEditorLoadState.
        Log.Information("Skipped standalone project bootstrap for VST embedding.");
        if (window.DataContext is MainWindowViewModel viewModel) {
            viewModel.PlaybackViewModel.SetExternalTransport(
                () => Volatile.Read(ref hostPlaying) != 0);
        }
        mainWindow = window;
        embeddedContent = content;
        return embeddedContent;
    }

    private static byte[] SaveProject() {
        var project = DocManager.Inst.Project;
        project.ustxVersion = Ustx.kUstxVersion;
        project.BeforeSave();
        try {
            var bytes = Encoding.UTF8.GetBytes(
                Yaml.DefaultSerializer.Serialize(project));
            lock (Gate) {
                currentSerializedState = bytes;
            }
            return bytes;
        } finally {
            project.AfterSave();
        }
    }

    private static PianoRollViewModel? CurrentPianoRoll() {
        var container = mainWindow?.FindControl<ContentControl>(
            "PianoRollContainer");
        return (container?.Content as Control)?.DataContext
            as PianoRollViewModel;
    }

    private static void CaptureEditorViewState(long instanceId) {
        if (instanceId == 0
                || mainWindow?.DataContext is not MainWindowViewModel main
                || CurrentPianoRoll() is not PianoRollViewModel piano) {
            return;
        }
        var notes = piano.NotesViewModel;
        var partIndex = notes.Part is null
            ? -1 : DocManager.Inst.Project.parts.IndexOf(notes.Part);
        var tracks = main.TracksViewModel;
        EditorViewStates[instanceId] = new EditorViewState(
            main.Page, main.ShowPianoRoll, partIndex,
            tracks.TickWidth, tracks.TrackHeight,
            tracks.TickOffset, tracks.TrackOffset,
            notes.TickWidth, notes.TrackHeight,
            notes.TickOffset, notes.TrackOffset);
    }

    private static void RestoreEditorViewState(long instanceId) {
        if (mainWindow?.DataContext is not MainWindowViewModel main) return;
        if (!EditorViewStates.TryGetValue(instanceId, out var state)) {
            main.Page = 1;
            return;
        }
        var tracks = main.TracksViewModel;
        tracks.TickWidth = state.TracksTickWidth;
        tracks.TrackHeight = state.TracksTrackHeight;
        tracks.TickOffset = state.TracksTickOffset;
        tracks.TrackOffset = state.TracksTrackOffset;
        main.Page = state.Page;
        main.ShowPianoRoll = state.ShowPianoRoll;

        var project = DocManager.Inst.Project;
        if (state.PartIndex < 0 || state.PartIndex >= project.parts.Count
                || project.parts[state.PartIndex] is not UVoicePart part) {
            return;
        }
        DocManager.Inst.ExecuteCmd(new LoadPartNotification(
            part, project, part.position + (int)state.NotesTickOffset));
        if (CurrentPianoRoll() is not PianoRollViewModel piano) return;
        var notes = piano.NotesViewModel;
        notes.TickWidth = state.NotesTickWidth;
        notes.TrackHeight = state.NotesTrackHeight;
        notes.TickOffset = state.NotesTickOffset;
        notes.TrackOffset = state.NotesTrackOffset;
    }

    private sealed record EditorViewState(
        int Page,
        bool ShowPianoRoll,
        int PartIndex,
        double TracksTickWidth,
        double TracksTrackHeight,
        double TracksTickOffset,
        double TracksTrackOffset,
        double NotesTickWidth,
        double NotesTrackHeight,
        double NotesTickOffset,
        double NotesTrackOffset);

    private static void SetLastError(string message) {
        lock (Gate) {
            if (lastError != nint.Zero) Marshal.FreeCoTaskMem(lastError);
            lastError = Marshal.StringToCoTaskMemUTF8(message);
        }
    }

    private sealed class StateSubscriber : ICmdSubscriber {
        public void OnNext(UCommand command, bool isUndo) {
            if (command is not UNotification || command is LoadProjectNotification) {
                Interlocked.Increment(ref revision);
            }
        }
    }
}
