using OpenUtau.Core.Ustx;
using OpenUtau.Vst.Engine;
using System.Text;
using System.Text.Json;
using OpenUtau.Vst.Protocol;
using System.Diagnostics;
using System.Net.Sockets;
using OpenUtau.Classic;
using OpenUtau.Core.Render;
using OpenUtau.Api;

if (!string.IsNullOrWhiteSpace(
        Environment.GetEnvironmentVariable("OPENUTAU_VST_INSTALLED_SINGER_USTX"))) {
    VerifyInstalledSingerRender();
    Console.WriteLine("Installed singer render test passed.");
    return;
}

var document = new ProjectDocument();
document.Edit(project => {
    project.name = "FL Studio state round-trip あ";
    project.tempos[0].bpm = 137.5;
    var part = new UVoicePart { name = "Lead", trackNo = 0, position = 240, duration = 1920 };
    var note = project.CreateNote(60, 120, 480);
    note.lyric = "あ";
    part.notes.Add(note);
    project.parts.Add(part);
});

Require(document.Revision == 1, "Edit did not advance revision.");
var payload = document.SaveUstx();
Require(payload.Length > 0, "Project serialized to an empty payload.");

var restored = new ProjectDocument();
restored.LoadUstx(payload);
Require(restored.Revision == 1, "Load did not advance revision.");
restored.Read(project => {
    Require(project.name == "FL Studio state round-trip あ", "Project name was not restored.");
    Require(project.tempos[0].bpm == 137.5, "Tempo was not restored.");
    Require(project.parts.Count == 1, "Voice part was not restored.");
    var part = project.parts[0] as UVoicePart;
    Require(part is not null && part.notes.Count == 1, "Voice note was not restored.");
    Require(part!.notes.First().lyric == "あ", "Unicode lyric was not restored.");
    return true;
});
restored.SynchronizeHostTiming(new HostTiming(4.0, 155.0, 2, 7, 8));
restored.Read(project => {
    Require(project.tempos.Any(tempo => tempo.position == 1920 && tempo.bpm == 155.0),
        "Host tempo marker was not synchronized at the musical position.");
    Require(project.timeSignatures.Any(signature => signature.barPosition == 2
            && signature.beatPerBar == 7 && signature.beatUnit == 8),
        "Host time signature was not synchronized.");
    return true;
});

ExpectInvalid(Array.Empty<byte>());
ExpectInvalid(new byte[] { 0xff, 0xfe, 0xfd });

var emptyDocument = new ProjectDocument();
var renderer = new ProjectRenderer(emptyDocument);
foreach (var sampleRate in new[] { 44100.0, 48000.0, 96000.0 }) {
    var audio = renderer.Render(1234, 257, sampleRate);
    Require(audio.Length == 514, "Renderer returned the wrong stereo frame count.");
    Require(audio.All(sample => sample == 0), "Empty project did not render silence.");
}

await VerifyNonSilentClassicRenderAsync();

using (var clientToServer = new MemoryStream())
using (var serverToClient = new MemoryStream()) {
    await FramedStream.WriteAsync(clientToServer, new ControlRequest(42, ControlKinds.Hello, "secret"));
    clientToServer.Position = 0;
    var server = new EngineServer(document, "secret");
    await server.RunAsync(new DuplexTestStream(clientToServer, serverToClient));
    serverToClient.Position = 0;
    var response = await FramedStream.ReadAsync<ControlResponse>(serverToClient);
    Require(response is { RequestId: 42, Kind: ControlKinds.Ok }, "Engine handshake failed.");
    Require(response!.Payload == ProtocolVersion.Current.ToString(), "Protocol version mismatch.");
}

await RunSidecarEndToEndAsync();
Console.WriteLine("OpenUtau project state tests passed.");

void ExpectInvalid(byte[] bytes) {
    try {
        restored.LoadUstx(bytes);
        throw new Exception("Invalid USTX payload was accepted.");
    } catch (InvalidDataException) {
        // Expected.
    } catch (DecoderFallbackException) {
        // Expected for malformed UTF-8.
    }
}

static void Require(bool condition, string message) {
    if (!condition) throw new Exception(message);
}

static void VerifyInstalledSingerRender() {
    var projectPath = Environment.GetEnvironmentVariable("OPENUTAU_VST_INSTALLED_SINGER_USTX");
    if (string.IsNullOrWhiteSpace(projectPath)) return;
    var document = new ProjectDocument();
    document.LoadUstx(File.ReadAllBytes(projectPath));
    var renderer = new ProjectRenderer(document);
    var samples = renderer.Render(0, 44100 * 2, 44100);
    document.Read(project => {
        var track = project.tracks.Single();
        var part = project.parts.OfType<UVoicePart>().Single();
        var phones = part.renderPhrases.SelectMany(phrase => phrase.phones).ToArray();
        Require(track.Singer is { Found: true, Loaded: true },
            $"Installed singer '{track.Singer?.Id}' was not loaded.");
        var singer = track.Singer!;
        Require(track.Phonemizer.GetType().FullName ==
                "OpenUtau.Plugin.Builtin.EnXSampaPhonemizer",
            $"Unexpected installed-singer phonemizer: {track.Phonemizer.GetType().FullName}.");
        Console.WriteLine(
            $"Installed singer '{singer.Id}' loaded {singer.Otos.Count} oto aliases; " +
            $"part has {part.phonemes.Count} phonemes and {phones.Length} render phones.");
        var apiNote = new Phonemizer.Note {
            lyric = "hi",
            duration = 960,
            position = 0,
            tone = 60,
            phonemeAttributes = new[] { new Phonemizer.PhonemeAttributes {
                index = 0,
                consonantStretchRatio = 1,
                voiceColor = "",
            } },
        };
        track.Phonemizer.SetSinger(singer);
        track.Phonemizer.SetTiming(project.timeAxis);
        track.Phonemizer.SetUp(new[] { new[] { apiNote } }, project, track);
        var direct = track.Phonemizer.Process(
            new[] { apiNote }, null, null, null, null, Array.Empty<Phonemizer.Note>());
        Console.WriteLine("Direct phonemizer aliases: " +
            string.Join(", ", direct.phonemes.Select(phone => phone.phoneme)));
        Require(phones.Length > 0,
            "Installed-singer phonemizer produced no renderable phones. " +
            $"Notes: {string.Join("; ", part.notes.Select(note =>
                $"{note.lyric} error={note.Error}"))}");
        Require(part.phonemes.All(phone => !phone.Error),
            "Installed-singer phonemes contain errors: " +
            string.Join("; ", part.phonemes.Select(phone =>
                $"{phone.phoneme} mapped={phone.phonemeMapped} error={phone.Error} " +
                $"reason={phone.ErrorException?.Message}")));
        return true;
    });
    Require(samples.Any(sample => Math.Abs(sample) > 1e-5f),
        "Installed singer rendered silence despite valid phones.");
}

static string FindEngineHost() {
    var root = new DirectoryInfo(AppContext.BaseDirectory);
    while (root is not null && !File.Exists(Path.Combine(root.FullName, "Directory.Build.props"))) {
        root = root.Parent;
    }
    Require(root is not null, "Could not locate repository root for sidecar E2E.");
    var hostDll = Path.Combine(root!.FullName, ".build", "dotnet", "bin",
        "OpenUtau.Vst.Engine.Host", "Release", "net8.0",
        "OpenUtau.Vst.Engine.Host.dll");
    Require(File.Exists(hostDll), $"Engine host was not built at {hostDll}.");
    return hostDll;
}

static async Task VerifyNonSilentClassicRenderAsync() {
    var root = Path.Combine(Path.GetTempPath(), "OpenUtauVstRenderTest", Guid.NewGuid().ToString("N"));
    var dataHome = Path.Combine(root, "data");
    var cacheHome = Path.Combine(root, "cache");
    var singerRoot = Path.Combine(dataHome, "OpenUtau", "Singers");
    var voiceDirectory = Path.Combine(singerRoot, "VSTFixture");
    Directory.CreateDirectory(voiceDirectory);
    try {
        var character = Path.Combine(voiceDirectory, "character.txt");
        var audio = Path.Combine(voiceDirectory, "audio.wav");
        File.WriteAllText(character, "name=VST Sine Fixture\n", Encoding.UTF8);
        File.WriteAllText(Path.Combine(voiceDirectory, "oto.ini"),
            "audio.wav=a,0,100,500,50,10\n", Encoding.UTF8);
        WriteSineWave(audio, 44100, 2.0, 220.0);
        VoicebankLoader.IsTest = true;
        var voicebank = new Voicebank { File = character, BasePath = singerRoot };
        VoicebankLoader.LoadVoicebank(voicebank);
        var singer = new ClassicSinger(voicebank);
        singer.EnsureLoaded();
        Require(singer.Loaded && singer.TryGetMappedOto("a", 60, "", out _),
            "Redistributable sine voicebank fixture did not load.");

        Directory.CreateDirectory(OpenUtau.Core.PathManager.Inst.CachePath);
        var singingDocument = new ProjectDocument();
        singingDocument.Edit(project => {
            project.tracks[0].Singer = singer;
            project.tracks[0].RendererSettings.renderer = Renderers.WORLDLINE_R;
            var part = new UVoicePart {
                name = "Sine render",
                trackNo = 0,
                position = 0,
                duration = 1920,
            };
            var note = project.CreateNote(60, 0, 960);
            note.lyric = "a";
            part.notes.Add(note);
            project.parts.Add(part);
        });
        singingDocument.Read(project => {
            var part = project.parts.OfType<UVoicePart>().Single();
            Require(project.tracks[0].RendererSettings.Renderer is not null,
                "Classic render fixture did not select a renderer.");
            Require(part.notes.All(note => !note.Error),
                "Classic render fixture note failed validation.");
            return true;
        });
        var renderer = new ProjectRenderer(singingDocument);
        foreach (var sampleRate in new[] { 44100.0, 48000.0, 96000.0 }) {
            var samples = renderer.Render(0, checked((int)sampleRate * 2), sampleRate);
            Require(samples.Any(sample => Math.Abs(sample) > 1e-5f),
                $"Classic singer rendered silence at {sampleRate} Hz.");
            Require(samples.All(float.IsFinite),
                $"Classic singer produced non-finite samples at {sampleRate} Hz.");
        }
        singingDocument.Read(project => {
            var part = project.parts.OfType<UVoicePart>().Single();
            Require(part.renderPhrases.SelectMany(phrase => phrase.phones).Any(),
                "Synchronous VST phonemization did not produce render phrases.");
            return true;
        });

        var fallbackDocument = new ProjectDocument();
        fallbackDocument.Edit(project => {
            var track = project.tracks[0];
            track.Singer = singer;
            track.RendererSettings.renderer = Renderers.CLASSIC;
            track.RendererSettings.resampler = "not-installed-resampler";
            track.RendererSettings.wavtool = "not-installed-wavtool";
            var part = new UVoicePart {
                name = "Missing tool fallback",
                trackNo = 0,
                position = 0,
                duration = 1920,
            };
            var note = project.CreateNote(60, 0, 960);
            note.lyric = "a";
            part.notes.Add(note);
            project.parts.Add(part);
        });
        fallbackDocument.Read(project => {
            var settings = project.tracks[0].RendererSettings;
            Require(settings.Resampler?.GetType().Name == "WorldlineResampler"
                    && settings.resampler != "not-installed-resampler",
                "Missing resampler did not fall back to bundled Worldline.");
            Require(settings.Wavtool?.GetType().Name == "SharpWavtool"
                    && settings.wavtool != "not-installed-wavtool",
                "Missing wavtool did not fall back to bundled SharpWavtool.");
            return true;
        });
        var fallbackSamples = new ProjectRenderer(fallbackDocument)
            .Render(0, 44100 * 2, 44100);
        Require(fallbackSamples.Any(sample => Math.Abs(sample) > 1e-5f),
            "Bundled resampler/wavtool fallback rendered silence.");
        Require(fallbackSamples.All(float.IsFinite),
            "Bundled resampler/wavtool fallback produced non-finite samples.");

        const string missingSingerId = "VSTFixtureMissingSinger";
        var missingSingerDocument = new ProjectDocument();
        missingSingerDocument.Edit(project => {
            project.tracks[0].Singer = USinger.CreateMissing(missingSingerId);
            project.tracks[0].singer = missingSingerId;
            var part = new UVoicePart {
                name = "Missing singer safety",
                trackNo = 0,
                position = 0,
                duration = 1920,
            };
            var note = project.CreateNote(60, 0, 960);
            note.lyric = "a";
            part.notes.Add(note);
            project.parts.Add(part);
        });
        var restoredMissingSinger = new ProjectDocument();
        restoredMissingSinger.LoadUstx(missingSingerDocument.SaveUstx());
        restoredMissingSinger.Read(project => {
            Require(project.tracks[0].Singer is { Found: false },
                "Missing singer was not restored as a safe placeholder.");
            return true;
        });
        var missingSingerSamples = new ProjectRenderer(restoredMissingSinger)
            .Render(0, 44100, 44100);
        Require(missingSingerSamples.All(float.IsFinite),
            "Missing singer produced non-finite samples.");
        Require(missingSingerSamples.All(sample => sample == 0),
            "Missing singer should render deterministic silence.");
        Require(Encoding.UTF8.GetString(restoredMissingSinger.SaveUstx())
                .Contains(missingSingerId, StringComparison.Ordinal),
            "Missing singer identifier was lost from the saved DAW state.");

        await VerifySidecarSingerRenderAsync(
            singingDocument.SaveUstx(), dataHome, cacheHome);
        await Task.CompletedTask;
    } finally {
        try { Directory.Delete(root, recursive: true); } catch { }
    }
}

static async Task VerifySidecarSingerRenderAsync(
        byte[] state, string dataHome, string cacheHome) {
    const string token = "abcdef0123456789abcdef0123456789";
    var hostDll = FindEngineHost();
    using var process = Process.Start(new ProcessStartInfo {
        FileName = "dotnet",
        ArgumentList = { hostDll, "--port", "0", "--token", token },
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
        CreateNoWindow = true,
        Environment = {
            ["XDG_DATA_HOME"] = dataHome,
            ["XDG_CACHE_HOME"] = cacheHome,
            // PathManager uses explicit VST paths on every platform. XDG is
            // still supplied to cover the ordinary Linux path as well.
            ["OPENUTAU_VST_DATA_HOME"] = Path.Combine(dataHome, "OpenUtau"),
            ["OPENUTAU_VST_CACHE_HOME"] = Path.Combine(cacheHome, "OpenUtau"),
        },
    }) ?? throw new Exception("Failed to start singer-render engine host.");
    try {
        var ready = await process.StandardOutput.ReadLineAsync()
            .WaitAsync(TimeSpan.FromSeconds(10));
        Require(ready?.StartsWith("OPENUTAU_VST_PORT=", StringComparison.Ordinal) == true,
            $"Singer-render host did not report readiness: {ready}");
        var port = int.Parse(ready!["OPENUTAU_VST_PORT=".Length..]);
        using (var client = new TcpClient { NoDelay = true }) {
            await client.ConnectAsync("127.0.0.1", port);
            var stream = client.GetStream();
            await FramedStream.WriteAsync(stream,
                new ControlRequest(1, ControlKinds.Hello, token));
            Require(await FramedStream.ReadAsync<ControlResponse>(stream)
                    is { RequestId: 1, Kind: ControlKinds.Ok },
                "Singer-render sidecar handshake failed.");
            await FramedStream.WriteAsync(stream, new ControlRequest(
                2, ControlKinds.SetState, Convert.ToBase64String(state)));
            Require(await FramedStream.ReadAsync<ControlResponse>(stream)
                    is { RequestId: 2, Kind: ControlKinds.Ok },
                "Singer-render sidecar rejected USTX state.");
            var request = new RenderRequest(1, 0, 44100 * 2, 44100, true);
            await FramedStream.WriteAsync(stream, new ControlRequest(
                3, ControlKinds.Render,
                JsonSerializer.Serialize(request, ProtocolJsonContext.Default.RenderRequest)));
            var response = await FramedStream.ReadAsync<ControlResponse>(stream);
            Require(response is { RequestId: 3, Kind: ControlKinds.Ok },
                $"Singer-render sidecar failed: {response?.Payload}");
            var bytes = Convert.FromBase64String(response!.Payload!);
            var samples = new float[bytes.Length / sizeof(float)];
            Buffer.BlockCopy(bytes, 0, samples, 0, bytes.Length);
            Require(samples.Any(sample => Math.Abs(sample) > 1e-5f),
                "Singer-render sidecar returned silence.");
        }
        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        Require(process.ExitCode == 0,
            $"Singer-render host exited with {process.ExitCode}: "
            + await process.StandardError.ReadToEndAsync());
    } finally {
        if (!process.HasExited) process.Kill(entireProcessTree: true);
    }
}

static void WriteSineWave(string path, int sampleRate, double seconds, double frequency) {
    var frames = checked((int)Math.Round(sampleRate * seconds));
    using var stream = File.Create(path);
    using var writer = new BinaryWriter(stream, Encoding.ASCII, leaveOpen: false);
    writer.Write(Encoding.ASCII.GetBytes("RIFF"));
    writer.Write(36 + frames * sizeof(short));
    writer.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
    writer.Write(16);
    writer.Write((short)1);
    writer.Write((short)1);
    writer.Write(sampleRate);
    writer.Write(sampleRate * sizeof(short));
    writer.Write((short)sizeof(short));
    writer.Write((short)16);
    writer.Write(Encoding.ASCII.GetBytes("data"));
    writer.Write(frames * sizeof(short));
    for (var frame = 0; frame < frames; frame++) {
        var sample = 0.25 * Math.Sin(2 * Math.PI * frequency * frame / sampleRate);
        writer.Write((short)Math.Round(sample * short.MaxValue));
    }
}

static async Task RunSidecarEndToEndAsync() {
    const string token = "0123456789abcdef0123456789abcdef";
    var hostDll = FindEngineHost();
    using var process = Process.Start(new ProcessStartInfo {
        FileName = "dotnet",
        ArgumentList = { hostDll, "--port", "0", "--token", token },
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
        CreateNoWindow = true,
    }) ?? throw new Exception("Failed to start engine host.");

    try {
        var readyLine = await process.StandardOutput.ReadLineAsync()
            .WaitAsync(TimeSpan.FromSeconds(10));
        Require(readyLine?.StartsWith("OPENUTAU_VST_PORT=", StringComparison.Ordinal) == true,
            $"Engine host did not report readiness: {readyLine}");
        var port = int.Parse(readyLine!["OPENUTAU_VST_PORT=".Length..]);
        using (var client = new TcpClient { NoDelay = true }) {
            await client.ConnectAsync("127.0.0.1", port);
            var stream = client.GetStream();
            await FramedStream.WriteAsync(stream,
                new ControlRequest(1, ControlKinds.Hello, token));
            var hello = await FramedStream.ReadAsync<ControlResponse>(stream);
            Require(hello is { RequestId: 1, Kind: ControlKinds.Ok }, "Sidecar handshake failed.");

            await FramedStream.WriteAsync(stream,
                new ControlRequest(2, ControlKinds.GetState));
            var state = await FramedStream.ReadAsync<ControlResponse>(stream);
            Require(state is { RequestId: 2, Kind: ControlKinds.Ok }, "Sidecar state request failed.");
            Require(Convert.FromBase64String(state!.Payload!).Length > 0, "Sidecar state was empty.");

            var timing = new HostTiming(8.0, 160.0, 2, 3, 4);
            await FramedStream.WriteAsync(stream, new ControlRequest(
                3, ControlKinds.SetHostTiming,
                JsonSerializer.Serialize(timing, ProtocolJsonContext.Default.HostTiming)));
            var timingResponse = await FramedStream.ReadAsync<ControlResponse>(stream);
            Require(timingResponse is { RequestId: 3, Kind: ControlKinds.Ok },
                "Sidecar host timing request failed.");

            var request = new RenderRequest(1, 0, 64, 48000, false);
            await FramedStream.WriteAsync(stream, new ControlRequest(
                4, ControlKinds.Render,
                JsonSerializer.Serialize(request, ProtocolJsonContext.Default.RenderRequest)));
            var render = await FramedStream.ReadAsync<ControlResponse>(stream);
            Require(render is { RequestId: 4, Kind: ControlKinds.Ok }, "Sidecar render failed.");
            Require(Convert.FromBase64String(render!.Payload!).Length == 64 * 2 * sizeof(float),
                "Sidecar returned the wrong audio size.");
        }
        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        Require(process.ExitCode == 0,
            $"Engine host exited with {process.ExitCode}: {await process.StandardError.ReadToEndAsync()}");
    } finally {
        if (!process.HasExited) process.Kill(entireProcessTree: true);
    }
}

sealed class DuplexTestStream : Stream {
    private readonly Stream input;
    private readonly Stream output;
    public DuplexTestStream(Stream input, Stream output) { this.input = input; this.output = output; }
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => true;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() => output.Flush();
    public override Task FlushAsync(CancellationToken token) => output.FlushAsync(token);
    public override int Read(byte[] buffer, int offset, int count) => input.Read(buffer, offset, count);
    public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken token = default) => input.ReadAsync(buffer, token);
    public override void Write(byte[] buffer, int offset, int count) => output.Write(buffer, offset, count);
    public override ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken token = default) => output.WriteAsync(buffer, token);
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
}
