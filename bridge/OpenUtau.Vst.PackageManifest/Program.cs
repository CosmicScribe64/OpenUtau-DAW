using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Xml;

const string manifestName = "PACKAGE-MANIFEST.sha256";

if (!((args.Length == 2 && (args[0] == "create" || args[0] == "verify")) ||
      (args.Length >= 5 && args[0] == "notices") ||
      (args.Length == 1 && args[0] == "self-test"))) {
    Console.Error.WriteLine("Usage: OpenUtau.Vst.PackageManifest create <directory> | verify <archive.zip> | notices <nuget-cache> <overrides.json> <output-directory> <deps.json>... | self-test");
    return 2;
}

try {
    if (args[0] == "self-test") {
        return SelfTest();
    }
    if (args[0] == "create") {
        var root = Path.GetFullPath(args[1]);
        CreateManifest(root);
        return 0;
    }
    if (args[0] == "notices") {
        ThirdPartyNoticeGenerator.Create(
            Path.GetFullPath(args[1]),
            Path.GetFullPath(args[2]),
            Path.GetFullPath(args[3]),
            args.Skip(4).Select(Path.GetFullPath));
        return 0;
    }
    var archive = Path.GetFullPath(args[1]);
    VerifyArchive(archive);
    Console.WriteLine($"Verified {archive}");
    return 0;
} catch (Exception exception) when (exception is IOException or InvalidOperationException or InvalidDataException or CryptographicException or JsonException or XmlException) {
    Console.Error.WriteLine($"Package manifest error: {exception.Message}");
    return 1;
}

static void CreateManifest(string root) {
    if (!Directory.Exists(root)) throw new InvalidOperationException($"Package directory does not exist: {root}");
    var manifest = Path.Combine(root, manifestName);
    var files = Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
        .Where(path => !string.Equals(path, manifest, StringComparison.Ordinal))
        .OrderBy(path => Path.GetRelativePath(root, path), StringComparer.Ordinal)
        .ToArray();
    if (files.Length == 0) throw new InvalidOperationException("Package contains no files.");
    using var output = new StreamWriter(manifest, false, new UTF8Encoding(false));
    foreach (var path in files) {
        var relative = Path.GetRelativePath(root, path).Replace(Path.DirectorySeparatorChar, '/');
        output.WriteLine($"{HashFile(path)}  {relative}");
    }
}

static void VerifyArchive(string archive) {
    if (!File.Exists(archive)) throw new InvalidOperationException($"Archive does not exist: {archive}");
    using var zip = ZipFile.OpenRead(archive);
    var filesInZip = zip.Entries.Where(entry => !entry.FullName.EndsWith('/')).ToArray();
    var manifests = filesInZip.Where(entry => entry.FullName.EndsWith(manifestName, StringComparison.Ordinal)).ToArray();
    if (manifests.Length != 1) throw new InvalidOperationException("Archive must contain exactly one package manifest.");
    var manifestEntry = manifests[0];
    var prefix = manifestEntry.FullName[..^manifestName.Length];
    if (prefix.Length > 0 && !prefix.EndsWith('/')) throw new InvalidOperationException("Manifest has an invalid archive path.");
    var expected = ParseManifest(ReadAll(manifestEntry));
    var actual = new Dictionary<string, string>(StringComparer.Ordinal);
    foreach (var entry in filesInZip.Where(entry => entry != manifestEntry)) {
        if (!entry.FullName.StartsWith(prefix, StringComparison.Ordinal)) throw new InvalidOperationException($"Archive file outside package root: {entry.FullName}");
        var relative = entry.FullName[prefix.Length..];
        if (relative.Length == 0 || !actual.TryAdd(relative, HashEntry(entry))) throw new InvalidOperationException($"Duplicate or invalid archive path: {entry.FullName}");
    }
    if (!expected.Keys.ToHashSet(StringComparer.Ordinal).SetEquals(actual.Keys)) throw new InvalidOperationException("Manifest file set differs from archive.");
    foreach (var (path, checksum) in expected) {
        if (!string.Equals(checksum, actual[path], StringComparison.Ordinal)) throw new InvalidOperationException($"Checksum mismatch: {path}");
    }
}

static int SelfTest() {
    var root = Path.Combine(Path.GetTempPath(), "openutau-package-manifest-" + Guid.NewGuid().ToString("N"));
    try {
        Directory.CreateDirectory(Path.Combine(root, "package", "Contents", "Resources"));
        File.WriteAllText(Path.Combine(root, "package", "Contents", "module"), "native module\n");
        File.WriteAllText(Path.Combine(root, "package", "Contents", "Resources", "engine.dll"), "managed sidecar\n");
        var package = Path.Combine(root, "package");
        CreateManifest(package);
        var archive = Path.Combine(root, "package.zip");
        ZipFile.CreateFromDirectory(package, archive, CompressionLevel.NoCompression, includeBaseDirectory: true);
        VerifyArchive(archive);
        using (var zip = ZipFile.Open(archive, ZipArchiveMode.Update)) {
            zip.CreateEntry("package/Contents/module").Open().Dispose();
        }
        try {
            VerifyArchive(archive);
            throw new InvalidOperationException("Tampered package unexpectedly verified.");
        } catch (InvalidOperationException exception) when (exception.Message != "Tampered package unexpectedly verified.") {
            Console.WriteLine("Package manifest tamper detection passed.");
        }
        ThirdPartyNoticeGenerator.SelfTest(root);
        return 0;
    } finally {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static string HashFile(string path) {
    using var input = File.OpenRead(path);
    return Convert.ToHexString(SHA256.HashData(input)).ToLowerInvariant();
}

static string HashEntry(ZipArchiveEntry entry) {
    using var input = entry.Open();
    return Convert.ToHexString(SHA256.HashData(input)).ToLowerInvariant();
}

static string ReadAll(ZipArchiveEntry entry) {
    using var reader = new StreamReader(entry.Open(), Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
    return reader.ReadToEnd();
}

static Dictionary<string, string> ParseManifest(string text) {
    var output = new Dictionary<string, string>(StringComparer.Ordinal);
    foreach (var line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries)) {
        var separator = line.IndexOf("  ", StringComparison.Ordinal);
        if (separator != 64) throw new InvalidOperationException("Manifest has a malformed line.");
        var checksum = line[..separator];
        var path = line[(separator + 2)..].TrimEnd('\r');
        if (!checksum.All(Uri.IsHexDigit) || checksum != checksum.ToLowerInvariant() ||
            path.Length == 0 || path.StartsWith('/') || path.Split('/').Contains("..") || !output.TryAdd(path, checksum)) {
            throw new InvalidOperationException("Manifest has an invalid entry.");
        }
    }
    if (output.Count == 0) throw new InvalidOperationException("Manifest is empty.");
    return output;
}
