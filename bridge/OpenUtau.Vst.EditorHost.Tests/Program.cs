using Avalonia.Controls;
using Avalonia.Input;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using OpenUtau.Vst.EditorHost;

var shortcutRoot = new Border();
var shortcutChild = new Border();
shortcutRoot.Child = shortcutChild;
var shortcutCalls = 0;
EmbeddedShortcutRouter.Attach(shortcutRoot, (_, args) => {
    shortcutCalls++;
    Require(args.Key == Key.Z, "Embedded shortcut changed key identity.");
    Require(args.KeyModifiers == KeyModifiers.Control,
        "Embedded shortcut changed modifier identity.");
    args.Handled = true;
});
var shortcutEvent = new KeyEventArgs {
    RoutedEvent = InputElement.KeyDownEvent,
    Key = Key.Z,
    KeyModifiers = KeyModifiers.Control,
};
shortcutChild.RaiseEvent(shortcutEvent);
Require(shortcutCalls == 1,
    "Embedded root did not receive the descendant shortcut event.");
Require(shortcutEvent.Handled,
    "Embedded root did not preserve shortcut handled state.");

var viewportPolicy = new HostTransportViewportPolicy();
Require(!viewportPolicy.PreserveViewport(true, false, false),
    "A no-movement stop transition incorrectly requested a scroll update.");
Require(viewportPolicy.PreserveViewport(false, false, true),
    "FL's delayed stop rewind did not preserve the editor viewport.");
Require(!viewportPolicy.PreserveViewport(false, true, true),
    "A host play transition incorrectly suppressed editor auto-scroll.");
Require(!viewportPolicy.PreserveViewport(true, true, true),
    "Ongoing host playback incorrectly suppressed editor auto-scroll.");

var expiredViewportPolicy = new HostTransportViewportPolicy();
expiredViewportPolicy.PreserveViewport(true, false, false);
for (var i = 1; i < HostTransportViewportPolicy.StopSettleCallbacks; i++) {
    expiredViewportPolicy.PreserveViewport(false, false, false);
}
Require(!expiredViewportPolicy.PreserveViewport(false, false, true),
    "A normal stopped-host seek was mistaken for a delayed stop rewind.");

using var output = new VstPreviewAudioOutput();
output.SetSampleRate(48000);
var initialToneRevision = output.ToneRevision;
output.NotifyToneStarted(440.0);
Require(output.CopyToneState(out var toneFrequency, out var toneRevision) == 1,
    "Piano-key audition did not start.");
Require(toneFrequency == 440.0 && toneRevision == initialToneRevision + 1,
    "Piano-key audition published the wrong pitch or revision.");
output.NotifyToneEnded(440.0);
Require(output.CopyToneState(out _, out _) == 0,
    "Piano-key audition did not release.");
output.Init(new SignalGenerator(44100, 1) {
    Frequency = 440,
    Gain = 0.25,
    Type = SignalGeneratorType.Sin,
}.Take(TimeSpan.FromMilliseconds(250)));
Require(output.CopyToneState(out _, out _) == -1,
    "Normal OpenUtau playback remained in piano-key audition mode.");

Require(output.GetOutputDevices().Count == 0,
    "VST preview exposed a hardware audio device.");
output.Play();

var buffer = new float[4096 * 2];
var copiedFrames = 0;
var nonSilent = false;
var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(3);
while (DateTime.UtcNow < deadline && copiedFrames < 4800) {
    var copied = output.CopyTo(buffer);
    if (copied > 0) {
        copiedFrames += copied;
        nonSilent |= buffer.AsSpan(0, copied * 2)
            .ContainsAnyExcept(0.0f);
    } else {
        await Task.Delay(2);
    }
}

Require(copiedFrames >= 4800,
    "Device-free preview did not deliver resampled audio.");
Require(nonSilent, "Device-free preview returned silence.");
Require(output.GetPosition() > 0,
    "Preview consumption did not advance its playback clock.");

// Unbounded preview sources must not fill the entire ring while the DAW is
// stopped. A bounded queue also makes explicit buffer resets take effect soon.
output.Stop();
output.Init(new SignalGenerator(48000, 2) {
    Frequency = 440,
    Gain = 0.25,
    Type = SignalGeneratorType.Sin,
});
output.Play();
await Task.Delay(100);
var queuedAuditionFrames = output.CopyTo(new float[16384 * 2]);
Require(queuedAuditionFrames is > 0 and <= 12000,
    $"Piano-roll audition buffered {queuedAuditionFrames} frames instead of staying within its safety window.");
var auditionRevision = output.BufferRevision;
output.DiscardBufferedAudio();
Require(output.BufferRevision == auditionRevision + 1,
    "Piano-roll audition did not publish a buffer reset.");
Require(output.PlaybackState == PlaybackState.Playing,
    "Piano-roll audition stopped after clearing old audio.");
await Task.Delay(10);
Require(output.CopyTo(buffer) >= 0,
    "Piano-roll audition did not refill after clearing old audio.");

output.Pause();
Require(output.CopyTo(buffer) == -1,
    "Paused preview remained active.");
output.Play();
await Task.Delay(5);
Require(output.CopyTo(buffer) >= 0,
    "Paused preview did not resume.");
output.Stop();
Require(output.CopyTo(buffer) == -1,
    "Stopped preview remained active.");

Console.WriteLine(
    "Embedded shortcut routing and device-free preview tests passed.");

static void Require(bool condition, string message) {
    if (!condition) throw new Exception(message);
}
