using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Audio sources, loaded and streamed, their one-shots and voice fields, clips played at points, the project's mixer
/// (Audio/FeatureTest.smixer: Master over Music and Effects, Voices under Effects) and the game's values of its buses. The
/// scene's Speaker has Audio/Beep.wav and does not play on start; test runs play into the null output, which still keeps
/// time, so the checks assert what the main thread knows at a call, never that a sound has ended.</summary>
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
			Assert.IsFalse(source.Stream);
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
		Check("a source plays one-shots on voices of its own, and the scene plays a clip at a point", () =>
		{
			AudioClip beep = source.Clip!;
			Assert.AreEqual(string.Empty, source.Bus);
			Assert.AreEqual(128u, source.Priority);
			Assert.AreEqual(4u, source.MaxVoices);
			source.Bus = "Music";
			Assert.AreEqual("Music", source.Bus);
			source.Priority = 200;
			Assert.AreEqual(200u, source.Priority);
			source.MaxVoices = 3;
			Assert.AreEqual(3u, source.MaxVoices);
			Assert.IsTrue(source.PlayOneShot(0.5f), "a one-shot of the source's clip");
			Assert.IsTrue(source.PlayOneShot(beep, 0.25f), "a one-shot of a clip given");
			Assert.IsTrue(Audio.PlayClipAtPoint(beep, new Vector3(0.0f, 1.0f, -3.0f), 0.5f, "Music"));
			Assert.IsTrue(Audio.ActiveVoiceCount >= 3, $"{Audio.ActiveVoiceCount} voices");
			Assert.IsFalse(source.IsPlaying, "one-shots leave the source's own sound as it was");
			source.Bus = string.Empty;
			source.Priority = 128;
			source.MaxVoices = 4;
		});
		Check("the project's mixer names its buses, and the game sets its own values of them", () =>
		{
			Mixer mixer = Assets.Load<Mixer>("Audio/FeatureTest.smixer")!;
			Assert.AreEqual(4, mixer.BusCount);
			Assert.AreEqual("Master", mixer.GetBusName(0));
			Assert.AreEqual("Voices", mixer.GetBusName(3));
			Assert.IsTrue(mixer.HasBus("music"));
			Assert.IsFalse(mixer.HasBus("Ambience"));
			Assert.AreEqual("Effects", mixer.GetBusParent("Voices"));
			Assert.AreEqual(-6.0f, mixer.GetBusVolume("Music"));
			Assert.IsTrue(mixer.IsBusMuted("Effects"));

			// The game's values start at the defaults and are a stage of their own: the asset keeps its values.
			Assert.AreEqual(1.0f, Audio.MasterVolume);
			Audio.MasterVolume = 0.8f;
			Assert.AreEqual(0.8f, Audio.MasterVolume);
			Audio.MasterVolume = 1.0f;
			Assert.AreEqual(0.0f, Audio.GetBusVolume("Music"));
			Audio.SetBusVolume("music", -12.0f);
			Assert.AreEqual(-12.0f, Audio.GetBusVolume("MUSIC"));
			Assert.AreEqual(-6.0f, mixer.GetBusVolume("Music"), "the game's values never change the asset");
			Assert.IsFalse(Audio.IsBusMuted("Music"));
			Audio.SetBusMuted("Music", true);
			Assert.IsTrue(Audio.IsBusMuted("Music"));
			Assert.IsFalse(mixer.IsBusMuted("Music"));
			Audio.SetBusMuted("Music", false);
			Audio.SetBusVolume("Music", 0.0f);
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
				return false;
			case 3:
				Check("sources stream their clip from its file", () =>
				{
					source.Stream = true;
					Assert.IsTrue(source.Stream);
					Assert.IsFalse(source.IsPlaying, "a new sound waits for Play");
					source.Play();
					Assert.IsTrue(source.IsPlaying);
				});
				return false;
			case 4:
				Check("streamed sources loop, pause and stop", () =>
				{
					Assert.IsTrue(source.IsPlaying, "a looping streamed sound plays on");
					source.Pause();
					Assert.IsFalse(source.IsPlaying);
					source.Play();
					Assert.IsTrue(source.IsPlaying);
					source.Stop();
					Assert.IsFalse(source.IsPlaying);
					source.Stream = false;
					Assert.IsFalse(source.Stream);
				});
				return true;
			default:
				return true;
		}
	}
}
