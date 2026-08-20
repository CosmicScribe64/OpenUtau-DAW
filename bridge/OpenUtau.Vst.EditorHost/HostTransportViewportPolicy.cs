namespace OpenUtau.Vst.EditorHost;

/// <summary>
/// Keeps a DAW stop/rewind from pulling the user's editing viewport away from
/// the phrase they were watching. The playhead still follows the host; only
/// OpenUtau's automatic scroll is suppressed for the play-to-stop transition.
/// A stopped-host seek remains visible and may scroll normally.
/// </summary>
public sealed class HostTransportViewportPolicy {
    // EntryPoints receives transport at 30 Hz. FL Studio reports Stop in two
    // phases: playing becomes false at the last position, then a later callback
    // rewinds to the remembered start. Keep both phases inside one short visual
    // transaction without affecting normal seeks made while already stopped.
    public const int StopSettleCallbacks = 30;
    private int stopSettleCallbacksRemaining;

    public bool PreserveViewport(
            bool wasPlaying, bool isPlaying, bool positionChanged) {
        if (isPlaying) {
            stopSettleCallbacksRemaining = 0;
            return false;
        }
        if (wasPlaying) {
            stopSettleCallbacksRemaining = StopSettleCallbacks;
        }
        var preserve = positionChanged && stopSettleCallbacksRemaining > 0;
        if (stopSettleCallbacksRemaining > 0) {
            stopSettleCallbacksRemaining--;
        }
        return preserve;
    }
}
