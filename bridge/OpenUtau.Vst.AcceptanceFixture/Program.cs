using OpenUtau.Api;
using OpenUtau.Core;
using OpenUtau.Core.Render;
using OpenUtau.Core.Ustx;
using OpenUtau.Plugin.Builtin;
using OpenUtau.Vst.Engine;
using System.Text;

if (args.Length != 1) {
    Console.Error.WriteLine("Usage: OpenUtau.Vst.AcceptanceFixture <output-dir>");
    return 2;
}

var output = Path.GetFullPath(args[0]);
Directory.CreateDirectory(output);
Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
Environment.SetEnvironmentVariable("OPENUTAU_VST_SESSION", "1");
SingerManager.Inst.Initialize();
const int preRollTicks = 1920; // One complete 4/4 bar at 480 PPQ.

var tetoJapanese = GetSinger("重音テト OU用日本語統合ライブラリー");
var tetoEnglish = GetSinger("重音テト音声ライブラリー");
var adachiRei = GetSinger("足立レイver3.5.0");

var ensemble = new ProjectDocument();
ensemble.Edit(project => {
    project.name = "OpenUtau DAW multilingual ensemble acceptance";
    project.comment =
        "DAW-state fixture: Teto Japanese, Teto English, and Adachi Rei; " +
        "leave a full bar of pre-roll for voicebank pre-utterance and phrase attacks.";
    project.tempos[0].bpm = 140;

    ConfigureTrack(
        project.tracks[0], "Teto JA lead", tetoJapanese,
        CreatePhonemizer<JapanesePresampPhonemizer>());
    project.tracks.Add(NewTrack(
        "Teto EN response", tetoEnglish, CreatePhonemizer<EnXSampaPhonemizer>()));
    project.tracks.Add(NewTrack(
        "Adachi Rei harmony", adachiRei,
        CreatePhonemizer<JapanesePresampPhonemizer>()));

    AddPart(project, 0, "Teto Japanese melody", new[] {
        new NoteSpec(preRollTicks, 420, 60, "は"),
        new NoteSpec(preRollTicks + 480, 420, 64, "い"),
        new NoteSpec(preRollTicks + 960, 420, 67, "う"),
        new NoteSpec(preRollTicks + 1440, 900, 69, "た"),
        new NoteSpec(preRollTicks + 2880, 420, 72, "テ"),
        new NoteSpec(preRollTicks + 3360, 420, 71, "ト"),
        new NoteSpec(preRollTicks + 3840, 420, 67, "と"),
        new NoteSpec(preRollTicks + 4320, 900, 64, "レ"),
    });
    AddPart(project, 1, "Teto English response", new[] {
        // "hi" is a verified entry in this specific Teto English bank. Keep
        // the acceptance fixture deterministic instead of relying on a host's
        // optional English dictionary coverage for arbitrary words.
        new NoteSpec(preRollTicks + 1920, 420, 60, "hi"),
        new NoteSpec(preRollTicks + 2880, 900, 64, "hi"),
        new NoteSpec(preRollTicks + 4800, 420, 62, "hi"),
        new NoteSpec(preRollTicks + 5280, 900, 67, "hi"),
    });
    AddPart(project, 2, "Adachi Rei counter melody", new[] {
        new NoteSpec(preRollTicks, 900, 55, "あ"),
        new NoteSpec(preRollTicks + 960, 900, 57, "し"),
        new NoteSpec(preRollTicks + 1920, 900, 60, "た"),
        new NoteSpec(preRollTicks + 2880, 900, 62, "へ"),
        new NoteSpec(preRollTicks + 3840, 420, 64, "う"),
        new NoteSpec(preRollTicks + 4320, 420, 62, "た"),
        new NoteSpec(preRollTicks + 4800, 420, 60, "お"),
        new NoteSpec(preRollTicks + 5280, 900, 67, "う"),
    });
});

var alternate = new ProjectDocument();
alternate.Edit(project => {
    project.name = "OpenUtau DAW second-instance acceptance";
    project.comment = "Independent VST instance used to detect state or audio crosstalk.";
    project.tempos[0].bpm = 140;
    ConfigureTrack(
        project.tracks[0], "Adachi Rei second instance", adachiRei,
        CreatePhonemizer<JapanesePresampPhonemizer>());
    AddPart(project, 0, "Second-instance answer", new[] {
        new NoteSpec(7200, 420, 72, "レ"),
        new NoteSpec(7680, 420, 74, "イ"),
        new NoteSpec(8160, 900, 76, "で"),
        new NoteSpec(9120, 900, 72, "す"),
    });
});

var ensemblePath = Path.Combine(output, "multilingual-ensemble.ustx");
var alternatePath = Path.Combine(output, "second-instance.ustx");
File.WriteAllBytes(ensemblePath, ensemble.SaveUstx());
File.WriteAllBytes(alternatePath, alternate.SaveUstx());

RenderRoundTrip(ensemblePath, Path.Combine(output, "multilingual-ensemble-reference.wav"), 12);
RenderSoloTrack(ensemblePath, Path.Combine(output, "teto-japanese-solo.wav"), 0, 12);
RenderSoloTrack(ensemblePath, Path.Combine(output, "teto-english-solo.wav"), 1, 12);
RenderSoloTrack(ensemblePath, Path.Combine(output, "adachi-rei-solo.wav"), 2, 12);
RenderRoundTrip(alternatePath, Path.Combine(output, "second-instance-reference.wav"), 12);

Console.WriteLine($"Created {ensemblePath}");
Console.WriteLine($"Created {alternatePath}");
return 0;

static USinger GetSinger(string id) {
    if (!SingerManager.Inst.Singers.TryGetValue(id, out var singer)) {
        var available = string.Join(", ", SingerManager.Inst.Singers.Keys.OrderBy(key => key));
        throw new InvalidOperationException(
            $"Singer '{id}' is unavailable. Installed singers: {available}");
    }
    singer.EnsureLoaded();
    if (!singer.Loaded || singer.Otos.Count == 0) {
        throw new InvalidOperationException($"Singer '{id}' did not load any aliases.");
    }
    Console.WriteLine($"Loaded singer '{id}' with {singer.Otos.Count} aliases.");
    return singer;
}

static Phonemizer CreatePhonemizer<T>() where T : Phonemizer {
    return PhonemizerFactory.Get(typeof(T))?.Create()
        ?? throw new InvalidOperationException($"Phonemizer {typeof(T).FullName} is unavailable.");
}

static UTrack NewTrack(
        string name, USinger singer, Phonemizer phonemizer) {
    var track = new UTrack(name);
    ConfigureTrack(track, name, singer, phonemizer);
    return track;
}

static void ConfigureTrack(
        UTrack track, string name, USinger singer, Phonemizer phonemizer) {
    track.TrackName = name;
    track.Singer = singer;
    track.Phonemizer = phonemizer;
    track.RendererSettings.renderer = Renderers.WORLDLINE_R;
}

static void AddPart(UProject project, int trackNo, string name, IEnumerable<NoteSpec> specs) {
    var notes = specs.ToArray();
    var part = new UVoicePart {
        name = name,
        trackNo = trackNo,
        position = 0,
        duration = notes.Max(note => note.Position + note.Duration) + project.resolution,
    };
    foreach (var spec in notes) {
        var note = project.CreateNote(spec.Tone, spec.Position, spec.Duration);
        note.lyric = spec.Lyric;
        part.notes.Add(note);
    }
    project.parts.Add(part);
}

static void RenderRoundTrip(string ustxPath, string wavPath, int seconds) {
    var restored = new ProjectDocument();
    restored.LoadUstx(File.ReadAllBytes(ustxPath));
    var sampleRate = 48000;
    var samples = new ProjectRenderer(restored).Render(0, sampleRate * seconds, sampleRate);
    if (!samples.Any(sample => Math.Abs(sample) > 1e-5f)) {
        throw new InvalidOperationException($"Rendered fixture is silent: {ustxPath}");
    }
    WriteFloatWav(wavPath, samples, sampleRate);
}

static void RenderSoloTrack(
        string ustxPath, string wavPath, int trackIndex, int seconds) {
    var restored = new ProjectDocument();
    restored.LoadUstx(File.ReadAllBytes(ustxPath));
    restored.Edit(project => {
        var targetTrack = project.tracks[trackIndex];
        project.parts.RemoveAll(part => part.trackNo != trackIndex);
        foreach (var part in project.parts) {
            part.trackNo = 0;
        }
        targetTrack.TrackNo = 0;
        targetTrack.Mute = false;
        targetTrack.Muted = false;
        project.tracks = new List<UTrack> { targetTrack };
    });
    restored.Read(project => {
        var voiceParts = project.parts.OfType<UVoicePart>().ToArray();
        var phones = voiceParts.SelectMany(part => part.renderPhrases)
            .SelectMany(phrase => phrase.phones).ToArray();
        Console.WriteLine(
            $"Solo track {trackIndex}: {phones.Length} render phones; notes: " +
            string.Join(", ", voiceParts.SelectMany(part => part.notes)
                .Select(note => $"{note.lyric} error={note.Error}")));
        return true;
    });
    var sampleRate = 48000;
    var samples = new ProjectRenderer(restored).Render(0, sampleRate * seconds, sampleRate);
    var peak = samples.Select(sample => Math.Abs(sample)).DefaultIfEmpty().Max();
    Console.WriteLine($"Solo track {trackIndex} peak: {peak:0.000000}");
    WriteFloatWav(wavPath, samples, sampleRate);
}

static void WriteFloatWav(string path, float[] samples, int sampleRate) {
    using var stream = File.Create(path);
    using var writer = new BinaryWriter(stream);
    var dataBytes = checked(samples.Length * sizeof(float));
    writer.Write("RIFF"u8);
    writer.Write(36 + dataBytes);
    writer.Write("WAVEfmt "u8);
    writer.Write(16);
    writer.Write((short)3);
    writer.Write((short)2);
    writer.Write(sampleRate);
    writer.Write(sampleRate * 2 * sizeof(float));
    writer.Write((short)(2 * sizeof(float)));
    writer.Write((short)32);
    writer.Write("data"u8);
    writer.Write(dataBytes);
    foreach (var sample in samples) writer.Write(sample);
}

internal readonly record struct NoteSpec(int Position, int Duration, int Tone, string Lyric);
