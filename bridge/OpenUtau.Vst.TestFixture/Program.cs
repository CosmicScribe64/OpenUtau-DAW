using System.Text;
using OpenUtau.Classic;
using OpenUtau.Core.Render;
using OpenUtau.Core.Ustx;
using OpenUtau.Vst.Engine;

Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);

if (args.Length != 1) throw new ArgumentException("Expected fixture output directory.");
var output = Path.GetFullPath(args[0]);
var dataHome = Path.Combine(output, "data");
var singerRoot = Path.Combine(dataHome, "OpenUtau", "Singers");
var voiceDirectory = Path.Combine(singerRoot, "VSTFixture");
Directory.CreateDirectory(voiceDirectory);
Directory.CreateDirectory(Path.Combine(output, "cache"));

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
if (!singer.Loaded || !singer.TryGetMappedOto("a", 60, "", out _)) {
    throw new InvalidOperationException("Generated voicebank did not load.");
}

var document = new ProjectDocument();
document.Edit(project => {
    project.name = "VST audible E2E fixture";
    project.tracks[0].Singer = singer;
    project.tracks[0].RendererSettings.renderer = Renderers.WORLDLINE_R;
    var part = new UVoicePart {
        name = "Audible fixture",
        trackNo = 0,
        position = 0,
        duration = 1920,
    };
    var note = project.CreateNote(60, 0, 960);
    note.lyric = "a";
    part.notes.Add(note);
    project.parts.Add(part);
});
File.WriteAllBytes(Path.Combine(output, "project.ustx"), document.SaveUstx());

// A deliberately different project is used by the loaded-VST multi-instance
// E2E. It begins later and at another pitch, so state leakage cannot pass as a
// coincidentally identical waveform.
var alternateDocument = new ProjectDocument();
alternateDocument.Edit(project => {
    project.name = "VST alternate instance fixture";
    project.tracks[0].Singer = singer;
    project.tracks[0].RendererSettings.renderer = Renderers.WORLDLINE_R;
    var part = new UVoicePart {
        name = "Alternate fixture",
        trackNo = 0,
        position = 480,
        duration = 1920,
    };
    var note = project.CreateNote(67, 0, 960);
    note.lyric = "a";
    part.notes.Add(note);
    project.parts.Add(part);
});
File.WriteAllBytes(
    Path.Combine(output, "project-alternate.ustx"),
    alternateDocument.SaveUstx());
Console.WriteLine(output);

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
