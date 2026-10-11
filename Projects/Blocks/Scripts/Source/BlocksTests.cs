using System.Collections.Generic;
using System.Linq;
using Strada;
using Strada.Testing;

namespace Blocks;

/// <summary>The checks of Scenes/Tests.sscene: the rules of the board, the auto player and the camera's spring when the
/// scene starts, then the scene's game (Game) as it plays: moves, sounds, callouts, the camera, the game-over screen and the
/// title screen's demo (the test scene starts the game without the title screen, keeps no best score and seeds its demo).
/// The test run ends once they are done (play the scene, or `StradaRuntime --project Blocks.sproj --scene
/// Scenes/Tests.sscene --test`).</summary>
public sealed class BlocksTests : Script
{
	[Tooltip("The game the checks play")]
	public Entity? Game;

	// How long the checks give the demo to do what they wait for (frames).
	private const int Patience = 600;

	// The steps of the run after the first frame: each waits for its frames or condition, runs its checks and starts the
	// next.
	private enum Step
	{
		Start,
		Play,
		Lock,
		Paused,
		Fade,
		TitleReturn,
		KeyWait,
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
	// The settled cells that block a demo piece's way, until the demo has planned again.
	private readonly List<(int X, int Y)> m_Blocked = [];
	// What the callout checks saw over the frames: the first callout out of its room, and the frames that showed callouts in
	// every slot at once.
	private string? m_CalloutTrouble;
	private int m_FramesWithEverySlot;

	protected override void OnCreate()
	{
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
	}

	protected override void OnUpdate(float deltaTime)
	{
		BlocksGame game = Game!.As<BlocksGame>()!;
		CameraShake camera = game.Camera!.As<CameraShake>()!;
		m_StepFrames++;
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
				m_StartRow = game.Active.Y;
				m_CameraPlace = camera.Translation;
				Next(Step.Play);
				break;
			case Step.Play when m_StepFrames == 19:
				CheckPlay(game);
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
				ShowEveryCallout(game);
				game.SetPaused(true);
				Next(Step.Paused);
				break;
			case Step.Paused when m_StepFrames == 150:
				TestReporter.Run("callouts wait out of sight while the game is paused", () =>
				{
					Assert.AreEqual("TETRIS!", game.Callout(CalloutSlot.Clear));
					Assert.AreEqual(0.0f, Text("Clear Callout").Color.A);
				});
				game.SetPaused(false);
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
				// The next game over goes back to the title after a quarter of a second.
				game.TitleReturnDelay = 0.25f;
				ForceGameOver(game);
				Next(Step.TitleReturn);
				break;
			case Step.TitleReturn when m_StepFrames == 30:
				TestReporter.Run("the game-over screen goes back to the title after a while", () =>
				{
					Assert.AreEqual(GameState.Title, game.State);
					Assert.IsTrue(game.IsDemo);
				});
				// Without a delay, the game-over screen waits for a key.
				game.TitleReturnDelay = 0.0f;
				game.Restart();
				ForceGameOver(game);
				Next(Step.KeyWait);
				break;
			case Step.KeyWait when m_StepFrames == 60:
				TestReporter.Run("with no delay the game-over screen waits for a key", () =>
				{
					Assert.AreEqual(GameState.Over, game.State);
				});
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
				CheckDemo(game, camera);
				Next(Step.GameStarted);
				break;
			case Step.GameStarted:
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
		TestReporter.Run("the title camera stands back and sways, with the callouts' rooms between the title's sign and the prompt", () =>
		{
			Assert.IsTrue(camera.Attract);
			Assert.AreEqual(1.0f, camera.AttractWeight);
			Assert.AreEqual("PRESS ENTER OR CLICK TO PLAY", Text("Prompt").Text);
			// The sign's glowing bottom edge (a sprite centered on its height) and the top of the prompt's line.
			Entity edge = Entity.FindByName("Title Sign Bottom")!;
			float sign = edge.Translation.Y + (0.5f * edge.Scale.Y / Application.WindowHeight);
			float prompt = Text("Prompt").Entity.Translation.Y;
			foreach (CalloutSlot slot in System.Enum.GetValues<CalloutSlot>())
			{
				(float top, float bottom) = Callouts.Room(slot);
				Assert.IsTrue(sign < top && bottom < prompt, $"{slot}: room {top} to {bottom}, sign to {sign}, prompt from {prompt}");
			}
		});
		TestReporter.Run("the title's sign and prompt leave the board and its panels in sight", () =>
		{
			// Across the sign's width, rays through its bottom edge pass over the panels' tops (the highest part of the board)
			// and rays through the top of the prompt's line pass under the board's base.
			CameraComponent view = game.Camera!.GetComponent<CameraComponent>()!;
			float width = Application.WindowWidth;
			float height = Application.WindowHeight;
			Entity sign = Entity.FindByName("Title Sign")!;
			Entity edge = Entity.FindByName("Title Sign Bottom")!;
			Entity panel = Entity.FindByName("Next Panel")!;
			Entity floor = Entity.FindByName("Floor")!;
			Vector3 panelCenter = panel.GetComponent<TransformComponent>()!.WorldTranslation;
			Vector3 floorCenter = floor.GetComponent<TransformComponent>()!.WorldTranslation;
			float signBottom = (edge.Translation.Y * height) + (0.5f * edge.Scale.Y);
			float promptTop = Text("Prompt").Entity.Translation.Y * height;
			for (int i = 0; i <= 8; i++)
			{
				float x = (sign.Translation.X * width) + (((i / 8.0f) - 0.5f) * sign.Scale.X);
				float over = Height(view.ScreenToWorldRay(new Vector2(x, signBottom)), panelCenter.Z + (0.5f * panel.Scale.Z));
				Assert.IsTrue(over > panelCenter.Y + (0.5f * panel.Scale.Y), $"the sign reaches down to {over} at x {x}");
				float under = Height(view.ScreenToWorldRay(new Vector2(x, promptTop)), floorCenter.Z + (0.5f * floor.Scale.Z));
				Assert.IsTrue(under < floorCenter.Y - (0.5f * floor.Scale.Y), $"the prompt reaches up to {under} at x {x}");
			}
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
		TestReporter.Run("starting from the title gives a real game on an empty board", () =>
		{
			// What Enter or a click does on the title screen.
			game.Restart();
			Assert.AreEqual(GameState.Playing, game.State);
			Assert.IsFalse(game.IsDemo);
			Assert.AreEqual(0, CountCells(game.Board));
			Assert.AreEqual(0, game.Score);
			Assert.AreEqual(0, game.Rows);
			Assert.IsNull(game.Held);
			Assert.AreEqual(game.StartSound, game.LastSound);
			Assert.AreEqual("GO!", game.Callout(CalloutSlot.Level));
		});
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

	// A four-row clear right after another, after a two-row clear: the biggest callout of every slot at once ("TETRIS!" with
	// its points, "BACK-TO-BACK" and "LEVEL UP!" with the level).
	private static void ShowEveryCallout(BlocksGame game)
	{
		game.Restart();
		foreach (int rows in new[] { 2, 4, 4 })
		{
			FillRows(game.Board, 0, rows, Board.Width);
			game.ClearFullRows();
		}
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
			// A thousandth of the window's height for rounding.
			if (m_CalloutTrouble is null && (lines.Top < top - 0.001f || lines.Bottom > bottom + 0.001f))
			{
				m_CalloutTrouble = $"frame {m_StepFrames}: the {slot} callout spans {lines.Top} to {lines.Bottom}, its room {top} to {bottom}";
			}
		}
		m_FramesWithEverySlot += showing == System.Enum.GetValues<CalloutSlot>().Length ? 1 : 0;
	}

	// The lines a slot's callout shows (its texts and their shadows that are not transparent), from the top of the first to
	// the bottom of the last as fractions of the window's height; null when it shows none.
	private static (float Top, float Bottom)? CalloutLines(CalloutSlot slot)
	{
		(float Top, float Bottom)? lines = null;
		foreach (string part in new[] { "", " Shadow", " Detail", " Detail Shadow" })
		{
			TextComponent text = Text($"{slot} Callout{part}");
			if (text.Text.Length == 0 || text.Color.A <= 0.0f)
			{
				continue;
			}
			// Texts hang from their top; a line is as tall as the font's size.
			float top = text.Entity.Translation.Y;
			float bottom = top + (text.FontSize / Application.WindowHeight);
			lines = lines is { } before ? (Mathf.Min(before.Top, top), Mathf.Max(before.Bottom, bottom)) : (top, bottom);
		}
		return lines;
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

	private static TextComponent Text(string name) => Entity.FindByName(name)!.GetComponent<TextComponent>()!;

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
