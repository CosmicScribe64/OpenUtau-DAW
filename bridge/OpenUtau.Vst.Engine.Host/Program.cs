using System.Globalization;
using System.Net;
using System.Net.Sockets;
using OpenUtau.Vst.Engine;

// The sidecar is always a headless VST session. Set this before constructing
// ProjectDocument so paths and phonemizer initialization use deterministic
// plugin-safe behavior even when launched by a third-party host.
Environment.SetEnvironmentVariable("OPENUTAU_VST_SESSION", "1");

var options = ParseArguments(args);
using var shutdown = new CancellationTokenSource();
Console.CancelKeyPress += (_, eventArgs) => {
    eventArgs.Cancel = true;
    shutdown.Cancel();
};

var listener = new TcpListener(IPAddress.Loopback, options.Port);
listener.Start(1);
var boundPort = ((IPEndPoint)listener.LocalEndpoint).Port;
Console.Out.WriteLine($"OPENUTAU_VST_PORT={boundPort}");
Console.Out.Flush();

try {
    using var client = await listener.AcceptTcpClientAsync(shutdown.Token);
    client.NoDelay = true;
    listener.Stop();
    using var server = new EngineServer(
        authenticationToken: options.Token, editorExecutable: options.Editor);
    await server.RunAsync(client.GetStream(), shutdown.Token);
} catch (OperationCanceledException) when (shutdown.IsCancellationRequested) {
    // Normal shutdown.
} finally {
    listener.Stop();
}

static Options ParseArguments(string[] arguments) {
    int? port = null;
    string? token = null;
    string? editor = null;
    for (var i = 0; i < arguments.Length; i++) {
        switch (arguments[i]) {
            case "--port" when i + 1 < arguments.Length:
                port = int.Parse(arguments[++i], NumberStyles.None, CultureInfo.InvariantCulture);
                break;
            case "--token" when i + 1 < arguments.Length:
                token = arguments[++i];
                break;
            case "--editor" when i + 1 < arguments.Length:
                editor = Path.GetFullPath(arguments[++i]);
                break;
            default:
                throw new ArgumentException($"Unknown or incomplete argument '{arguments[i]}'.");
        }
    }
    if (port is null or < 0 or > 65535) throw new ArgumentException("--port must be 0..65535.");
    if (string.IsNullOrWhiteSpace(token) || token.Length < 32 || token.Length > 256) {
        throw new ArgumentException("--token must contain 32..256 characters.");
    }
    return new Options(port.Value, token, editor);
}

internal sealed record Options(int Port, string Token, string? Editor);
