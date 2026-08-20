using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using OpenUtau.Audio;

namespace OpenUtau.Vst.EditorHost;

/// <summary>
/// Device-free OpenUtau preview output for the embedded VST editor.
/// A background producer reads OpenUtau's sample provider into a bounded SPSC
/// ring. The JUCE message thread copies that ring into a second native ring;
/// the DAW audio thread never enters managed code.
/// </summary>
public sealed class VstPreviewAudioOutput : IAudioOutput, IDisposable {
    private const int Channels = 2;
    private const int DefaultSampleRate = 44100;
    private const int CapacityFrames = 1 << 18;
    private const int ProducerFrames = 2048;

    private readonly object gate = new();
    private readonly float[] ring = new float[CapacityFrames * Channels];
    private readonly int frameMask = CapacityFrames - 1;
    private ISampleProvider? source;
    private CancellationTokenSource? producerCancellation;
    private int outputSampleRate = DefaultSampleRate;
    private int playbackState = (int)PlaybackState.Stopped;
    private long generation;
    private long writeFrame;
    private long readFrame;
    private long positionFrames;
    private int endOfStream;
    private bool disposed;

    public PlaybackState PlaybackState =>
        (PlaybackState)Volatile.Read(ref playbackState);
    public int DeviceNumber => 0;

    public void SetSampleRate(double sampleRate) {
        if (!double.IsFinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000) {
            return;
        }
        var rounded = (int)Math.Round(sampleRate);
        lock (gate) {
            if (rounded == outputSampleRate) return;
            StopLocked(clearSource: true);
            outputSampleRate = rounded;
        }
    }

    public void Init(ISampleProvider sampleProvider) {
        ArgumentNullException.ThrowIfNull(sampleProvider);
        lock (gate) {
            ThrowIfDisposed();
            StopLocked(clearSource: true);
            ISampleProvider prepared = sampleProvider.ToStereo();
            if (prepared.WaveFormat.SampleRate != outputSampleRate) {
                prepared = new WdlResamplingSampleProvider(prepared, outputSampleRate);
            }
            source = prepared;
            Volatile.Write(ref writeFrame, 0);
            Volatile.Write(ref readFrame, 0);
            Volatile.Write(ref positionFrames, 0);
            Volatile.Write(ref endOfStream, 0);
            Volatile.Write(ref playbackState, (int)PlaybackState.Stopped);
        }
    }

    public void Play() {
        lock (gate) {
            ThrowIfDisposed();
            if (source is null || PlaybackState == PlaybackState.Playing) return;
            producerCancellation?.Cancel();
            producerCancellation?.Dispose();
            producerCancellation = new CancellationTokenSource();
            var producerGeneration = Interlocked.Increment(ref generation);
            var producerSource = source;
            Volatile.Write(ref endOfStream, 0);
            Volatile.Write(ref playbackState, (int)PlaybackState.Playing);
            _ = Task.Run(() => Produce(
                producerSource, producerGeneration, producerCancellation.Token));
        }
    }

    public void Pause() {
        lock (gate) {
            if (PlaybackState != PlaybackState.Playing) return;
            Interlocked.Increment(ref generation);
            producerCancellation?.Cancel();
            Volatile.Write(ref playbackState, (int)PlaybackState.Paused);
        }
    }

    public void Stop() {
        lock (gate) {
            StopLocked(clearSource: true);
        }
    }

    public long GetPosition() {
        // PlaybackManager expects a byte-like position whose division by
        // sizeof(float) yields stereo-frame time, matching MiniAudioOutput.
        return checked(Volatile.Read(ref positionFrames) * sizeof(float));
    }

    public List<AudioOutputDevice> GetOutputDevices() => [];
    public void SelectDevice(Guid guid, int deviceNumber) { }

    /// <summary>
    /// Copies stereo frames for the native message-thread bridge. Returns -1
    /// when preview is inactive, zero while an active producer is warming, or
    /// the number of copied frames.
    /// </summary>
    public int CopyTo(Span<float> destination) {
        var capacityFrames = destination.Length / Channels;
        if (capacityFrames <= 0) {
            return PlaybackState == PlaybackState.Playing ? 0 : -1;
        }
        if (PlaybackState != PlaybackState.Playing) return -1;

        var read = Volatile.Read(ref readFrame);
        var write = Volatile.Read(ref writeFrame);
        var frames = (int)Math.Min(capacityFrames, Math.Max(0, write - read));
        for (var frame = 0; frame < frames; ++frame) {
            var sourceOffset = (int)((read + frame) & frameMask) * Channels;
            var destinationOffset = frame * Channels;
            destination[destinationOffset] = ring[sourceOffset];
            destination[destinationOffset + 1] = ring[sourceOffset + 1];
        }
        if (frames > 0) {
            Volatile.Write(ref readFrame, read + frames);
            Interlocked.Add(ref positionFrames, frames);
        } else if (Volatile.Read(ref endOfStream) != 0) {
            Volatile.Write(ref playbackState, (int)PlaybackState.Stopped);
            return -1;
        }
        return frames;
    }

    private void Produce(
            ISampleProvider producerSource,
            long producerGeneration,
            CancellationToken cancellationToken) {
        var buffer = new float[ProducerFrames * Channels];
        try {
            while (!cancellationToken.IsCancellationRequested
                    && Volatile.Read(ref generation) == producerGeneration
                    && PlaybackState == PlaybackState.Playing) {
                var write = Volatile.Read(ref writeFrame);
                var read = Volatile.Read(ref readFrame);
                var writable = CapacityFrames - (int)Math.Min(
                    CapacityFrames, Math.Max(0, write - read));
                if (writable == 0) {
                    cancellationToken.WaitHandle.WaitOne(2);
                    continue;
                }
                var requestedFrames = Math.Min(ProducerFrames, writable);
                var samples = producerSource.Read(
                    buffer, 0, requestedFrames * Channels);
                if (samples <= 0) {
                    Volatile.Write(ref endOfStream, 1);
                    return;
                }
                var frames = samples / Channels;
                if (frames == 0
                        || Volatile.Read(ref generation) != producerGeneration
                        || cancellationToken.IsCancellationRequested) {
                    return;
                }
                for (var frame = 0; frame < frames; ++frame) {
                    var destinationOffset = (int)((write + frame) & frameMask) * Channels;
                    var sourceOffset = frame * Channels;
                    ring[destinationOffset] = buffer[sourceOffset];
                    ring[destinationOffset + 1] = buffer[sourceOffset + 1];
                }
                Volatile.Write(ref writeFrame, write + frames);
                if (samples < requestedFrames * Channels) {
                    Volatile.Write(ref endOfStream, 1);
                    return;
                }
            }
        } catch (OperationCanceledException) {
            // Expected when the editor pauses, stops, or the host rate changes.
        } catch {
            if (Volatile.Read(ref generation) == producerGeneration) {
                Volatile.Write(ref playbackState, (int)PlaybackState.Stopped);
            }
        }
    }

    private void StopLocked(bool clearSource) {
        Interlocked.Increment(ref generation);
        producerCancellation?.Cancel();
        producerCancellation?.Dispose();
        producerCancellation = null;
        Volatile.Write(ref playbackState, (int)PlaybackState.Stopped);
        Volatile.Write(ref writeFrame, 0);
        Volatile.Write(ref readFrame, 0);
        Volatile.Write(ref positionFrames, 0);
        Volatile.Write(ref endOfStream, 0);
        if (clearSource) source = null;
    }

    private void ThrowIfDisposed() {
        ObjectDisposedException.ThrowIf(disposed, this);
    }

    public void Dispose() {
        lock (gate) {
            if (disposed) return;
            StopLocked(clearSource: true);
            disposed = true;
        }
        GC.SuppressFinalize(this);
    }
}
