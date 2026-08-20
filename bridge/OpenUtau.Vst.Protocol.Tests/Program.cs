using OpenUtau.Vst.Protocol;

var state = new TransportState(
    Epoch: 7,
    ProjectSample: 48000,
    SampleRate: 48000,
    Tempo: 128.5,
    TimeSignatureNumerator: 7,
    TimeSignatureDenominator: 8,
    Playing: true,
    Recording: false,
    Looping: true,
    LoopStartSample: 24000,
    LoopEndSample: 96000);

var decoded = MessageCodec.Decode<TransportState>(MessageCodec.Encode(state));
Require(decoded == state, "Transport state did not round-trip.");
var timing = new HostTiming(16.25, 143.0, 4, 7, 8);
Require(MessageCodec.Decode<HostTiming>(MessageCodec.Encode(timing)) == timing,
    "Host timing did not round-trip.");
ExpectInvalid(Array.Empty<byte>());
ExpectInvalid(new byte[] { 10, 0, 0, 0, (byte)'{' });
Console.WriteLine("Protocol contract tests passed.");

static void ExpectInvalid(byte[] bytes) {
    try {
        MessageCodec.Decode<TransportState>(bytes);
        throw new Exception("Invalid frame was accepted.");
    } catch (InvalidDataException) {
        // Expected.
    }
}

static void Require(bool condition, string message) {
    if (!condition) {
        throw new Exception(message);
    }
}
