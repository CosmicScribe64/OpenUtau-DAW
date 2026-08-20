using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Xml.Linq;

internal static partial class ThirdPartyNoticeGenerator {
    private const string NoticeFileName = "THIRD-PARTY-NOTICES.md";
    private const string LicenseDirectoryName = "THIRD-PARTY-LICENSES";

    internal static void Create(
            string nugetCache,
            string overrideManifest,
            string outputDirectory,
            IEnumerable<string> dependencyManifests) {
        if (!Directory.Exists(nugetCache)) {
            throw new InvalidOperationException($"NuGet package cache does not exist: {nugetCache}");
        }
        Directory.CreateDirectory(outputDirectory);
        var overrides = ReadOverrides(overrideManifest);
        var packages = new Dictionary<string, PackageReference>(StringComparer.OrdinalIgnoreCase);
        foreach (var manifest in dependencyManifests) {
            ReadRuntimePackages(manifest, packages);
        }
        if (packages.Count == 0) {
            throw new InvalidOperationException("Dependency manifests contain no runtime NuGet packages.");
        }

        var licenseRoot = Path.Combine(outputDirectory, LicenseDirectoryName);
        if (Directory.Exists(licenseRoot)) {
            Directory.Delete(licenseRoot, recursive: true);
        }
        Directory.CreateDirectory(licenseRoot);
        var records = packages.Values
            .OrderBy(package => package.Id, StringComparer.OrdinalIgnoreCase)
            .ThenBy(package => package.Version, StringComparer.OrdinalIgnoreCase)
            .Select(package => ReadPackageNotice(nugetCache, licenseRoot, package, overrides))
            .ToArray();
        var missing = records.Where(record => string.IsNullOrWhiteSpace(record.License)).ToArray();
        if (missing.Length > 0) {
            throw new InvalidOperationException(
                "Runtime packages lack licence metadata: " +
                string.Join(", ", missing.Select(record => $"{record.Id}/{record.Version}")));
        }

        var noticePath = Path.Combine(outputDirectory, NoticeFileName);
        using var output = new StreamWriter(noticePath, false, new UTF8Encoding(false));
        output.WriteLine("# Third-party package notices");
        output.WriteLine();
        output.WriteLine("This inventory is generated from the RID-specific .NET dependency manifests used by this package.");
        output.WriteLine("It lists NuGet packages that contribute runtime, native, or resource assets; build-only packages are excluded.");
        output.WriteLine("Licence and notice files shipped inside those packages are copied under `THIRD-PARTY-LICENSES/`.");
        output.WriteLine("The private .NET runtime's own `LICENSE.txt` and `ThirdPartyNotices.txt` are shipped inside the VST3 resources.");
        output.WriteLine();
        output.WriteLine("This generated inventory supplements the OpenUtau, JUCE, VST3 SDK, AGPLv3, and distribution notices beside the VST3.");
        output.WriteLine();
        output.WriteLine("| Package | Version | Licence metadata | Project/source | Included notice files |");
        output.WriteLine("| --- | --- | --- | --- | --- |");
        foreach (var record in records) {
            var project = string.IsNullOrWhiteSpace(record.ProjectUrl) ? "—" : $"[link]({EscapeUrl(record.ProjectUrl)})";
            var files = record.NoticeFiles.Count == 0
                ? "—"
                : string.Join("<br>", record.NoticeFiles.Select(path => $"`{EscapeCell(path)}`"));
            output.WriteLine($"| {EscapeCell(record.Id)} | {EscapeCell(record.Version)} | {EscapeCell(record.License)} | {project} | {files} |");
        }
        Console.WriteLine($"Wrote {records.Length} runtime package notices to {noticePath}");
    }

    internal static void SelfTest(string testRoot) {
        var nuget = Path.Combine(testRoot, "nuget");
        var package = Path.Combine(nuget, "example.runtime", "1.2.3");
        Directory.CreateDirectory(package);
        File.WriteAllText(Path.Combine(package, "example.runtime.nuspec"), """
            <?xml version="1.0"?>
            <package><metadata>
              <id>Example.Runtime</id><version>1.2.3</version>
              <license type="expression">MIT</license>
              <projectUrl>https://example.invalid/runtime</projectUrl>
            </metadata></package>
            """);
        File.WriteAllText(Path.Combine(package, "LICENSE.txt"), "Example licence\n");
        var deps = Path.Combine(testRoot, "fixture.deps.json");
        File.WriteAllText(deps, """
            {
              "runtimeTarget": { "name": ".NETCoreApp,Version=v8.0/test-rid" },
              "targets": {
                ".NETCoreApp,Version=v8.0/test-rid": {
                  "Example.Runtime/1.2.3": { "runtime": { "lib/net8.0/Example.Runtime.dll": {} } },
                  "Build.Only/9.9.9": {}
                }
              },
              "libraries": {
                "Example.Runtime/1.2.3": { "type": "package", "path": "example.runtime/1.2.3" },
                "Build.Only/9.9.9": { "type": "package", "path": "build.only/9.9.9" }
              }
            }
            """);
        var overrides = Path.Combine(testRoot, "overrides.json");
        File.WriteAllText(overrides, "{ \"schemaVersion\": 1, \"packages\": [] }");
        var output = Path.Combine(testRoot, "notices");
        Create(nuget, overrides, output, new[] { deps });
        var notice = File.ReadAllText(Path.Combine(output, NoticeFileName));
        if (!notice.Contains("Example.Runtime", StringComparison.Ordinal) ||
            notice.Contains("Build.Only", StringComparison.Ordinal) ||
            !File.Exists(Path.Combine(output, LicenseDirectoryName, "Example.Runtime", "1.2.3", "LICENSE.txt"))) {
            throw new InvalidOperationException("Third-party notice self-test failed.");
        }
        Console.WriteLine("Third-party runtime notice generation passed.");
    }

    private static void ReadRuntimePackages(string manifest, IDictionary<string, PackageReference> packages) {
        if (!File.Exists(manifest)) {
            throw new InvalidOperationException($"Dependency manifest does not exist: {manifest}");
        }
        using var document = JsonDocument.Parse(File.ReadAllBytes(manifest));
        var root = document.RootElement;
        var runtimeTarget = root.GetProperty("runtimeTarget").GetProperty("name").GetString();
        if (string.IsNullOrWhiteSpace(runtimeTarget) ||
            !root.GetProperty("targets").TryGetProperty(runtimeTarget, out var target)) {
            throw new InvalidOperationException($"Dependency manifest has no selected runtime target: {manifest}");
        }
        var libraries = root.GetProperty("libraries");
        foreach (var targetLibrary in target.EnumerateObject()) {
            if (!ContributesRuntimeAssets(targetLibrary.Value) ||
                !libraries.TryGetProperty(targetLibrary.Name, out var metadata) ||
                !metadata.TryGetProperty("type", out var type) ||
                !string.Equals(type.GetString(), "package", StringComparison.OrdinalIgnoreCase) ||
                !metadata.TryGetProperty("path", out var pathElement)) {
                continue;
            }
            var separator = targetLibrary.Name.LastIndexOf('/');
            var path = pathElement.GetString();
            if (separator <= 0 || separator == targetLibrary.Name.Length - 1 || string.IsNullOrWhiteSpace(path)) {
                throw new InvalidOperationException($"Invalid package identity in dependency manifest: {targetLibrary.Name}");
            }
            var package = new PackageReference(
                targetLibrary.Name[..separator], targetLibrary.Name[(separator + 1)..], path);
            packages[$"{package.Id}/{package.Version}"] = package;
        }
    }

    private static bool ContributesRuntimeAssets(JsonElement targetLibrary) {
        foreach (var group in new[] { "runtime", "native", "resources", "runtimeTargets" }) {
            if (!targetLibrary.TryGetProperty(group, out var assets) || assets.ValueKind != JsonValueKind.Object) {
                continue;
            }
            if (assets.EnumerateObject().Any(asset => !asset.Name.EndsWith("/_._", StringComparison.Ordinal))) {
                return true;
            }
        }
        return false;
    }

    private static PackageNotice ReadPackageNotice(
            string nugetCache,
            string licenseRoot,
            PackageReference package,
            IReadOnlyDictionary<string, LicenseOverride> overrides) {
        var packageRoot = SafeChild(nugetCache, package.CachePath);
        if (!Directory.Exists(packageRoot)) {
            throw new InvalidOperationException($"Runtime package is absent from the NuGet cache: {package.Id}/{package.Version}");
        }
        var nuspec = Directory.EnumerateFiles(packageRoot, "*.nuspec", SearchOption.TopDirectoryOnly).SingleOrDefault();
        if (nuspec is null) {
            throw new InvalidOperationException($"Runtime package has no nuspec: {package.Id}/{package.Version}");
        }
        var metadata = XDocument.Load(nuspec).Descendants().FirstOrDefault(element => element.Name.LocalName == "metadata")
            ?? throw new InvalidOperationException($"Runtime package has malformed nuspec metadata: {package.Id}/{package.Version}");
        var declaredId = Value(metadata, "id") ?? package.Id;
        var declaredVersion = Value(metadata, "version") ?? package.Version;
        var licenseElement = metadata.Elements().FirstOrDefault(element => element.Name.LocalName == "license");
        var licenseType = licenseElement?.Attribute("type")?.Value;
        var licenseValue = licenseElement?.Value.Trim();
        string? license = null;
        if (!string.IsNullOrWhiteSpace(licenseValue)) {
            license = string.Equals(licenseType, "file", StringComparison.OrdinalIgnoreCase)
                ? $"file: {licenseValue}"
                : licenseValue;
        } else {
            var legacyUrl = Value(metadata, "licenseUrl");
            if (!string.IsNullOrWhiteSpace(legacyUrl) &&
                !string.Equals(legacyUrl, "https://aka.ms/deprecateLicenseUrl", StringComparison.OrdinalIgnoreCase)) {
                license = $"legacy URL: {legacyUrl}";
            }
        }

        overrides.TryGetValue($"{package.Id}/{package.Version}", out var licenseOverride);
        if (string.IsNullOrWhiteSpace(license) && licenseOverride is not null) {
            license = $"reviewed override: {licenseOverride.License}";
        }
        var destinationRoot = Path.Combine(licenseRoot, SafeName(declaredId), SafeName(declaredVersion));
        var candidates = Directory.EnumerateFiles(packageRoot, "*", SearchOption.AllDirectories)
            .Where(path => IsNoticeFile(Path.GetFileName(path)))
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (string.Equals(licenseType, "file", StringComparison.OrdinalIgnoreCase) && !string.IsNullOrWhiteSpace(licenseValue)) {
            var declaredLicense = SafeChild(packageRoot, licenseValue.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(declaredLicense)) {
                throw new InvalidOperationException($"Declared licence file is missing for {package.Id}/{package.Version}: {licenseValue}");
            }
            candidates.Add(declaredLicense);
        }
        if (licenseOverride is not null) {
            candidates.Add(licenseOverride.LicenseFile);
        }
        var copied = new List<string>();
        var fullPackageRoot = Path.GetFullPath(packageRoot) + Path.DirectorySeparatorChar;
        foreach (var source in candidates.OrderBy(path => path, StringComparer.OrdinalIgnoreCase)) {
            var relative = Path.GetFullPath(source).StartsWith(fullPackageRoot, StringComparison.Ordinal)
                ? Path.GetRelativePath(packageRoot, source)
                : Path.GetFileName(source);
            var destination = SafeChild(destinationRoot, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(source, destination, overwrite: true);
            copied.Add(Path.Combine(LicenseDirectoryName, SafeName(declaredId), SafeName(declaredVersion), relative)
                .Replace(Path.DirectorySeparatorChar, '/'));
        }
        return new PackageNotice(
            declaredId,
            declaredVersion,
            license ?? string.Empty,
            licenseOverride?.SourceUrl ?? Value(metadata, "projectUrl"),
            copied);
    }

    private static IReadOnlyDictionary<string, LicenseOverride> ReadOverrides(string manifest) {
        if (!File.Exists(manifest)) {
            throw new InvalidOperationException($"Licence override manifest does not exist: {manifest}");
        }
        var model = JsonSerializer.Deserialize<OverrideManifest>(File.ReadAllBytes(manifest), new JsonSerializerOptions {
            PropertyNameCaseInsensitive = true,
        }) ?? throw new InvalidOperationException("Licence override manifest is empty.");
        if (model.SchemaVersion != 1 || model.Packages is null) {
            throw new InvalidOperationException("Unsupported licence override manifest schema.");
        }
        var root = Path.GetDirectoryName(manifest)!;
        var output = new Dictionary<string, LicenseOverride>(StringComparer.OrdinalIgnoreCase);
        foreach (var item in model.Packages) {
            if (string.IsNullOrWhiteSpace(item.Id) || string.IsNullOrWhiteSpace(item.Version) ||
                string.IsNullOrWhiteSpace(item.License) || string.IsNullOrWhiteSpace(item.SourceUrl) ||
                string.IsNullOrWhiteSpace(item.LicenseFile) || string.IsNullOrWhiteSpace(item.LicenseSha256)) {
                throw new InvalidOperationException("Licence override entries must specify id, version, licence, source URL, file, and SHA-256.");
            }
            var file = SafeChild(root, item.LicenseFile.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(file)) {
                throw new InvalidOperationException($"Reviewed licence file does not exist: {file}");
            }
            var actualHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file))).ToLowerInvariant();
            if (!string.Equals(actualHash, item.LicenseSha256, StringComparison.OrdinalIgnoreCase)) {
                throw new InvalidOperationException($"Reviewed licence checksum differs for {item.Id}/{item.Version}.");
            }
            if (!output.TryAdd(
                    $"{item.Id}/{item.Version}",
                    new LicenseOverride(item.License, item.SourceUrl, file))) {
                throw new InvalidOperationException($"Duplicate licence override: {item.Id}/{item.Version}");
            }
        }
        return output;
    }

    private static string? Value(XElement metadata, string name) =>
        metadata.Elements().FirstOrDefault(element => element.Name.LocalName == name)?.Value.Trim();

    private static bool IsNoticeFile(string fileName) => NoticeFilePattern().IsMatch(fileName);

    private static string SafeChild(string root, string relative) {
        var fullRoot = Path.GetFullPath(root) + Path.DirectorySeparatorChar;
        var fullPath = Path.GetFullPath(Path.Combine(root, relative));
        if (!fullPath.StartsWith(fullRoot, StringComparison.Ordinal)) {
            throw new InvalidOperationException($"Package metadata path escapes its root: {relative}");
        }
        return fullPath;
    }

    private static string SafeName(string value) => InvalidFileNameCharacters().Replace(value, "_");
    private static string EscapeCell(string value) => value.Replace("|", "\\|", StringComparison.Ordinal).Replace("\r", " ").Replace("\n", " ");
    private static string EscapeUrl(string value) => value.Replace(")", "%29", StringComparison.Ordinal);

    [GeneratedRegex("^(licen[cs]e|copying|notice|third[-_. ]?party).*$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex NoticeFilePattern();

    [GeneratedRegex("[<>:\"/\\\\|?*\\x00-\\x1F]", RegexOptions.CultureInvariant)]
    private static partial Regex InvalidFileNameCharacters();

    private sealed record PackageReference(string Id, string Version, string CachePath);
    private sealed record PackageNotice(string Id, string Version, string License, string? ProjectUrl, IReadOnlyList<string> NoticeFiles);
    private sealed record LicenseOverride(string License, string SourceUrl, string LicenseFile);
    private sealed class OverrideManifest {
        public int SchemaVersion { get; set; }
        public List<OverrideItem>? Packages { get; set; }
    }
    private sealed class OverrideItem {
        public string? Id { get; set; }
        public string? Version { get; set; }
        public string? License { get; set; }
        public string? SourceUrl { get; set; }
        public string? LicenseFile { get; set; }
        public string? LicenseSha256 { get; set; }
    }
}
