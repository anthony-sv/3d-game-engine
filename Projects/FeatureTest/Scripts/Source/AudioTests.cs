using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Audio sources. The scene's Speaker has Audio/Beep.wav and does not play on start; test runs play into the null
/// output, which still keeps time.</summary>
public sealed class AudioTests : FeatureTestScript
{
	public Entity? Speaker;

	protected override void Start()
	{
		AudioSourceComponent source = Speaker!.GetComponent<AudioSourceComponent>()!;
		Check("audio source properties", () =>
		{
			AudioClip? beep = Assets.Load<AudioClip>("Audio/Beep.wav");
			Assert.AreEqual(beep, source.Clip);
			source.Clip = null;
			Assert.IsNull(source.Clip);
			source.Clip = beep;
			Assert.IsFalse(source.PlayOnStart);
			source.PlayOnStart = false;
			source.Volume = 0.5f;
			Assert.AreEqual(0.5f, source.Volume);
			source.Pitch = 1.25f;
			Assert.AreEqual(1.25f, source.Pitch);
			source.Loop = true;
			Assert.IsTrue(source.Loop);
			source.Spatial = true;
			Assert.IsTrue(source.Spatial);
			source.MinDistance = 2.0f;
			Assert.AreEqual(2.0f, source.MinDistance);
			source.MaxDistance = 40.0f;
			Assert.AreEqual(40.0f, source.MaxDistance);
		});
	}

	protected override bool Step(int frame, float deltaTime)
	{
		AudioSourceComponent source = Speaker!.GetComponent<AudioSourceComponent>()!;
		switch (frame)
		{
			case 1:
				Check("sources play when asked", () =>
				{
					Assert.IsFalse(source.IsPlaying);
					source.Play();
					Assert.IsTrue(source.IsPlaying);
				});
				return false;
			case 2:
				Check("sources pause, resume and stop", () =>
				{
					Assert.IsTrue(source.IsPlaying, "a looping sound plays on");
					source.Pause();
					Assert.IsFalse(source.IsPlaying);
					source.Play();
					Assert.IsTrue(source.IsPlaying);
					source.Stop();
					Assert.IsFalse(source.IsPlaying);
				});
				return true;
			default:
				return true;
		}
	}
}
