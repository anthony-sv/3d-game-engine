using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Strada;
using Strada.Testing;

namespace Blocks;

/// <summary>The checks of Scenes/Tests.sscene: the rules of the board, the auto player, the camera's spring and view and the
/// screen's numbers when the scene starts, then the scene's game (Game) as it plays: moves, sounds, callouts, the camera, the
/// screen (the labels over the board, the score panel and the menus, whose buttons it clicks as the mouse does, and that
/// none of it covers the board on screens from 5:4 to 21:9), the game-over screen and the title screen's demo (the test scene
/// starts the game without the title screen, keeps no best score and seeds its demo). The checks run in a language that
/// groups thousands with a narrow no-break space, as French does. The test run ends once they are done (play the scene, or
/// `StradaRuntime --project Blocks.sproj --scene Scenes/Tests.sscene --test`).</summary>
public sealed class BlocksTests : Script
{
	[Tooltip("The game the checks play")]
	public Entity? Game;

	// How long the checks give the demo to do what they wait for (frames).
	private const int Patience = 600;

	// The screens the layout checks look at, from 5:4 to 21:9 (the test run's window is 16:10).
	private static readonly Vector2[] s_Screens =
	[
		new(1000.0f, 800.0f), new(1024.0f, 768.0f), new(1280.0f, 800.0f), new(1920.0f, 1080.0f), new(2560.0f, 1080.0f),
	];

	// The screen on which the title's sign is largest beside the board, so it clears the board last while the camera glides to
	// the title's view: the canvas fits its 16:9 reference by the screen's height from 16:9 on, where the board keeps its size
	// too, so wider screens show both as 16:9 does.
	private static readonly Vector2 s_WideScreen = new(1920.0f, 1080.0f);

	// The board's parts the screen leaves in sight: the frame with its backdrop, and the panels with the strips along their
	// tops (the highest parts of the board).
	private static readonly string[] s_BoardParts =
	[
		"Left Wall", "Right Wall", "Floor", "Backdrop", "Hold Panel", "Next Panel", "Hold Neon", "Next Neon",
	];

	// The title's sign (its block letters and the subtitle under them) and its menu.
	private static readonly string[] s_Sign = ["Tile B", "Tile L", "Tile O", "Tile C", "Tile K", "Tile S", "Subtitle"];
	private static readonly string[] s_TitleMenu = ["Play Button", "Quit Button", "Title Hint"];
	private static readonly string[] s_Panels = ["Score Panel", "Controls Panel"];

	// The steps of the run after the first frame: each waits for its frames or condition, runs its checks and starts the
	// next. A click takes effect at the start of the next frame, so the checks of what it did wait two frames (three for the
	// camera, which may move before the game does in a frame).
	private enum Step
	{
		Start,
		Play,
		Lock,
		Paused,
		Resumed,
		Fade,
		NewBest,
		TitleReturn,
		KeyWait,
		PlayAgain,
		PauseRestart,
		Restarted,
		PauseQuit,
		QuitToTitle,
		GameOverQuit,
		BackToTitle,
		DemoHold,
		DemoBlock,
		DemoReplan,
		DemoSettle,
		Demo,
		GameStarted,
		CameraBack,
		Done,
	}

	private Step m_Step = Step.Start;
	private int m_StepFrames;
	private int m_StartRow;
	private Vector3 m_CameraPlace;
	private int m_BestBeforeDemo;
	private int m_PiecesBeforeBlock;
	private int m_ReplansBeforeBlock;
	// The score of a game when a closed menu's button was clicked.
	private int m_ScoreBeforeClicks;
	// The settled cells that block a demo piece's way, until the demo has planned again.
	private readonly List<(int X, int Y)> m_Blocked = [];
	// What the callout checks saw over the frames: the first callout out of its room, and the frames that showed callouts in
	// every slot at once.
	private string? m_CalloutTrouble;
	private int m_FramesWithEverySlot;
	// The points of the last clear the callouts showed.
	private int m_LastClearPoints;
	// What the title's watch saw over the frames: the first frame the title covered the board, and the frames of the camera's
	// glide to the title's view in which the sign waited and in which it faded in.
	private string? m_TitleTrouble;
	private int m_SignWaited;
	private int m_SignFadedIn;
	// The language the scene had, given back when the checks end.
	private CultureInfo? m_Culture;
	// Entities of the scene by name, found once.
	private readonly Dictionary<string, Entity> m_Found = [];

	protected override void OnCreate()
	{
		// The checks run in a language that groups thousands with a narrow no-break space, as French does: the font has no
		// such space, so the screen's numbers must not take the player's language.
		m_Culture = CultureInfo.CurrentCulture;
		CultureInfo spaced = (CultureInfo)CultureInfo.InvariantCulture.Clone();
		spaced.NumberFormat.NumberGroupSeparator = "\u202F";
		CultureInfo.CurrentCulture = spaced;

		TestReporter.Run("every shape has four cells in every rotation", () =>
		{
			foreach (PieceKind kind in System.Enum.GetValues<PieceKind>())
			{
				for (int rotation = 0; rotation < 4; rotation++)
				{
					Assert.AreEqual(4, new Piece(kind, 0, 3, rotation).Cells.Distinct().Count(), $"{kind} {rotation}");
				}
			}
			Assert.AreEqual(new Piece(PieceKind.T, 0, 0, 0), new Piece(PieceKind.T, 0, 0, 4));
		});
		TestReporter.Run("pieces fit inside the board, on empty cells", () =>
		{
			Board board = new();
			Piece piece = Board.Spawn(PieceKind.T);
			Assert.IsTrue(board.Fits(piece));
			Assert.IsFalse(board.Fits(piece.Moved(-4, 0)), "past the left wall");
			Assert.IsFalse(board.Fits(piece.Moved(7, 0)), "past the right wall");
			Assert.IsFalse(board.Fits(new Piece(PieceKind.O, 0, 0, 0)), "below the floor");
			board.Set(4, Board.Height - 1, PieceKind.I);
			Assert.IsFalse(board.Fits(piece), "on a settled cell");
		});
		TestReporter.Run("pieces drop onto the floor or the stack", () =>
		{
			Board board = new();
			Piece landed = board.Drop(Board.Spawn(PieceKind.O));
			Assert.AreEqual(0, landed.Cells.Min(cell => cell.Y));
			board.Lock(landed);
			Assert.AreEqual(2, board.Drop(Board.Spawn(PieceKind.O)).Cells.Min(cell => cell.Y));
		});
		TestReporter.Run("turns kick pieces off the walls", () =>
		{
			Board board = new();
			// A vertical I against the left wall turns flat by moving right.
			Piece upright = new(PieceKind.I, -1, 10, 3);
			Assert.IsTrue(board.Fits(upright));
			Piece? flat = board.Turn(upright, 1);
			Assert.IsNotNull(flat);
			Assert.IsTrue(board.Fits(flat.Value));
			Assert.AreEqual(0, flat.Value.Rotation);
			// Walled in on both sides, nothing fits.
			for (int y = 0; y < Board.Height; y++)
			{
				for (int x = 0; x < Board.Width; x++)
				{
					if (x != 4)
					{
						board.Set(x, y, PieceKind.O);
					}
				}
			}
			Assert.IsNull(board.Turn(new Piece(PieceKind.I, 2, 10, 1), 1));
		});
		TestReporter.Run("full rows clear and the rows above move down", () =>
		{
			Board board = new();
			for (int x = 0; x < Board.Width; x++)
			{
				board.Set(x, 0, PieceKind.L);
				board.Set(x, 2, PieceKind.J);
			}
			board.Set(3, 1, PieceKind.S);
			board.Set(5, 3, PieceKind.Z);
			Assert.IsTrue(board.IsFull(0));
			Assert.IsFalse(board.IsFull(1));
			Board copy = board.Copy();
			Assert.AreEqual(2, board.ClearRows());
			Assert.AreEqual(PieceKind.S, board[3, 0]);
			Assert.AreEqual(PieceKind.Z, board[5, 1]);
			Assert.IsNull(board[3, 1]);
			Assert.IsNull(board[5, 3]);
			Assert.AreEqual(0, board.ClearRows());
			Assert.AreEqual(PieceKind.J, copy[3, 2], "a copy keeps its cells");
		});
		TestReporter.Run("clearing more rows at once scores more, more on higher levels", () =>
		{
			Assert.AreEqual(100, Board.Score(1, 1));
			Assert.AreEqual(800, Board.Score(4, 1));
			Assert.AreEqual(1600, Board.Score(4, 2));
			Assert.AreEqual(0, Board.Score(0, 3));
		});
		TestReporter.Run("each bag holds every kind once", () =>
		{
			PieceBag bag = new(Random.Range);
			for (int round = 0; round < 3; round++)
			{
				List<PieceKind> kinds = [];
				for (int i = 0; i < 7; i++)
				{
					PieceKind next = bag.Peek();
					Assert.AreEqual(next, bag.Next());
					kinds.Add(next);
				}
				Assert.AreEqual(7, kinds.Distinct().Count());
			}
			// The queue looks ahead across bags.
			PieceKind[] ahead = [bag.Peek(0), bag.Peek(1), bag.Peek(9)];
			Assert.AreEqual(ahead[0], bag.Next());
			Assert.AreEqual(ahead[1], bag.Next());
			for (int i = 0; i < 7; i++)
			{
				bag.Next();
			}
			Assert.AreEqual(ahead[2], bag.Next());
		});
		TestReporter.Run("the auto player only makes legal placements and keeps clearing rows", () =>
		{
			// A hundred pieces of a fixed sequence, each played as the demo plays it: turn in place, slide, drop.
			System.Random random = new(7);
			PieceBag bag = new((min, max) => random.Next(min, max));
			Board board = new();
			AutoPlayer player = new();
			int lines = 0;
			int bigClears = 0;
			for (int placed = 0; placed < 100; placed++)
			{
				Piece piece = Board.Spawn(bag.Next());
				Placement? choice = player.Choose(board, piece, bag.Peek());
				Assert.IsNotNull(choice, $"piece {placed} has a place");
				Placement placement = choice.Value;
				Assert.IsTrue(board.Fits(placement.Landed), $"piece {placed} lands on empty cells");
				Assert.IsFalse(board.Fits(placement.Landed.Moved(0, -1)), $"piece {placed} rests on the stack");
				Assert.IsTrue(placement.Turns is >= -1 and <= 2, $"piece {placed} turns {placement.Turns}");
				Piece moved = piece;
				for (int i = 0; i < System.Math.Abs(placement.Turns); i++)
				{
					Piece? turned = board.Turn(moved, System.Math.Sign(placement.Turns));
					Assert.IsNotNull(turned, $"piece {placed} turns");
					moved = turned.Value;
				}
				while (moved.X != placement.Landed.X)
				{
					moved = moved.Moved(System.Math.Sign(placement.Landed.X - moved.X), 0);
					Assert.IsTrue(board.Fits(moved), $"piece {placed} slides freely");
				}
				Assert.AreEqual(placement.Landed, board.Drop(moved), $"piece {placed} drops where it was planned");
				Assert.IsTrue(board.Lock(placement.Landed), $"piece {placed} settles inside the board");
				int cleared = board.ClearRows();
				lines += cleared;
				bigClears += cleared >= 3 ? 1 : 0;
				for (int x = 0; x < Board.Width; x++)
				{
					Assert.IsNull(board[x, 16], $"the stack stays low (piece {placed})");
				}
			}
			Assert.IsTrue(lines >= 25, $"{lines} rows cleared");
			Assert.IsTrue(bigClears >= 3, $"{bigClears} clears of three or four rows");
		});
		TestReporter.Run("the auto player takes the clears the board offers, the shortest way", () =>
		{
			AutoPlayer player = new();
			// Four rows full but for the rightmost column: a long piece goes down the well.
			Board board = new();
			FillRows(board, 0, 4, Board.Width - 1);
			Placement? tetris = player.Choose(board, Board.Spawn(PieceKind.I), PieceKind.T);
			Assert.IsNotNull(tetris);
			Assert.IsTrue(tetris.Value.Landed.Cells.All(cell => cell.X == Board.Width - 1), "down the well");
			board.Lock(tetris.Value.Landed);
			Assert.AreEqual(4, board.ClearRows());
			// A row with a gap of two: the square fills it, without turning (its turns change nothing).
			board.Clear();
			for (int x = 2; x < Board.Width; x++)
			{
				board.Set(x, 0, PieceKind.L);
			}
			Placement? fill = player.Choose(board, Board.Spawn(PieceKind.O));
			Assert.IsNotNull(fill);
			Assert.AreEqual(0, fill.Value.Turns);
			board.Lock(fill.Value.Landed);
			Assert.AreEqual(1, board.ClearRows());
		});
		TestReporter.Run("the auto player gives up only when the piece has no room", () =>
		{
			AutoPlayer player = new();
			Board board = new();
			for (int y = 0; y < Board.Height + Board.HiddenRows; y++)
			{
				for (int x = 0; x < Board.Width; x++)
				{
					board.Set(x, y, x % 2 == 0 ? PieceKind.Z : null);
				}
			}
			Assert.IsNull(player.Choose(board, Board.Spawn(PieceKind.T), PieceKind.I));
			board.Clear();
			Assert.IsNotNull(player.Choose(board, Board.Spawn(PieceKind.T)), "without knowing the next piece");
		});
		TestReporter.Run("the auto player holds a piece when the one that comes instead fits clearly better", () =>
		{
			AutoPlayer player = new();
			Board board = new();
			FillRows(board, 0, 4, Board.Width - 1);
			Assert.IsTrue(player.ShouldHold(board, Board.Spawn(PieceKind.S), PieceKind.Z, PieceKind.I, PieceKind.O), "an S waits for the long piece");
			Assert.IsFalse(player.ShouldHold(board, Board.Spawn(PieceKind.I), PieceKind.O, PieceKind.S, PieceKind.Z), "the long piece goes down the well");
			// A piece that comes instead without room is never taken: a settled cell where the square appears.
			board.Clear();
			board.Set(4, Board.Height, PieceKind.L);
			Assert.IsFalse(player.ShouldHold(board, Board.Spawn(PieceKind.I), PieceKind.T, PieceKind.O, PieceKind.T), "no room for the square");
		});
		TestReporter.Run("the camera's kick springs back steadily, however long a frame takes", () =>
		{
			Vector3 kick = new(0.0f, -0.22f, 0.0f);
			// A quarter-second frame moves the spring as fifteen sixtieths of a second do.
			Vector3 offset = kick;
			Vector3 velocity = Vector3.Zero;
			CameraShake.StepSpring(ref offset, ref velocity, 17.0f, 0.55f, 0.25f);
			Vector3 steady = kick;
			Vector3 steadyVelocity = Vector3.Zero;
			for (int i = 0; i < 15; i++)
			{
				CameraShake.StepSpring(ref steady, ref steadyVelocity, 17.0f, 0.55f, 1.0f / 60.0f);
			}
			Assert.AreApproximatelyEqual(steady, offset, 0.001f);
			// Long frames never throw the view further than the kick, and it settles.
			offset = kick;
			velocity = Vector3.Zero;
			for (int i = 0; i < 8; i++)
			{
				CameraShake.StepSpring(ref offset, ref velocity, 17.0f, 0.55f, 0.25f);
				Assert.IsTrue(offset.Length <= kick.Length, $"{offset.Length} after {i + 1} long frames");
			}
			Assert.IsTrue(offset.Length < 0.001f, $"settled to {offset.Length}");
		});
		TestReporter.Run("the view keeps its width on screens narrower than it was made for", () =>
		{
			Assert.AreEqual(42.0f, CameraShake.FitFieldOfView(42.0f, 1.6f, 1.6f), "16:10 as made");
			Assert.AreEqual(42.0f, CameraShake.FitFieldOfView(42.0f, 1.6f, 21.0f / 9.0f), "wider screens see more at the sides");
			// The half width of the view a unit in front of the camera.
			float across = Mathf.Tan(21.0f * Mathf.Deg2Rad) * 1.6f;
			foreach (float aspect in new[] { 1.5f, 4.0f / 3.0f, 1.25f, 9.0f / 16.0f })
			{
				float fieldOfView = CameraShake.FitFieldOfView(42.0f, 1.6f, aspect);
				Assert.IsTrue(fieldOfView > 42.0f, $"{fieldOfView} at {aspect}");
				Assert.AreApproximatelyEqual(across, Mathf.Tan(0.5f * fieldOfView * Mathf.Deg2Rad) * aspect, 1e-4f, $"as wide at {aspect}");
			}
			Assert.AreEqual(170.0f, CameraShake.FitFieldOfView(42.0f, 1.6f, 0.001f), "a sliver of a screen");
			foreach (float aspect in new[] { 0.0f, -1.0f, float.NaN, float.PositiveInfinity })
			{
				Assert.AreEqual(42.0f, CameraShake.FitFieldOfView(42.0f, 1.6f, aspect), $"an aspect ratio of {aspect}");
			}
		});
		TestReporter.Run("the screen's numbers group thousands with commas in every language", () =>
		{
			Assert.AreEqual("\u202F", CultureInfo.CurrentCulture.NumberFormat.NumberGroupSeparator, "the checks' language");
			Assert.AreEqual("2,650", Hud.FormatScore(2650));
			Assert.AreEqual("1,234,567", Hud.FormatScore(1234567));
			Assert.AreEqual("0", Hud.FormatScore(0));
			Assert.AreEqual("12", Hud.FormatNumber(12));
		});
	}

	protected override void OnDestroy()
	{
		if (m_Culture is not null)
		{
			CultureInfo.CurrentCulture = m_Culture;
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		BlocksGame game = Game!.As<BlocksGame>()!;
		CameraShake camera = game.Camera!.As<CameraShake>()!;
		m_StepFrames++;
		WatchTitle(game, camera);
		switch (m_Step)
		{
			case Step.Start:
				TestReporter.Run("a game starts with a piece at the top", () =>
				{
					Assert.IsFalse(game.IsOver);
					Assert.IsFalse(game.IsDemo);
					Assert.AreEqual(0, game.Score);
					Assert.AreEqual(1, game.Level);
					Assert.IsTrue(game.Active.Y >= Board.Height - 1, game.Active.Y.ToString());
				});
				TestReporter.Run("the screen starts on the game: no title, menu, dim or DEMO tag", () =>
				{
					Assert.IsFalse(Rect("Title Screen").Visible, "the title");
					Assert.IsFalse(Rect("Pause Menu").Visible, "the pause menu");
					Assert.IsFalse(Rect("Game Over").Visible, "the game-over screen");
					Assert.IsFalse(Rect("Dim").Visible, "the dim");
					Assert.IsFalse(Rect("Demo Tag").Visible, "the DEMO tag");
					Assert.IsNull(game.Hud?.OpenMenu, "no menu takes the keys");
				});
				TestReporter.Run("in a game the score and controls panels leave the board in sight on screens from 5:4 to 21:9", () =>
				{
					Assert.AreEqual(0.0f, camera.AttractWeight, "the camera stands at its place");
					Assert.IsNull(FindCover(game, s_Screens, s_Panels));
				});
				m_StartRow = game.Active.Y;
				m_CameraPlace = camera.Translation;
				Next(Step.Play);
				break;
			case Step.Play when m_StepFrames == 19:
				CheckPlay(game);
				CheckLabels(game);
				Next(Step.Lock);
				break;
			case Step.Lock when m_StepFrames == 45:
				// The piece the play checks left on the floor has waited there.
				TestReporter.Run("a landed piece settles after a moment, with a click", () =>
				{
					Assert.AreEqual(game.LockSound, game.LastSound);
					Assert.AreEqual(4, CountCells(game.Board));
					Assert.IsTrue(game.Active.Y > Board.Height / 2, "the next piece comes from the top");
				});
				CheckShake(game);
				// The biggest callouts in every slot at once: they wait while the game is paused, then play out.
				m_LastClearPoints = ShowEveryCallout(game);
				game.SetPaused(true);
				Next(Step.Paused);
				break;
			case Step.Paused when m_StepFrames == 150:
				CheckPause(game, m_LastClearPoints);
				// The menu's Resume goes on, as Escape does.
				Click("Resume Button");
				Next(Step.Resumed);
				break;
			case Step.Resumed when m_StepFrames == 2:
				TestReporter.Run("the pause menu's Resume goes on with the game", () =>
				{
					Assert.AreEqual(GameState.Playing, game.State);
					Assert.IsNull(game.Hud?.OpenMenu);
				});
				Next(Step.Fade);
				break;
			case Step.Fade when m_StepFrames < 150:
				WatchCallouts();
				break;
			case Step.Fade:
				TestReporter.Run("callouts keep to rooms of their own, so they never run into one another", () =>
				{
					Assert.IsNull(m_CalloutTrouble, "a callout left its room");
					Assert.IsTrue(m_FramesWithEverySlot > 60, $"every slot showed for {m_FramesWithEverySlot} frames");
					foreach (CalloutSlot slot in System.Enum.GetValues<CalloutSlot>())
					{
						(float top, float bottom) = Callouts.Room(slot);
						Assert.IsTrue(top < bottom, $"{slot}: {top} to {bottom}");
						foreach (CalloutSlot other in System.Enum.GetValues<CalloutSlot>())
						{
							(float otherTop, float otherBottom) = Callouts.Room(other);
							Assert.IsTrue(other == slot || bottom <= otherTop || otherBottom <= top, $"{slot} and {other} share room");
						}
					}
				});
				TestReporter.Run("callouts fade away", () =>
				{
					Assert.IsNull(game.Callout(CalloutSlot.Clear));
					Assert.IsNull(game.Callout(CalloutSlot.Bonus));
					Assert.IsNull(game.Callout(CalloutSlot.Level));
				});
				TestReporter.Run("the pause menu and the dim are gone once the game goes on", () =>
				{
					Assert.IsFalse(Rect("Pause Menu").Visible);
					Assert.IsFalse(Rect("Dim").Visible);
				});
				// The game ends with the best score so far; its game-over screen goes back to the title after a quarter of a second.
				game.TitleReturnDelay = 0.25f;
				ForceGameOver(game);
				Next(Step.NewBest);
				break;
			case Step.NewBest when m_StepFrames == 2:
				TestReporter.Run("the game-over screen shows a new best in gold under its tag, with the level and lines reached", () =>
				{
					Assert.AreEqual(GameState.Over, game.State);
					Assert.IsTrue(game.IsNewBest);
					Assert.IsTrue(Rect("Game Over").Visible);
					Assert.IsTrue(Rect("New Best Tag").Visible);
					UITextComponent score = UIText("Final Score");
					Assert.IsTrue(game.Score >= 1000, $"a score of {game.Score} groups its thousands");
					Assert.AreEqual(Hud.FormatScore(game.Score), score.Text);
					Assert.IsTrue(score.Color.B < 0.5f && score.Color.G < score.Color.R, $"gold: {score.Color}");
					Assert.AreEqual($"LEVEL {Hud.FormatNumber(game.Level)}   ·   LINES {Hud.FormatNumber(game.Rows)}", UIText("Final Stats").Text);
				});
				Next(Step.TitleReturn);
				break;
			case Step.TitleReturn when m_StepFrames == 28:
				TestReporter.Run("the game-over screen goes back to the title after a while", () =>
				{
					Assert.AreEqual(GameState.Title, game.State);
					Assert.IsTrue(game.IsDemo);
				});
				// Without a delay, the game-over screen waits for the player.
				game.TitleReturnDelay = 0.0f;
				game.Restart();
				ForceGameOver(game);
				Next(Step.KeyWait);
				break;
			case Step.KeyWait when m_StepFrames == 60:
				TestReporter.Run("with no delay the game-over screen waits for the player", () =>
				{
					Assert.AreEqual(GameState.Over, game.State);
				});
				TestReporter.Run("the game-over screen opens with Play Again focused, the game's score and no new best", () =>
				{
					Assert.AreEqual("Play Again Button", game.Hud?.OpenMenu?.FocusedButton.Name);
					Assert.IsFalse(game.IsNewBest);
					Assert.IsFalse(Rect("New Best Tag").Visible);
					Assert.AreEqual(Hud.FormatScore(game.Score), UIText("Final Score").Text);
				});
				Click("Play Again Button");
				Next(Step.PlayAgain);
				break;
			case Step.PlayAgain when m_StepFrames == 2:
				TestReporter.Run("the game-over screen's Play Again starts a new game", () =>
				{
					Assert.AreEqual(GameState.Playing, game.State);
					Assert.AreEqual(0, game.Score);
					Assert.AreEqual(0, CountCells(game.Board));
				});
				// Points to lose; the title's Play, whose menu is closed, does not start a game.
				game.HardDrop();
				m_ScoreBeforeClicks = game.Score;
				Click("Play Button");
				game.SetPaused(true);
				Next(Step.PauseRestart);
				break;
			case Step.PauseRestart when m_StepFrames == 2:
				TestReporter.Run("menus that are closed take no clicks", () =>
				{
					Assert.AreEqual(GameState.Paused, game.State);
					Assert.IsTrue(m_ScoreBeforeClicks > 0, "a hard drop scored");
					Assert.AreEqual(m_ScoreBeforeClicks, game.Score, "the title's Play started no game");
					Assert.AreEqual(UIButtonState.Disabled, Element<UIButtonComponent>("Play Button").State);
				});
				Click("Restart Button");
				Next(Step.Restarted);
				break;
			case Step.Restarted when m_StepFrames == 2:
				TestReporter.Run("the pause menu's Restart starts the game over", () =>
				{
					Assert.AreEqual(GameState.Playing, game.State);
					Assert.AreEqual(0, game.Score);
					Assert.AreEqual(0, CountCells(game.Board));
					Assert.AreEqual(game.StartSound, game.LastSound);
				});
				game.SetPaused(true);
				Next(Step.PauseQuit);
				break;
			case Step.PauseQuit when m_StepFrames == 2:
				Click("Pause Quit Button");
				Next(Step.QuitToTitle);
				break;
			case Step.QuitToTitle when m_StepFrames == 2:
				TestReporter.Run("the pause menu's Quit goes back to the title", () =>
				{
					Assert.AreEqual(GameState.Title, game.State);
					Assert.IsTrue(game.IsDemo);
				});
				game.Restart();
				ForceGameOver(game);
				Next(Step.GameOverQuit);
				break;
			case Step.GameOverQuit when m_StepFrames == 2:
				Click("Game Over Quit Button");
				Next(Step.BackToTitle);
				break;
			case Step.BackToTitle when m_StepFrames == 2:
				TestReporter.Run("the game-over screen's Quit goes back to the title", () =>
				{
					Assert.AreEqual(GameState.Title, game.State);
					Assert.IsTrue(game.IsDemo);
				});
				// The demo starts again from its seed.
				m_BestBeforeDemo = game.BestScore;
				game.ShowTitle();
				Next(Step.DemoHold);
				break;
			case Step.DemoHold when game.DemoHolds > 0 || m_StepFrames == Patience:
				// The scene's demo seed starts with a piece the auto player puts aside.
				TestReporter.Run("the demo puts a piece aside when the one that comes instead fits better", () =>
				{
					Assert.IsTrue(game.DemoHolds > 0, "it held");
					Assert.IsNotNull(game.Held);
					Assert.AreEqual(game.HoldSound, game.LastSound);
				});
				Next(Step.DemoBlock);
				break;
			case Step.DemoBlock when game.DemoPlan is { } plan && plan.Landed.Rotation == game.Active.Rotation && plan.Landed.X != game.Active.X:
				// The piece is about to slide: settled cells block its way.
				m_PiecesBeforeBlock = game.DemoPieces;
				m_ReplansBeforeBlock = game.DemoReplans;
				BlockSlide(game, System.Math.Sign(plan.Landed.X - game.Active.X));
				Next(Step.DemoReplan);
				break;
			case Step.DemoBlock when m_StepFrames == Patience:
				TestReporter.Fail("a blocked demo move makes a new plan, and the piece still settles", "the demo never slid a piece");
				Next(Step.Demo);
				break;
			case Step.DemoReplan when game.DemoReplans > m_ReplansBeforeBlock || game.DemoPieces > m_PiecesBeforeBlock || m_StepFrames == Patience:
				// It planned again (or the piece settled): the cells go, so the pieces after it find the board the demo made.
				foreach ((int x, int y) in m_Blocked)
				{
					game.Board.Set(x, y, null);
				}
				m_Blocked.Clear();
				Next(Step.DemoSettle);
				break;
			case Step.DemoSettle when game.DemoPieces > m_PiecesBeforeBlock || m_StepFrames == Patience:
				TestReporter.Run("a blocked demo move makes a new plan, and the piece still settles", () =>
				{
					Assert.IsTrue(game.DemoReplans > m_ReplansBeforeBlock, "it planned again");
					Assert.IsTrue(game.DemoPieces > m_PiecesBeforeBlock, "the piece settled");
					Assert.IsTrue(game.IsDemo);
				});
				Next(Step.Demo);
				break;
			case Step.Demo when (game.DemoPieces >= 3 && camera.AttractWeight >= 1.0f) || m_StepFrames == Patience:
				TestReporter.Run("the title's sign waits for the camera to stand back, and the title never covers the board", () =>
				{
					Assert.IsNull(m_TitleTrouble);
					Assert.IsTrue(m_SignWaited > 30, $"the sign waited for {m_SignWaited} frames of the camera's glide");
					Assert.IsTrue(m_SignFadedIn > 10, $"the sign faded in over {m_SignFadedIn} frames of the glide");
				});
				CheckDemo(game, camera);
				// What a click on the title's Play does.
				Click("Play Button");
				Next(Step.GameStarted);
				break;
			case Step.GameStarted when m_StepFrames == 3:
				TestReporter.Run("the title's Play button starts a real game on an empty board", () =>
				{
					Assert.AreEqual(GameState.Playing, game.State);
					Assert.IsFalse(game.IsDemo);
					Assert.AreEqual(0, CountCells(game.Board));
					Assert.AreEqual(0, game.Score);
					Assert.AreEqual(0, game.Rows);
					Assert.IsNull(game.Held);
					Assert.AreEqual(game.StartSound, game.LastSound);
					Assert.AreEqual("GO!", game.Callout(CalloutSlot.Level));
					Assert.IsFalse(Rect("Demo Tag").Visible, "no DEMO tag");
				});
				TestReporter.Run("the camera leaves attract mode when a game starts", () =>
				{
					Assert.IsFalse(camera.Attract);
					Assert.IsTrue(camera.AttractWeight < 1.0f, "it glides back");
				});
				Next(Step.CameraBack);
				break;
			case Step.CameraBack when m_StepFrames == 120:
				TestReporter.Run("the camera glides back to its place in a game", () =>
				{
					Assert.AreEqual(0.0f, camera.AttractWeight);
					Assert.AreApproximatelyEqual(m_CameraPlace, camera.Translation, 0.01f);
				});
				TestReporter.Finish();
				Application.Quit();
				Next(Step.Done);
				break;
		}
	}

	private void Next(Step step)
	{
		m_Step = step;
		m_StepFrames = 0;
	}

	// The checks that play the game through its public methods, right after one another.
	private void CheckPlay(BlocksGame game)
	{
		// About a third of a second has passed: three rows at the scene's fall interval of a tenth of a second.
		TestReporter.Run("pieces fall by themselves", () =>
		{
			int fallen = m_StartRow - game.Active.Y;
			Assert.IsTrue(fallen >= 2 && fallen <= 4, $"fell {fallen} rows");
		});
		TestReporter.Run("pieces move sideways and stop at the walls", () =>
		{
			int column = game.Active.X;
			Assert.IsTrue(game.Shift(-1));
			Assert.AreEqual(column - 1, game.Active.X);
			int moves = 0;
			while (game.Shift(-1))
			{
				moves++;
			}
			Assert.IsTrue(moves < Board.Width, "the wall stops it");
			Assert.IsTrue(game.Active.Cells.Min(cell => cell.X) == 0);
		});
		TestReporter.Run("hard drops settle the piece and bring the next", () =>
		{
			PieceKind next = game.Next;
			int score = game.Score;
			game.HardDrop();
			Assert.AreEqual(next, game.Active.Kind);
			Assert.IsTrue(game.Score > score, "two points per row");
			Assert.IsTrue(game.BurstsSpawned > 0, "dust where it landed");
			Assert.AreEqual(4, CountCells(game.Board));
		});
		TestReporter.Run("a piece can be held once until the next one settles", () =>
		{
			PieceKind current = game.Active.Kind;
			PieceKind next = game.Next;
			Assert.IsNull(game.Held);
			Assert.IsTrue(game.Hold());
			Assert.AreEqual(current, game.Held);
			Assert.AreEqual(next, game.Active.Kind, "the next piece comes when nothing is held");
			Assert.IsFalse(game.Hold(), "once per piece");
			game.HardDrop();
			PieceKind dropped = game.Active.Kind;
			Assert.IsTrue(game.Hold(), "the next piece may hold again");
			Assert.AreEqual(current, game.Active.Kind, "the held piece comes back");
			Assert.AreEqual(dropped, game.Held);
			Assert.IsTrue(game.Active.Y >= Board.Height - 1, "at the top");
		});
		TestReporter.Run("a paused game holds its piece", () =>
		{
			game.SetPaused(true);
			Assert.IsTrue(game.IsPaused);
			Assert.AreEqual(GameState.Paused, game.State);
			Assert.IsFalse(game.Shift(1));
			Assert.IsFalse(game.Turn(1));
			Assert.IsFalse(game.Hold());
			Assert.IsFalse(game.SoftDrop());
			game.SetPaused(false);
			Assert.AreEqual(GameState.Playing, game.State);
		});
		TestReporter.Run("the game ends when the stack reaches the top, ending its callouts, and starts again", () =>
		{
			FillRows(game.Board, 0, 1, Board.Width);
			Assert.AreEqual(1, game.ClearFullRows());
			Assert.AreEqual("SINGLE", game.Callout(CalloutSlot.Clear));
			int score = game.Score;
			ForceGameOver(game);
			Assert.IsTrue(game.IsOver);
			Assert.AreEqual(GameState.Over, game.State);
			Assert.AreEqual(game.GameOverSound, game.LastSound);
			Assert.IsNull(game.Callout(CalloutSlot.Clear), "the game-over screen shows no callout");
			Assert.IsFalse(game.Shift(1), "nothing moves after the end");
			Assert.IsTrue(game.BestScore >= score, "the best score keeps the game's");
			game.Restart();
			Assert.IsFalse(game.IsOver);
			Assert.AreEqual(0, game.Score);
			Assert.IsNull(game.Held);
			Assert.IsNull(game.Board[0, 0]);
		});
		TestReporter.Run("full rows burst into debris, score and sound", () =>
		{
			game.Restart();
			FillRows(game.Board, 0, 1, Board.Width);
			game.Board.Set(4, 1, PieceKind.S);
			int debris = game.DebrisSpawned;
			int bursts = game.BurstsSpawned;
			int sounds = game.SoundsPlayed;
			Assert.AreEqual(1, game.ClearFullRows());
			Assert.AreEqual(debris + Board.Width, game.DebrisSpawned);
			Assert.AreEqual(bursts + 1, game.BurstsSpawned, "sparks along the row");
			Assert.IsTrue(game.SoundsPlayed > sounds, "a clear sound played");
			Assert.AreEqual(1, game.Rows);
			Assert.AreEqual(Board.Score(1, 1), game.Score);
			Assert.AreEqual(PieceKind.S, game.Board[4, 0], "the row above moved down");
			Assert.AreEqual(0, game.ClearFullRows(), "nothing more to clear");
		});
		TestReporter.Run("four rows at once score the most and shake the camera", () =>
		{
			game.Restart();
			FillRows(game.Board, 0, 4, Board.Width);
			Assert.AreEqual(4, game.ClearFullRows());
			Assert.AreEqual(Board.Score(4, 1), game.Score);
			CameraShake? shake = game.Camera?.As<CameraShake>();
			Assert.IsNotNull(shake);
			Assert.IsTrue(shake.Trauma > 0.5f, $"trauma {shake.Trauma}");
		});
		TestReporter.Run("every action plays its own sound", () =>
		{
			AudioClip?[] clips =
			[
				game.MoveSound, game.RotateSound, game.HoldSound, game.SoftDropSound, game.LockSound, game.DropSound, game.SingleSound,
				game.DoubleSound, game.TripleSound, game.TetrisSound, game.LevelUpSound, game.GameOverSound, game.StartSound,
				game.PauseSound,
			];
			Assert.IsTrue(clips.All(clip => clip is not null), "every sound is set");
			Assert.AreEqual(clips.Length, clips.Distinct().Count(), "and differs from the others");

			game.Restart();
			Assert.AreEqual(game.StartSound, game.LastSound);
			Assert.IsTrue(game.Shift(1));
			Assert.AreEqual(game.MoveSound, game.LastSound);
			Assert.IsTrue(game.Turn(1));
			Assert.AreEqual(game.RotateSound, game.LastSound);
			int score = game.Score;
			Assert.IsTrue(game.SoftDrop());
			Assert.AreEqual(game.SoftDropSound, game.LastSound);
			Assert.AreEqual(score + 1, game.Score, "a point per row");
			Assert.IsTrue(game.Hold());
			Assert.AreEqual(game.HoldSound, game.LastSound);
			game.SetPaused(true);
			Assert.AreEqual(game.PauseSound, game.LastSound);
			game.SetPaused(false);
			game.HardDrop();
			Assert.AreEqual(game.DropSound, game.LastSound, "a hard drop thumps (and does not click)");
			(int Rows, AudioClip? Sound)[] clears = [(1, game.SingleSound), (2, game.DoubleSound), (3, game.TripleSound), (4, game.TetrisSound)];
			foreach ((int rows, AudioClip? sound) in clears)
			{
				game.Restart();
				FillRows(game.Board, 0, rows, Board.Width);
				Assert.AreEqual(rows, game.ClearFullRows());
				Assert.AreEqual(sound, game.LastSound, $"{rows} rows");
			}
		});
		TestReporter.Run("clears pop callouts and score combos and back-to-back clears", () =>
		{
			game.Restart();
			FillRows(game.Board, 0, 2, Board.Width);
			Assert.AreEqual(2, game.ClearFullRows());
			Assert.AreEqual("DOUBLE", game.Callout(CalloutSlot.Clear));
			Assert.IsNull(game.Callout(CalloutSlot.Bonus));
			Assert.AreEqual(300, game.Score);
			// Clears in a row add 50 points per clear in a row and level.
			FillRows(game.Board, 0, 4, Board.Width);
			Assert.AreEqual(4, game.ClearFullRows());
			Assert.AreEqual("TETRIS!", game.Callout(CalloutSlot.Clear));
			Assert.AreEqual("COMBO x2", game.Callout(CalloutSlot.Bonus));
			Assert.AreEqual(300 + 800 + 50, game.Score);
			// A four-row clear after another scores half again.
			FillRows(game.Board, 0, 4, Board.Width);
			Assert.AreEqual(4, game.ClearFullRows());
			Assert.AreEqual("BACK-TO-BACK", game.Callout(CalloutSlot.Bonus));
			Assert.AreEqual(300 + 850 + 1200 + 100, game.Score);
			// Ten rows: the next level.
			Assert.AreEqual(10, game.Rows);
			Assert.AreEqual(2, game.Level);
			Assert.AreEqual("LEVEL UP!", game.Callout(CalloutSlot.Level));
			Assert.AreEqual(game.LevelUpSound, game.LastSound);
			Assert.AreEqual("TETRIS!", game.Callout(CalloutSlot.Clear));
		});

		// The next check needs time: a piece lands on the floor and waits there before it settles.
		game.Restart();
		while (game.SoftDrop())
		{
		}
	}

	// Run once the camera has calmed down from the play checks.
	private static void CheckShake(BlocksGame game)
	{
		TestReporter.Run("hard drops jolt the view down and bigger clears shake it more", () =>
		{
			CameraShake shake = game.Camera!.As<CameraShake>()!;
			Assert.IsTrue(shake.Trauma < 0.05f, $"calm before: {shake.Trauma}");
			game.Restart();
			float kick = shake.KickOffset.Y;
			game.HardDrop();
			Assert.IsTrue(shake.KickOffset.Y < kick, $"the view moved down: {shake.KickOffset.Y}");
			game.Restart();
			FillRows(game.Board, 0, 1, Board.Width);
			float before = shake.Trauma;
			game.ClearFullRows();
			float single = shake.Trauma - before;
			game.Restart();
			FillRows(game.Board, 0, 4, Board.Width);
			before = shake.Trauma;
			game.ClearFullRows();
			float tetris = shake.Trauma - before;
			Assert.IsTrue(single > 0.0f, $"a single shakes: {single}");
			Assert.IsTrue(tetris > 2.0f * single, $"a tetris shakes more: {tetris} after {single}");
		});
	}

	// The HOLD and NEXT labels stand where the camera shows their panels' headers: they follow the camera as it moves.
	private static void CheckLabels(BlocksGame game)
	{
		TestReporter.Run("HOLD and NEXT stand over their panels' headers, as large as the camera shows them", () =>
		{
			CameraComponent view = game.Camera!.GetComponent<CameraComponent>()!;
			foreach ((string label, Entity? header) in new[] { ("Hold Label", game.HoldHeader), ("Next Label", game.NextHeader) })
			{
				Assert.IsNotNull(header, label);
				UIRectComponent rect = Rect(label);
				Assert.IsTrue(rect.Visible, label);
				TransformComponent transform = header.Transform;
				Vector3 half = transform.Up * (0.5f * header.Scale.Y);
				Vector2 middle = Flat(view.WorldToScreenPoint(transform.WorldTranslation));
				float height = Vector2.Distance(Flat(view.WorldToScreenPoint(transform.WorldTranslation + half)),
												Flat(view.WorldToScreenPoint(transform.WorldTranslation - half)));
				Assert.IsTrue(Vector2.Distance(middle, rect.Position) < 0.5f, $"{label} at {rect.Position}, its header's middle at {middle}");
				Assert.AreApproximatelyEqual(height, rect.Size.Y, 0.5f, $"{label}: as high as its header");
				float letters = UIText($"{label} Text").FontSize;
				Assert.IsTrue(letters > 0.4f * height && letters < 0.8f * height, $"{label}: letters of {letters} in a header of {height}");
			}
		});
	}

	// The paused game: the menu over the dimmed game, the callouts waiting (the clear's with its points) and the score panel's
	// numbers.
	private static void CheckPause(BlocksGame game, int clearPoints)
	{
		TestReporter.Run("callouts wait out of sight while the game is paused", () =>
		{
			Assert.AreEqual("TETRIS!", game.Callout(CalloutSlot.Clear));
			Assert.IsTrue(clearPoints >= 1000, $"a clear of {clearPoints} points groups its thousands");
			Assert.AreEqual($"+{Hud.FormatScore(clearPoints)}", UIText("Clear Callout Detail").Text);
			Assert.AreEqual(0.0f, Rect("Clear Callout").Opacity);
		});
		TestReporter.Run("the pause menu opens over the dimmed game with Resume focused", () =>
		{
			UIRectComponent menu = Rect("Pause Menu");
			Assert.IsTrue(menu.Visible);
			Assert.AreEqual(1.0f, menu.Opacity);
			Assert.IsTrue(Rect("Dim").Visible && Rect("Dim").Opacity > 0.5f, "the game is dimmed");
			Assert.AreEqual("Resume Button", game.Hud?.OpenMenu?.FocusedButton.Name);
			Assert.AreNotEqual(UIButtonState.Disabled, Element<UIButtonComponent>("Resume Button").State);
			Assert.AreEqual(UIButtonState.Disabled, Element<UIButtonComponent>("Play Button").State, "the title's menu is closed");
		});
		TestReporter.Run("the score panel shows the game's numbers and the rows toward the next level", () =>
		{
			Assert.IsTrue(game.Score >= 1000, $"a score of {game.Score} groups its thousands");
			Assert.AreEqual(Hud.FormatScore(game.Score), UIText("Score Value").Text);
			Assert.AreEqual(Hud.FormatNumber(game.Level), UIText("Level Value").Text);
			Assert.AreEqual(Hud.FormatNumber(game.Rows), UIText("Lines Value").Text);
			Assert.AreEqual(Hud.FormatScore(System.Math.Max(game.BestScore, game.Score)), UIText("Best Value").Text);
			Assert.IsTrue(game.LevelProgress > 0.0f, $"rows toward the next level: {game.Rows}");
			UIRectComponent bar = Rect("Level Bar Fill");
			Assert.IsTrue(bar.Visible);
			Assert.AreApproximatelyEqual(game.LevelProgress, bar.AnchorMax.X, 0.01f);
		});
	}

	// The demo has played a few pieces on the title screen.
	private void CheckDemo(BlocksGame game, CameraShake camera)
	{
		TestReporter.Run("the title screen plays a demo game by itself", () =>
		{
			Assert.AreEqual(GameState.Title, game.State);
			Assert.IsTrue(game.IsDemo);
			Assert.IsTrue(game.DemoPieces >= 3, $"{game.DemoPieces} pieces settled");
			Assert.IsTrue(CountCells(game.Board) > 0, "the demo builds a stack");
			Assert.IsTrue(game.Score > 0, "the demo scores");
			Assert.AreEqual(m_BestBeforeDemo, game.BestScore, "demos do not count for the best score");
		});
		TestReporter.Run("the title's menu opens with Play focused, over a score panel tagged DEMO", () =>
		{
			UIRectComponent title = Rect("Title Screen");
			Assert.IsTrue(title.Visible);
			Assert.AreEqual(1.0f, title.Opacity);
			Assert.AreEqual(1.0f, ShownOpacity("Title Sign"), "the sign shows once the camera stands back");
			Assert.AreEqual("Play Button", game.Hud?.OpenMenu?.FocusedButton.Name);
			Assert.IsTrue(Rect("Demo Tag").Visible);
		});
		TestReporter.Run("the title camera stands back and sways, with the callouts' rooms between the title and its menu", () =>
		{
			Assert.IsTrue(camera.Attract);
			Assert.AreEqual(1.0f, camera.AttractWeight);
			// The subtitle under the block letters ends the title; the callouts' column hangs from a point over the board.
			float title = ScreenRect("Subtitle").Max.Y;
			float menu = ScreenRect("Title Menu").Min.Y;
			float middle = ScreenRect("Callouts").Min.Y;
			float scale = CanvasScale(Window);
			foreach (CalloutSlot slot in System.Enum.GetValues<CalloutSlot>())
			{
				(float top, float bottom) = Callouts.Room(slot);
				float roomTop = middle + (top * scale);
				float roomBottom = middle + (bottom * scale);
				Assert.IsTrue(title < roomTop && roomBottom < menu, $"{slot}: room {roomTop} to {roomBottom}, title to {title}, menu from {menu}");
			}
		});
		TestReporter.Run("the title, its menu and the score and controls panels leave the board in sight on screens from 5:4 to 21:9", () =>
		{
			Assert.IsNull(FindCover(game, s_Screens, [.. s_Sign, .. s_TitleMenu, .. s_Panels]));
			// Through the bottom of the title the camera looks over the panels' tops (the highest part of the board), and through
			// the top of the menu under the board's base.
			CameraComponent view = game.Camera!.GetComponent<CameraComponent>()!;
			Entity panel = Entity.FindByName("Next Panel")!;
			Entity floor = Entity.FindByName("Floor")!;
			Vector3 panelCenter = panel.Transform.WorldTranslation;
			Vector3 floorCenter = floor.Transform.WorldTranslation;
			Area sign = ScreenRect("Subtitle");
			Area menu = ScreenRect("Title Menu");
			float over = Height(view.ScreenToWorldRay(new Vector2(sign.Center.X, sign.Max.Y)), panelCenter.Z + (0.5f * panel.Scale.Z));
			Assert.IsTrue(over > panelCenter.Y + (0.5f * panel.Scale.Y), $"the title reaches down to {over}");
			float under = Height(view.ScreenToWorldRay(new Vector2(menu.Center.X, menu.Min.Y)), floorCenter.Z + (0.5f * floor.Scale.Z));
			Assert.IsTrue(under < floorCenter.Y - (0.5f * floor.Scale.Y), $"the menu reaches up to {under}");
		});
		TestReporter.Run("a demo that tops out starts over without a game over", () =>
		{
			int best = game.BestScore;
			ForceGameOver(game);
			Assert.AreEqual(GameState.Title, game.State);
			Assert.IsTrue(game.IsDemo);
			Assert.AreEqual(0, CountCells(game.Board), "on an empty board");
			Assert.AreEqual(0, game.DemoPieces);
			Assert.AreEqual(0, game.Score);
			Assert.AreEqual(best, game.BestScore);
			Assert.AreEqual(game.DropSound, game.LastSound, "no game-over sound");
		});
	}

	// A four-row clear right after another, after a three-row clear: the biggest callout of every slot at once ("TETRIS!" with
	// its points, "BACK-TO-BACK" and "LEVEL UP!" with the level), and a row toward the level after. Returns the points of the
	// last clear.
	private static int ShowEveryCallout(BlocksGame game)
	{
		game.Restart();
		int points = 0;
		foreach (int rows in new[] { 3, 4, 4 })
		{
			FillRows(game.Board, 0, rows, Board.Width);
			int before = game.Score;
			game.ClearFullRows();
			points = game.Score - before;
		}
		return points;
	}

	// Checks the callouts that show this frame: each keeps to its slot's room (so none runs into another).
	private void WatchCallouts()
	{
		int showing = 0;
		foreach (CalloutSlot slot in System.Enum.GetValues<CalloutSlot>())
		{
			if (CalloutLines(slot) is not { } lines)
			{
				continue;
			}
			showing++;
			(float top, float bottom) = Callouts.Room(slot);
			// Half a canvas unit for rounding.
			if (m_CalloutTrouble is null && (lines.Top < top - 0.5f || lines.Bottom > bottom + 0.5f))
			{
				m_CalloutTrouble = $"frame {m_StepFrames}: the {slot} callout spans {lines.Top} to {lines.Bottom}, its room {top} to {bottom}";
			}
		}
		m_FramesWithEverySlot += showing == System.Enum.GetValues<CalloutSlot>().Length ? 1 : 0;
	}

	// The lines a slot's callout shows (its texts and their shadows), from the top of the first to the bottom of the last in
	// canvas units from the middle of the callouts' column; null when it shows none.
	private static (float Top, float Bottom)? CalloutLines(CalloutSlot slot)
	{
		if (Rect($"{slot} Callout").Opacity <= 0.0f)
		{
			return null;
		}
		(float Top, float Bottom)? lines = null;
		foreach (string part in new[] { "Text", "Text Shadow", "Detail", "Detail Shadow" })
		{
			UITextComponent text = UIText($"{slot} Callout {part}");
			if (text.Text.Length == 0)
			{
				continue;
			}
			// A line hangs from the top of its rectangle and is as tall as the font's size.
			float top = text.Entity.GetComponent<UIRectComponent>()!.Position.Y;
			float bottom = top + text.FontSize;
			lines = lines is { } before ? (Mathf.Min(before.Top, top), Mathf.Max(before.Bottom, bottom)) : (top, bottom);
		}
		return lines;
	}

	// Leaves every visible row full but for the side columns (so none clears, whatever the stack was), then drops the piece:
	// it settles where the next piece appears, which then has no room.
	private static void ForceGameOver(BlocksGame game)
	{
		game.Board.Clear();
		for (int y = 0; y < Board.Height; y++)
		{
			for (int x = 1; x < Board.Width - 1; x++)
			{
				game.Board.Set(x, y, PieceKind.O);
			}
		}
		game.HardDrop();
	}

	// Settles the empty cells in the column next to the active piece on the side it slides to, from two rows under it to its
	// top, and remembers them.
	private void BlockSlide(BlocksGame game, int direction)
	{
		List<(int X, int Y)> cells = game.Active.Cells.ToList();
		int column = direction > 0 ? cells.Max(cell => cell.X) + 1 : cells.Min(cell => cell.X) - 1;
		int top = cells.Max(cell => cell.Y);
		for (int y = System.Math.Max(cells.Min(cell => cell.Y) - 2, 0); y <= top; y++)
		{
			if (game.Board[column, y] is null)
			{
				game.Board.Set(column, y, PieceKind.Z);
				m_Blocked.Add((column, y));
			}
		}
	}

	// Settles the first columns of rows.
	private static void FillRows(Board board, int bottom, int count, int columns)
	{
		for (int y = bottom; y < bottom + count; y++)
		{
			for (int x = 0; x < columns; x++)
			{
				board.Set(x, y, PieceKind.I);
			}
		}
	}

	// A component of an element of the screen, found by its name.
	private static T Element<T>(string name)
		where T : Component
	{
		T? component = Entity.FindByName(name)?.GetComponent<T>();
		Assert.IsNotNull(component, $"{name} has a {typeof(T).Name}");
		return component;
	}

	private static UIRectComponent Rect(string name) => Element<UIRectComponent>(name);

	private static UITextComponent UIText(string name) => Element<UITextComponent>(name);

	// Clicks a button of the screen as the mouse does: at the start of the next frame.
	private static void Click(string button) => Entity.FindByName(button)?.GetComponent<UIButtonComponent>()?.Click();

	// An entity of the scene by its name, found once.
	private Entity Find(string name)
	{
		if (!m_Found.TryGetValue(name, out Entity? entity))
		{
			entity = Entity.FindByName(name);
			Assert.IsNotNull(entity, $"the scene has an entity named {name}");
			m_Found[name] = entity;
		}
		return entity;
	}

	// The window the checks run in, in pixels.
	private static Vector2 Window => new(Application.WindowWidth, Application.WindowHeight);

	// Pixels per canvas unit of the screen's canvas on a screen of the size: the canvas fits its whole reference resolution.
	private static float CanvasScale(Vector2 screen)
	{
		UICanvasComponent canvas = Element<UICanvasComponent>("HUD");
		Assert.AreEqual(UIScaleMode.ScaleWithScreenSize, canvas.ScaleMode);
		Vector2 reference = canvas.ReferenceResolution;
		return Mathf.Min(screen.X / reference.X, screen.Y / reference.Y);
	}

	// Where an element of the screen's canvas lies in the window, in pixels from its top-left corner.
	private Area ScreenRect(string name) => new Placed(Find(name)).On(Window, CanvasScale(Window));

	// How much of an element of the screen shows: its opacity times its ancestors', none when it or one of them is hidden.
	private float ShownOpacity(string name) => new Placed(Find(name)).Opacity;

	// Every frame: what of the title shows (its sign and its menu) leaves the board, its panels and their labels in sight, in
	// the window and on a wide screen, where the sign clears the board last while the camera glides to the title's view. The
	// frames of that glide in which the sign waited for the camera, and in which it faded in, are counted.
	private void WatchTitle(BlocksGame game, CameraShake camera)
	{
		if (ShownOpacity("Title Screen") <= 0.0f)
		{
			return;
		}
		if (game.State == GameState.Title && camera.AttractWeight < 1.0f)
		{
			bool shows = ShownOpacity("Title Sign") > 0.0f;
			m_SignWaited += shows ? 0 : 1;
			m_SignFadedIn += shows ? 1 : 0;
		}
		if (m_TitleTrouble is null && FindCover(game, [Window, s_WideScreen], [.. s_Sign, .. s_TitleMenu]) is { } cover)
		{
			m_TitleTrouble = $"{cover}, {camera.AttractWeight} of the way to the title's view";
		}
	}

	// The first element of the screen that covers a part of the board on one of the screens (in the window also the HOLD or
	// NEXT label the screen places over a panel), described; null when the elements that show leave all of them in sight.
	private string? FindCover(BlocksGame game, IEnumerable<Vector2> screens, IEnumerable<string> names)
	{
		List<(string Name, Placed Element)> elements = [];
		foreach (string name in names)
		{
			Placed element = new(Find(name));
			if (element.Opacity > 0.0f)
			{
				elements.Add((name, element));
			}
		}
		if (elements.Count == 0)
		{
			return null;
		}
		foreach (Vector2 screen in screens)
		{
			List<(string Name, Area Area)> board = [];
			using (ScreenView view = new(game, screen))
			{
				foreach (string part in s_BoardParts)
				{
					board.Add((part, view.Box(Find(part))));
				}
			}
			if (screen == Window)
			{
				foreach (string label in new[] { "Hold Label", "Next Label" })
				{
					// A label of the canvas of pixels: its position is its middle.
					UIRectComponent rect = Find(label).GetComponent<UIRectComponent>()!;
					if (rect.Visible)
					{
						board.Add((label, new Area(rect.Position - (0.5f * rect.Size), rect.Position + (0.5f * rect.Size))));
					}
				}
			}
			float scale = CanvasScale(screen);
			foreach ((string name, Placed element) in elements)
			{
				Area area = element.On(screen, scale);
				foreach ((string part, Area covered) in board)
				{
					if (area.Overlaps(covered))
					{
						return $"on a {screen.X} x {screen.Y} screen the {name} ({area}) covers the {part} ({covered})";
					}
				}
			}
		}
		return null;
	}

	// A rectangle of the screen, in pixels from its top-left corner.
	private readonly record struct Area(Vector2 Min, Vector2 Max)
	{
		public Vector2 Center => 0.5f * (Min + Max);

		public bool Overlaps(Area other) => Min.X < other.Max.X && other.Min.X < Max.X && Min.Y < other.Max.Y && other.Min.Y < Max.Y;

		public override string ToString() => $"{Min} to {Max}";
	}

	// An element of the screen's canvas as the UI places it, read once: its rectangle and those of its ancestors below the
	// canvas, laid out on screens of any size.
	private sealed class Placed
	{
		private readonly List<(Vector2 AnchorMin, Vector2 AnchorMax, Vector2 Pivot, Vector2 Position, Vector2 Size)> m_Chain = [];

		public Placed(Entity element)
		{
			Opacity = 1.0f;
			for (Entity? entity = element; entity is not null && !entity.HasComponent<UICanvasComponent>(); entity = entity.Parent)
			{
				UIRectComponent rect = entity.GetComponent<UIRectComponent>()!;
				Opacity *= rect.Visible ? rect.Opacity : 0.0f;
				m_Chain.Add((rect.AnchorMin, rect.AnchorMax, rect.Pivot, rect.Position, rect.Size));
			}
		}

		// How much of the element shows: its opacity times its ancestors', none when it or one of them is hidden.
		public float Opacity { get; }

		// Where the element lies on a screen of the size, in pixels from its top-left corner, at the canvas's scale there
		// (pixels per canvas unit): laid out as the UI lays it out, in its parent's rectangle by its anchors, pivot, position
		// and size.
		public Area On(Vector2 screen, float scale)
		{
			Area area = new(Vector2.Zero, screen);
			for (int i = m_Chain.Count - 1; i >= 0; i--)
			{
				(Vector2 anchorMin, Vector2 anchorMax, Vector2 pivot, Vector2 position, Vector2 size) = m_Chain[i];
				Vector2 parent = area.Max - area.Min;
				Vector2 spanStart = area.Min + (anchorMin * parent);
				Vector2 span = area.Min + (anchorMax * parent) - spanStart;
				Vector2 extent = span + (size * scale);
				Vector2 min = spanStart + (span * pivot) + (position * scale) - (extent * pivot);
				area = new Area(min, min + extent);
			}
			return area;
		}
	}

	// The camera showing the scene on a screen of a size: while the view lasts, the camera keeps that screen's aspect ratio
	// with the field of view its CameraShake fits to it; disposing the view gives the camera back to the window.
	private sealed class ScreenView : System.IDisposable
	{
		private readonly CameraComponent m_Camera;
		private readonly bool m_FixedAspectRatio;
		private readonly float m_AspectRatio;
		private readonly float m_FieldOfView;
		// Pixels of the screen per pixel of the window, across and down.
		private readonly Vector2 m_Scale;

		public ScreenView(BlocksGame game, Vector2 screen)
		{
			m_Camera = game.Camera!.GetComponent<CameraComponent>()!;
			m_FixedAspectRatio = m_Camera.FixedAspectRatio;
			m_AspectRatio = m_Camera.AspectRatio;
			m_FieldOfView = m_Camera.PerspectiveFOV;
			m_Camera.FixedAspectRatio = true;
			m_Camera.AspectRatio = screen.X / screen.Y;
			game.Camera!.As<CameraShake>()!.FitView();
			m_Scale = new Vector2(screen.X / Application.WindowWidth, screen.Y / Application.WindowHeight);
		}

		// The rectangle around where the camera shows a box (an entity that scales a cube one unit wide) on the screen.
		public Area Box(Entity box)
		{
			Matrix4 world = box.Transform.WorldTransform;
			Vector2 min = new(float.MaxValue);
			Vector2 max = new(float.MinValue);
			for (int corner = 0; corner < 8; corner++)
			{
				Vector3 local = new((corner & 1) == 0 ? -0.5f : 0.5f, (corner & 2) == 0 ? -0.5f : 0.5f, (corner & 4) == 0 ? -0.5f : 0.5f);
				Vector3 shown = m_Camera.WorldToScreenPoint(world.TransformPoint(local));
				Vector2 point = new(shown.X * m_Scale.X, shown.Y * m_Scale.Y);
				min = new Vector2(Mathf.Min(min.X, point.X), Mathf.Min(min.Y, point.Y));
				max = new Vector2(Mathf.Max(max.X, point.X), Mathf.Max(max.Y, point.Y));
			}
			return new Area(min, max);
		}

		public void Dispose()
		{
			m_Camera.FixedAspectRatio = m_FixedAspectRatio;
			m_Camera.AspectRatio = m_AspectRatio;
			m_Camera.PerspectiveFOV = m_FieldOfView;
		}
	}

	private static Vector2 Flat(Vector3 screen) => new(screen.X, screen.Y);

	// The height at which a ray from the camera crosses the plane at a depth (z).
	private static float Height(Ray ray, float depth) => ray.GetPoint((depth - ray.Origin.Z) / ray.Direction.Z).Y;

	private static int CountCells(Board board)
	{
		int cells = 0;
		for (int x = 0; x < Board.Width; x++)
		{
			for (int y = 0; y < Board.Height + Board.HiddenRows; y++)
			{
				cells += board[x, y] is null ? 0 : 1;
			}
		}
		return cells;
	}
}
