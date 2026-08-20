using System.Text.Json;
using System.Text.Json.Serialization;

namespace OpenUtau.Vst.Protocol;

public static class ProtocolVersion {
    public const int Current = 5;
}

public sealed record Hello(
    int Protocol,
    Guid InstanceId,
    string SharedMemoryName,
    int SharedMemoryBytes);

public sealed record TransportState(
    long Epoch,
    long ProjectSample,
    double SampleRate,
    double Tempo,
    int TimeSignatureNumerator,
    int TimeSignatureDenominator,
    bool Playing,
    bool Recording,
    bool Looping,
    long LoopStartSample,
    long LoopEndSample);

public sealed record RenderRequest(
    long Epoch,
    long StartSample,
    int FrameCount,
    double SampleRate,
    bool Offline);

public sealed record RenderReady(long Epoch, long StartSample, int FrameCount);

public sealed record HostTiming(
    double QuarterNotePosition,
    double Tempo,
    long BarPosition,
    int TimeSignatureNumerator,
    int TimeSignatureDenominator);

public sealed record EngineFault(string Code, string Message, bool Recoverable);

public static class ControlKinds {
    public const string Hello = "hello";
    public const string GetState = "getState";
    public const string SetState = "setState";
    public const string Render = "render";
    public const string SetHostTiming = "setHostTiming";
    public const string OpenEditor = "openEditor";
    public const string Ok = "ok";
    public const string Error = "error";
}

public sealed record ControlRequest(long RequestId, string Kind, string? Payload = null);
public sealed record ControlResponse(long RequestId, string Kind, string? Payload = null);

[JsonSourceGenerationOptions(
    PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    GenerationMode = JsonSourceGenerationMode.Metadata)]
[JsonSerializable(typeof(Hello))]
[JsonSerializable(typeof(TransportState))]
[JsonSerializable(typeof(RenderRequest))]
[JsonSerializable(typeof(RenderReady))]
[JsonSerializable(typeof(HostTiming))]
[JsonSerializable(typeof(EngineFault))]
[JsonSerializable(typeof(ControlRequest))]
[JsonSerializable(typeof(ControlResponse))]
public partial class ProtocolJsonContext : JsonSerializerContext;

public static class MessageCodec {
    private const int MaximumMessageBytes = FramedStream.MaximumFrameBytes;

    public static byte[] Encode<T>(T value) {
        ArgumentNullException.ThrowIfNull(value);
        var payload = JsonSerializer.SerializeToUtf8Bytes(value, typeof(T), ProtocolJsonContext.Default);
        if (payload.Length > MaximumMessageBytes) {
            throw new InvalidDataException($"Message exceeds {MaximumMessageBytes} bytes.");
        }
        var frame = new byte[payload.Length + sizeof(int)];
        BitConverter.TryWriteBytes(frame, payload.Length);
        payload.CopyTo(frame.AsSpan(sizeof(int)));
        return frame;
    }

    public static T Decode<T>(ReadOnlySpan<byte> frame) {
        if (frame.Length < sizeof(int)) {
            throw new InvalidDataException("Message frame is truncated.");
        }
        var length = BitConverter.ToInt32(frame[..sizeof(int)]);
        if (length < 0 || length > MaximumMessageBytes || length != frame.Length - sizeof(int)) {
            throw new InvalidDataException("Message length is invalid.");
        }
        return (T?)JsonSerializer.Deserialize(frame[sizeof(int)..], typeof(T), ProtocolJsonContext.Default)
            ?? throw new InvalidDataException("Message payload is null.");
    }
}

public static class FramedStream {
    public const int MaximumFrameBytes = 256 * 1024 * 1024;

    public static async ValueTask WriteAsync<T>(Stream stream, T value,
                                                 CancellationToken cancellationToken = default) {
        ArgumentNullException.ThrowIfNull(stream);
        var frame = MessageCodec.Encode(value);
        await stream.WriteAsync(frame, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    public static async ValueTask<T?> ReadAsync<T>(Stream stream,
                                                    CancellationToken cancellationToken = default) {
        ArgumentNullException.ThrowIfNull(stream);
        var header = new byte[sizeof(int)];
        var headerRead = await ReadExactlyOrEofAsync(stream, header, cancellationToken).ConfigureAwait(false);
        if (!headerRead) return default;
        var length = BitConverter.ToInt32(header);
        if (length < 0 || length > MaximumFrameBytes) {
            throw new InvalidDataException("Frame length is invalid.");
        }
        var frame = new byte[checked(length + sizeof(int))];
        header.CopyTo(frame, 0);
        await stream.ReadExactlyAsync(frame.AsMemory(sizeof(int), length), cancellationToken)
            .ConfigureAwait(false);
        return MessageCodec.Decode<T>(frame);
    }

    private static async ValueTask<bool> ReadExactlyOrEofAsync(
            Stream stream, Memory<byte> destination, CancellationToken cancellationToken) {
        var read = 0;
        while (read < destination.Length) {
            var count = await stream.ReadAsync(destination[read..], cancellationToken).ConfigureAwait(false);
            if (count == 0) {
                if (read == 0) return false;
                throw new EndOfStreamException("Frame header is truncated.");
            }
            read += count;
        }
        return true;
    }
}
