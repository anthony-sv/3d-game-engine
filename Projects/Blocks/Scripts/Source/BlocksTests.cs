using System.Collections.Generic;
using System.Linq;
using Strada;
using Strada.Testing;

namespace Blocks;

/// <summary>The checks of Scenes/Tests.sscene: the board's rules when the scene starts, then the scene's game (Game) as it
/// plays (the test scene starts it without the title screen and keeps no best score). The test run ends once they are done (play the scene, or `StradaRuntime --project Blocks.sproj --scene
/// Scenes/Tests.sscene --test`).</summary>
public sealed class BlocksTests : Script
{
	[Tooltip("The game the checks play")]
	public Entity? Game;

	private int m_Frame;
	private int m_StartRow;

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
			Assert.AreEqual(2, board.ClearRows());
			Assert.AreEqual(PieceKind.S, board[3, 0]);
			Assert.AreEqual(PieceKind.Z, board[5, 1]);
			Assert.IsNull(board[3, 1]);
			Assert.IsNull(board[5, 3]);
			Assert.AreEqual(0, board.ClearRows());
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
	}

	protected override void OnUpdate(float deltaTime)
	{
		BlocksGame game = Game!.As<BlocksGame>()!;
		m_Frame++;
		if (m_Frame == 1)
		{
			TestReporter.Run("a game starts with a piece at the top", () =>
			{
				Assert.IsFalse(game.IsOver);
				Assert.AreEqual(0, game.Score);
				Assert.AreEqual(1, game.Level);
				Assert.IsTrue(game.Active.Y >= Board.Height - 1, game.Active.Y.ToString());
			});
			m_StartRow = game.Active.Y;
			return;
		}
		if (m_Frame < 20)
		{
			return;
		}

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
			int settled = 0;
			for (int x = 0; x < Board.Width; x++)
			{
				for (int y = 0; y < 4; y++)
				{
					settled += game.Board[x, y] is null ? 0 : 1;
				}
			}
			Assert.AreEqual(4, settled);
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
			for (int x = 0; x < Board.Width; x++)
			{
				game.Board.Set(x, 0, PieceKind.T);
			}
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
			for (int y = 0; y < 4; y++)
			{
				for (int x = 0; x < Board.Width; x++)
				{
					game.Board.Set(x, y, PieceKind.I);
				}
			}
			Assert.AreEqual(4, game.ClearFullRows());
			Assert.AreEqual(Board.Score(4, 1), game.Score);
			CameraShake? shake = game.Camera?.As<CameraShake>();
			Assert.IsNotNull(shake);
			Assert.IsTrue(shake.Trauma > 0.5f, $"trauma {shake.Trauma}");
		});
		TestReporter.Finish();
		Application.Quit();
	}
}
