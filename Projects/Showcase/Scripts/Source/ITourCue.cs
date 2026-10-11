namespace Showcase;

/// <summary>An exhibit that sets itself off on cue: the tour cues it when the camera passes a shot that names it, and its key
/// does the same while you explore.</summary>
public interface ITourCue
{
	/// <summary>Sets the exhibit off (swings the wrecking ball, tips the dominoes, fires the ball launcher).</summary>
	void Perform();
}
