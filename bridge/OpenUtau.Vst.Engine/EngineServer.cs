using System.Text.Json;
using System.Security.Cryptography;
using System.Text;
using OpenUtau.Vst.Protocol;

namespace OpenUtau.Vst.Engine;

/// <summary>Serves one plugin instance over an authenticated local stream.</summary>
public sealed class EngineServer : IDisposable {
    private readonly ProjectDocument document;
    private readonly ProjectRenderer renderer;
    private readonly byte[]? authenticationToken;
    private bool authenticated;
    private string? editorDirectory;
    private string? editorProjectPath;
    private DateTime editorProjectWriteUtc;
    private System.Diagnostics.Process? editorProcess;
    private readonly string? editorExecutable;

    public EngineServer(ProjectDocument? document = null, string? authenticationToken = null,
                        string? editorExecutable = null) {
        this.document = document ?? new ProjectDocument();
        renderer = new ProjectRenderer(this.document);
        this.authenticationToken = authenticationToken is null
            ? null
            : Encoding.UTF8.GetBytes(authenticationToken);
        authenticated = this.authenticationToken is null;
        this.editorExecutable = editorExecutable;
    }

    public async Task RunAsync(Stream stream, CancellationToken cancellationToken = default) {
        ArgumentNullException.ThrowIfNull(stream);
        while (!cancellationToken.IsCancellationRequested) {
            var request = await FramedStream.ReadAsync<ControlRequest>(stream, cancellationToken)
                .ConfigureAwait(false);
            if (request is null) return;
            ControlResponse response;
            try {
                response = Handle(request, cancellationToken);
            } catch (Exception error) when (error is not OperationCanceledException) {
                var fault = new EngineFault(error.GetType().Name, error.Message, true);
                response = new ControlResponse(
                    request.RequestId, ControlKinds.Error,
                    JsonSerializer.Serialize(fault, ProtocolJsonContext.Default.EngineFault));
            }
            await FramedStream.WriteAsync(stream, response, cancellationToken).ConfigureAwait(false);
        }
    }

    private ControlResponse Handle(ControlRequest request, CancellationToken cancellationToken) {
        if (!authenticated) {
            if (request.Kind != ControlKinds.Hello || !TokenMatches(request.Payload)) {
                throw new UnauthorizedAccessException("Engine authentication failed.");
            }
            authenticated = true;
        }
        RefreshEditorProject();
        return request.Kind switch {
            ControlKinds.Hello => new ControlResponse(
                request.RequestId, ControlKinds.Ok, ProtocolVersion.Current.ToString()),
            ControlKinds.GetState => new ControlResponse(
                request.RequestId, ControlKinds.Ok, Convert.ToBase64String(document.SaveUstx())),
            ControlKinds.SetState => SetState(request),
            ControlKinds.Render => Render(request, cancellationToken),
            ControlKinds.SetHostTiming => SetHostTiming(request),
            ControlKinds.OpenEditor => OpenEditor(request),
            _ => throw new InvalidDataException($"Unknown control request '{request.Kind}'."),
        };
    }

    private ControlResponse OpenEditor(ControlRequest request) {
        if (editorProcess is { HasExited: false }) {
            return new ControlResponse(request.RequestId, ControlKinds.Ok, editorProjectPath);
        }
        var editor = editorExecutable ?? Environment.GetEnvironmentVariable("OPENUTAU_VST_EDITOR");
        if (string.IsNullOrWhiteSpace(editor) || !File.Exists(editor)) {
            throw new FileNotFoundException(
                "The packaged OpenUtau editor executable is unavailable.", editor);
        }
        editorDirectory ??= Path.Combine(Path.GetTempPath(), "OpenUtauVst", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(editorDirectory);
        editorProjectPath = Path.Combine(editorDirectory, "plugin-project.ustx");
        File.WriteAllBytes(editorProjectPath, document.SaveUstx());
        editorProjectWriteUtc = File.GetLastWriteTimeUtc(editorProjectPath);
        var startInfo = new System.Diagnostics.ProcessStartInfo(editor) {
            UseShellExecute = false,
        };
        startInfo.ArgumentList.Add(editorProjectPath);
        startInfo.Environment["OPENUTAU_VST_SESSION"] = "1";
        editorProcess = System.Diagnostics.Process.Start(startInfo)
            ?? throw new InvalidOperationException("Failed to launch the OpenUtau editor.");
        return new ControlResponse(request.RequestId, ControlKinds.Ok, editorProjectPath);
    }

    private void RefreshEditorProject() {
        if (editorProjectPath is null || !File.Exists(editorProjectPath)) return;
        var writeUtc = File.GetLastWriteTimeUtc(editorProjectPath);
        if (writeUtc <= editorProjectWriteUtc) return;
        try {
            var bytes = File.ReadAllBytes(editorProjectPath);
            document.LoadUstx(bytes);
            editorProjectWriteUtc = writeUtc;
        } catch (IOException) {
            // The editor may still be replacing the file. Retry next request.
        }
    }

    public void Dispose() {
        if (editorProcess is { HasExited: false }) {
            try { editorProcess.Kill(entireProcessTree: true); } catch { }
        }
        editorProcess?.Dispose();
        if (editorDirectory is not null && Directory.Exists(editorDirectory)) {
            try { Directory.Delete(editorDirectory, recursive: true); } catch { }
        }
    }

    private bool TokenMatches(string? candidate) {
        if (candidate is null || authenticationToken is null) return false;
        var candidateBytes = Encoding.UTF8.GetBytes(candidate);
        return candidateBytes.Length == authenticationToken.Length
            && CryptographicOperations.FixedTimeEquals(candidateBytes, authenticationToken);
    }

    private ControlResponse SetState(ControlRequest request) {
        document.LoadUstx(Convert.FromBase64String(
            request.Payload ?? throw new InvalidDataException("State payload is missing.")));
        return new ControlResponse(request.RequestId, ControlKinds.Ok, document.Revision.ToString());
    }

    private ControlResponse SetHostTiming(ControlRequest request) {
        var timing = JsonSerializer.Deserialize(
            request.Payload ?? throw new InvalidDataException("Host timing payload is missing."),
            ProtocolJsonContext.Default.HostTiming)
            ?? throw new InvalidDataException("Host timing payload is null.");
        document.SynchronizeHostTiming(timing);
        return new ControlResponse(request.RequestId, ControlKinds.Ok, document.Revision.ToString());
    }

    private ControlResponse Render(ControlRequest request, CancellationToken cancellationToken) {
        var renderRequest = JsonSerializer.Deserialize(
            request.Payload ?? throw new InvalidDataException("Render payload is missing."),
            ProtocolJsonContext.Default.RenderRequest)
            ?? throw new InvalidDataException("Render request is null.");
        if (int.TryParse(Environment.GetEnvironmentVariable("OPENUTAU_VST_TEST_RENDER_DELAY_MS"),
                         out var testDelay) && testDelay is > 0 and <= 10000) {
            Task.Delay(testDelay, cancellationToken).GetAwaiter().GetResult();
        }
        var samples = renderer.Render(
            renderRequest.StartSample, renderRequest.FrameCount,
            renderRequest.SampleRate, cancellationToken);
        var bytes = new byte[checked(samples.Length * sizeof(float))];
        Buffer.BlockCopy(samples, 0, bytes, 0, bytes.Length);
        return new ControlResponse(request.RequestId, ControlKinds.Ok, Convert.ToBase64String(bytes));
    }
}
