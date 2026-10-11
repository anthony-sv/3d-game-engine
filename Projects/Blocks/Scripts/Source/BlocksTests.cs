using System.Collections.Generic;
using System.Linq;
using Strada;
using Strada.Testing;

namespace Blocks;

/// <summary>The checks of Scenes/Tests.sscene: the rules of the board and the auto player when the scene starts, then the
/// scene's game (Game) as it plays: moves, sounds, callouts, the camera, and the title screen's demo (the test scene starts
/// the game without the title screen and keeps no best score). The test run ends once they are done (play the scene, or
/// `StradaRuntime --project Blocks.sproj --scene Scenes/Tests.sscene --test`).</summary>
public sealed class BlocksTests : Script
{
	[Tooltip("The game the checks play")]
	public Entity? Game;

	// The frames at which the checks that need time to pass run, counted from the start: the play checks, then (when the
	// camera has calmed down) the lock and shake checks, then (when the callouts are gone) the demo.
	private const int PlayFrame = 20;
	private const int LockFrame = PlayFrame + 45;
	private const int FadeFrame = LockFrame + 150;
	private const int DemoFrame = FadeFrame + 240;

	private int m_Frame;
	private int m_StartRow;
	private int m_BestBeforeDemo;

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
			int lines = 0;
			int bigClears = 0;
			for (int placed = 0; placed < 100; placed++)
			{
				Piece piece = Board.Spawn(bag.Next());
				Placement? choice = AutoPlayer.Choose(board, piece, bag.Peek());
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
		TestReporter.Run("the auto player takes the clears the board offers", () =>
		{
			// Four rows full but for the rightmost column: a long piece goes down the well.
			Board board = new();
			for (int y = 0; y < 4; y++)
			{
				for (int x = 0; x < Board.Width - 1; x++)
				{
					board.Set(x, y, PieceKind.O);
				}
			}
			Placement? tetris = AutoPlayer.Choose(board, Board.Spawn(PieceKind.I), PieceKind.T);
			Assert.IsNotNull(tetris);
			Assert.IsTrue(tetris.Value.Landed.Cells.All(cell => cell.X == Board.Width - 1), "down the well");
			board.Lock(tetris.Value.Landed);
			Assert.AreEqual(4, board.ClearRows());
			// A row with a gap of two: the square fills it.
			board.Clear();
			for (int x = 2; x < Board.Width; x++)
			{
				board.Set(x, 0, PieceKind.L);
			}
			Placement? fill = AutoPlayer.Choose(board, Board.Spawn(PieceKind.O));
			Assert.IsNotNull(fill);
			board.Lock(fill.Value.Landed);
			Assert.AreEqual(1, board.ClearRows());
		});
		TestReporter.Run("the auto player gives up only when the piece has no room", () =>
		{
			Board board = new();
			for (int y = 0; y < Board.Height + Board.HiddenRows; y++)
			{
				for (int x = 0; x < Board.Width; x++)
				{
					board.Set(x, y, x % 2 == 0 ? PieceKind.Z : null);
				}
			}
			Assert.IsNull(AutoPlayer.Choose(board, Board.Spawn(PieceKind.T), PieceKind.I));
			board.Clear();
			Assert.IsNotNull(AutoPlayer.Choose(board, Board.Spawn(PieceKind.T)), "without knowing the next piece");
		});
	}

	protected override void OnUpdate(float deltaTime)
	{
		BlocksGame game = Game!.As<BlocksGame>()!;
		m_Frame++;
		switch (m_Frame)
		{
			case 1:
				TestReporter.Run("a game starts with a piece at the top", () =>
				{
					Assert.IsFalse(game.IsOver);
					Assert.IsFalse(game.IsDemo);
					Assert.AreEqual(0, game.Score);
					Assert.AreEqual(1, game.Level);
					Assert.IsTrue(game.Active.Y >= Board.Height - 1, game.Active.Y.ToString());
				});
				m_StartRow = game.Active.Y;
				break;
			case PlayFrame:
				CheckPlay(game);
				break;
			case LockFrame:
				TestReporter.Run("a landed piece settles after a moment, with a click", () =>
				{
					Assert.AreEqual(game.LockSound, game.LastSound);
					Assert.AreEqual(4, CountCells(game.Board));
					Assert.IsTrue(game.Active.Y > Board.Height / 2, "the next piece comes from the top");
				});
				CheckShake(game);
				break;
			case FadeFrame:
				TestReporter.Run("callouts fade away", () =>
				{
					Assert.IsNull(game.Callout(CalloutSlot.Clear));
					Assert.IsNull(game.Callout(CalloutSlot.Bonus));
					Assert.IsNull(game.Callout(CalloutSlot.Level));
				});
				m_BestBeforeDemo = game.BestScore;
				game.ShowTitle();
				break;
			case DemoFrame:
				TestReporter.Run("the title screen plays a demo game by itself", () =>
				{
					Assert.AreEqual(GameState.Title, game.State);
					Assert.IsTrue(game.IsDemo);
					Assert.IsTrue(game.DemoPieces >= 3, $"{game.DemoPieces} pieces settled");
					Assert.IsTrue(CountCells(game.Board) > 0, "the demo builds a stack");
					Assert.IsTrue(game.Score > 0, "the demo scores");
					Assert.AreEqual(m_BestBeforeDemo, game.BestScore, "demos do not count for the best score");
					Assert.IsTrue(game.Camera!.As<CameraShake>()!.Sway, "the camera sways over the demo");
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
				break;
			case DemoFrame + 1:
				TestReporter.Run("the camera stops swaying when a game starts", () =>
				{
					Assert.IsFalse(game.Camera!.As<CameraShake>()!.Sway);
				});
				TestReporter.Finish();
				Application.Quit();
				break;
		}
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
		TestReporter.Run("the game ends when the stack reaches the top, and starts again", () =>
		{
			for (int y = 0; y < Board.Height; y++)
			{
				game.Board.Set(0, y, PieceKind.I);
				game.Board.Set(Board.Width - 1, y, PieceKind.I);
				for (int x = 1; x < Board.Width - 1; x++)
				{
					game.Board.Set(x, y, x % 2 == 0 ? PieceKind.O : null);
				}
			}
			int score = game.Score;
			game.HardDrop();
			Assert.IsTrue(game.IsOver);
			Assert.AreEqual(GameState.Over, game.State);
			Assert.AreEqual(game.GameOverSound, game.LastSound);
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
			FillRows(game, 0, 1);
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
			FillRows(game, 0, 4);
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
				FillRows(game, 0, rows);
				Assert.AreEqual(rows, game.ClearFullRows());
				Assert.AreEqual(sound, game.LastSound, $"{rows} rows");
			}
		});
		TestReporter.Run("clears pop callouts and score combos and back-to-back clears", () =>
		{
			game.Restart();
			FillRows(game, 0, 2);
			Assert.AreEqual(2, game.ClearFullRows());
			Assert.AreEqual("DOUBLE", game.Callout(CalloutSlot.Clear));
			Assert.IsNull(game.Callout(CalloutSlot.Bonus));
			Assert.AreEqual(300, game.Score);
			// Clears in a row add 50 points per clear in a row and level.
			FillRows(game, 0, 4);
			Assert.AreEqual(4, game.ClearFullRows());
			Assert.AreEqual("TETRIS!", game.Callout(CalloutSlot.Clear));
			Assert.AreEqual("COMBO x2", game.Callout(CalloutSlot.Bonus));
			Assert.AreEqual(300 + 800 + 50, game.Score);
			// A four-row clear after another scores half again.
			FillRows(game, 0, 4);
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
			FillRows(game, 0, 1);
			float before = shake.Trauma;
			game.ClearFullRows();
			float single = shake.Trauma - before;
			game.Restart();
			FillRows(game, 0, 4);
			before = shake.Trauma;
			game.ClearFullRows();
			float tetris = shake.Trauma - before;
			Assert.IsTrue(single > 0.0f, $"a single shakes: {single}");
			Assert.IsTrue(tetris > 2.0f * single, $"a tetris shakes more: {tetris} after {single}");
		});
	}

	private static void FillRows(BlocksGame game, int bottom, int count)
	{
		for (int y = bottom; y < bottom + count; y++)
		{
			for (int x = 0; x < Board.Width; x++)
			{
				game.Board.Set(x, y, PieceKind.I);
			}
		}
	}

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
