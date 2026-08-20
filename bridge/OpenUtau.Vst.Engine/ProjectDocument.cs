using System.Text;
using OpenUtau.Core;
using OpenUtau.Core.Format;
using OpenUtau.Core.Ustx;
using OpenUtau.Core.Util;
using OpenUtau.Vst.Protocol;
using OpenUtau.Classic;

namespace OpenUtau.Vst.Engine;

/// <summary>
/// Owns the canonical OpenUtau project for one plugin instance. All mutations
/// and snapshots are serialized so a DAW state request cannot race the editor.
/// </summary>
public sealed class ProjectDocument {
    public const int MaximumProjectBytes = 256 * 1024 * 1024;
    private readonly object sync = new();
    private UProject project;
    private long revision;

    static ProjectDocument() {
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        Directory.CreateDirectory(PathManager.Inst.CachePath);
        ToolsManager.Inst.Initialize();
        SingerManager.Inst.Initialize();
        DocManager.Inst.SearchAllPlugins();
        DocManager.Inst.SearchAllLegacyPlugins();
    }

    public ProjectDocument() : this(Ustx.Create()) { }

    public ProjectDocument(UProject project) {
        this.project = project ?? throw new ArgumentNullException(nameof(project));
    }

    public long Revision {
        get { lock (sync) return revision; }
    }

    public TResult Read<TResult>(Func<UProject, TResult> reader) {
        ArgumentNullException.ThrowIfNull(reader);
        lock (sync) return reader(project);
    }

    public void Edit(Action<UProject> edit) {
        ArgumentNullException.ThrowIfNull(edit);
        lock (sync) {
            edit(project);
            project.ValidateFull();
            revision++;
        }
    }

    public void SynchronizeHostTiming(HostTiming timing) {
        if (!double.IsFinite(timing.QuarterNotePosition) || timing.QuarterNotePosition < 0
            || !double.IsFinite(timing.Tempo) || timing.Tempo < 10 || timing.Tempo > 1000) {
            throw new InvalidDataException("Host musical position or tempo is invalid.");
        }
        if (timing.TimeSignatureNumerator is < 1 or > 64
            || timing.TimeSignatureDenominator is < 1 or > 64
            || (timing.TimeSignatureDenominator & (timing.TimeSignatureDenominator - 1)) != 0
            || timing.BarPosition is < -1 or > int.MaxValue) {
            throw new InvalidDataException("Host time signature is invalid.");
        }
        lock (sync) {
            var tick = checked((int)Math.Round(
                timing.QuarterNotePosition * project.resolution,
                MidpointRounding.AwayFromZero));
            var preceding = project.tempos
                .Where(item => item.position <= tick)
                .OrderBy(item => item.position)
                .LastOrDefault();
            var atTick = project.tempos.FirstOrDefault(item => item.position == tick);
            var changed = false;
            if (atTick is not null) {
                if (Math.Abs(atTick.bpm - timing.Tempo) > 1e-9) {
                    atTick.bpm = timing.Tempo;
                    changed = true;
                }
            } else if (preceding is null || Math.Abs(preceding.bpm - timing.Tempo) > 1e-9) {
                project.tempos.Add(new UTempo(tick, timing.Tempo));
                changed = true;
            }
            var bar = timing.BarPosition < 0 ? 0 : checked((int)timing.BarPosition);
            var signature = project.timeSignatures.FirstOrDefault(item => item.barPosition == bar);
            if (signature is null) {
                project.timeSignatures.Add(new UTimeSignature(
                    bar, timing.TimeSignatureNumerator, timing.TimeSignatureDenominator));
                changed = true;
            } else if (signature.beatPerBar != timing.TimeSignatureNumerator
                       || signature.beatUnit != timing.TimeSignatureDenominator) {
                signature.beatPerBar = timing.TimeSignatureNumerator;
                signature.beatUnit = timing.TimeSignatureDenominator;
                changed = true;
            }
            if (changed) {
                project.ValidateFull();
                revision++;
            }
        }
    }

    public byte[] SaveUstx() {
        lock (sync) {
            project.ustxVersion = Ustx.kUstxVersion;
            project.BeforeSave();
            try {
                var bytes = Encoding.UTF8.GetBytes(Yaml.DefaultSerializer.Serialize(project));
                if (bytes.Length > MaximumProjectBytes) {
                    throw new InvalidDataException($"USTX exceeds {MaximumProjectBytes} bytes.");
                }
                return bytes;
            } finally {
                project.AfterSave();
            }
        }
    }

    public void LoadUstx(ReadOnlySpan<byte> bytes) {
        if (bytes.IsEmpty || bytes.Length > MaximumProjectBytes) {
            throw new InvalidDataException("USTX payload size is invalid.");
        }
        var text = new UTF8Encoding(false, true).GetString(bytes);
        var incoming = Yaml.DefaultDeserializer.Deserialize<UProject>(text)
            ?? throw new InvalidDataException("USTX payload did not contain a project.");
        if (incoming.ustxVersion > Ustx.kUstxVersion) {
            throw new InvalidDataException(
                $"USTX {incoming.ustxVersion} is newer than supported {Ustx.kUstxVersion}.");
        }
        Ustx.AddDefaultExpressions(incoming);
        incoming.AfterLoad();
        incoming.ValidateFull();
        incoming.ustxVersion = Ustx.kUstxVersion;
        incoming.FilePath = string.Empty;
        incoming.Saved = true;
        lock (sync) {
            project = incoming;
            revision++;
        }
    }
}
