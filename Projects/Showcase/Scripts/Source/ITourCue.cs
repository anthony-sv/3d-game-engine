namespace Showcase;

/// <summary>An exhibit that sets itself off on cue: the tour cues it when the camera passes a shot that names it, and its key
/// does the same while you explore.</summary>
public interface ITourCue
{
	/// <summary>Sets the exhibit off (swings the wrecking ball, tips the dominoes, fires the ball launcher).</summary>
	void Perform();

	/// <summary>Tells an exhibit the tour cues that the tour is over (it ended or was skipped): what the exhibit keeps up for the
	/// tour stops. Exhibits whose cue simply runs its course do nothing.</summary>
	void TourEnded()
	{
	}
}
