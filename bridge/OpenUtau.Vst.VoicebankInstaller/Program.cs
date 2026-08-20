using System.Text;
using OpenUtau.Classic;

Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);

if (args.Length is < 2 or > 4) {
    Console.Error.WriteLine(
        "Usage: OpenUtau.Vst.VoicebankInstaller <archive> <singers-dir> " +
        "[archive-encoding] [text-encoding]");
    return 2;
}

var archivePath = Path.GetFullPath(args[0]);
var singersPath = Path.GetFullPath(args[1]);
var archiveEncoding = Encoding.GetEncoding(args.Length >= 3 ? args[2] : "shift_jis");
var textEncoding = Encoding.GetEncoding(args.Length >= 4 ? args[3] : "shift_jis");

if (!File.Exists(archivePath)) {
    Console.Error.WriteLine($"Voicebank archive does not exist: {archivePath}");
    return 2;
}

Directory.CreateDirectory(singersPath);
var before = Directory.GetDirectories(singersPath)
    .Select(Path.GetFullPath)
    .ToHashSet(StringComparer.Ordinal);

var installer = new VoicebankInstaller(
    singersPath,
    (progress, item) => {
        if (progress == 0 || progress >= 100 || (int)progress % 10 == 0) {
            Console.WriteLine($"{progress,6:F1}% {item}");
        }
    },
    archiveEncoding,
    textEncoding);
installer.Install(archivePath, "utau");

var installedRoots = Directory.GetDirectories(singersPath)
    .Select(Path.GetFullPath)
    .Where(path => !before.Contains(path))
    .OrderBy(path => path, StringComparer.Ordinal)
    .ToArray();

if (installedRoots.Length == 0) {
    Console.WriteLine("Installed into an existing singer directory.");
} else {
    foreach (var root in installedRoots) {
        Console.WriteLine($"Installed singer root: {root}");
    }
}

return 0;
