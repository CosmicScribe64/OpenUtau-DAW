using OpenUtau.Core.Render;

namespace OpenUtau.Vst.Engine;

/// <summary>Renders bounded DAW sample ranges from an OpenUtau project.</summary>
public sealed class ProjectRenderer {
    private const double CacheSeconds = 4.0;
    private readonly ProjectDocument document;
    private long preparedRevision = -1;
    private long cachedRevision = -1;
    private long cachedStartSample;
    private int cachedFrameCount;
    private double cachedSampleRate;
    private float[] cachedAudio = Array.Empty<float>();

    public ProjectRenderer(ProjectDocument document) {
        this.document = document ?? throw new ArgumentNullException(nameof(document));
    }

    public float[] Render(long startSample, int frameCount, double hostSampleRate,
                          CancellationToken cancellationToken = default) {
        if (startSample < 0) throw new ArgumentOutOfRangeException(nameof(startSample));
        if (frameCount < 0) throw new ArgumentOutOfRangeException(nameof(frameCount));
        if (!double.IsFinite(hostSampleRate) || hostSampleRate < 8000 || hostSampleRate > 384000) {
            throw new ArgumentOutOfRangeException(nameof(hostSampleRate));
        }
        if (frameCount == 0) return Array.Empty<float>();

        return document.Read(project => {
            var revision = document.Revision;
            if (preparedRevision != revision) {
                VstRenderAdapter.PrepareProject(project);
                preparedRevision = revision;
            }
            var requestEnd = checked(startSample + frameCount);
            var cacheEnd = checked(cachedStartSample + cachedFrameCount);
            if (cachedRevision != revision
                || Math.Abs(cachedSampleRate - hostSampleRate) > 0.001
                || startSample < cachedStartSample
                || requestEnd > cacheEnd) {
                cachedStartSample = startSample;
                cachedFrameCount = Math.Max(
                    frameCount, checked((int)Math.Ceiling(hostSampleRate * CacheSeconds)));
                cachedSampleRate = hostSampleRate;
                cachedAudio = RenderUncached(
                    project, cachedStartSample, cachedFrameCount,
                    hostSampleRate, cancellationToken);
                cachedRevision = revision;
            }
            var result = new float[checked(frameCount * VstRenderAdapter.Channels)];
            var cacheOffset = checked((int)(startSample - cachedStartSample));
            Array.Copy(
                cachedAudio, checked(cacheOffset * VstRenderAdapter.Channels),
                result, 0, result.Length);
            return result;
        });
    }

    private static float[] RenderUncached(
            OpenUtau.Core.Ustx.UProject project, long startSample, int frameCount,
            double hostSampleRate, CancellationToken cancellationToken) {
            var ratio = VstRenderAdapter.SampleRate / hostSampleRate;
            var sourceStartExact = startSample * ratio;
            var sourceStart = (long)Math.Floor(sourceStartExact);
            var sourceEndExact = (startSample + frameCount - 1) * ratio;
            var sourceFrames = checked((int)(Math.Ceiling(sourceEndExact) - sourceStart + 2));
            var source = VstRenderAdapter.RenderFrames(
                project, sourceStart, sourceFrames, cancellationToken,
                prepareProject: false);

            var output = new float[checked(frameCount * VstRenderAdapter.Channels)];
            for (var frame = 0; frame < frameCount; frame++) {
                var position = (startSample + frame) * ratio - sourceStart;
                var left = (int)Math.Floor(position);
                var fraction = (float)(position - left);
                for (var channel = 0; channel < VstRenderAdapter.Channels; channel++) {
                    var a = source[left * 2 + channel];
                    var b = source[(left + 1) * 2 + channel];
                    output[frame * 2 + channel] = a + (b - a) * fraction;
                }
            }
            return output;
    }
}
