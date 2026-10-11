using System;
using System.Collections.Generic;

namespace Blocks;

/// <summary>A place the auto player chose for a piece: where it lands, reached from where the piece was by
/// <see cref="Turns"/> quarter turns in place (negative turns go counter-clockwise), then sliding sideways to the landing
/// column, then dropping.</summary>
public readonly record struct Placement(Piece Landed, int Turns);

/// <summary>Plays the board by itself, as the title screen's attract mode does. For the current piece it tries every place
/// it can reach from where the piece is (turning in place, sliding sideways, dropping), and for each of them every place of
/// the next piece; it rates the boards that result by Pierre Dellacherie's features with Islam El-Ashi's weights (how high
/// the piece lands, the rows it clears, the changes between filled and empty cells along rows and columns, holes and
/// wells). While the stack is low it keeps the rightmost column open as a well, where a piece clears three or four rows at
/// once.</summary>
public static class AutoPlayer
{
	// Below this stack height the well is kept open; above it any row clear will do.
	private const int WellStackHeight = 10;
	// The fewest rows a piece in the kept well must clear, and the bonus per row it clears.
	private const int WellRows = 3;
	private const double WellBonus = 2.0;
	private const double LandingHeightWeight = -4.500158825082766;
	private const double ErodedCellsWeight = 3.4181268101392694;
	private const double RowTransitionsWeight = -3.2178882868487753;
	private const double ColumnTransitionsWeight = -9.348695305445199;
	private const double HolesWeight = -7.899265427351652;
	private const double WellSumsWeight = -3.3855972247263626;
	// Between equally good places, the one with fewer moves.
	private const double MoveCost = 0.01;
	// The score of a next piece that has no room.
	private const double Blocked = -1.0e6;
	private const int WellColumn = Board.Width - 1;
	private const int Rows = Board.Height + Board.HiddenRows;

	// The ways to reach each rotation: in place, a quarter turn either way, or a half turn.
	private static readonly int[] s_TurnOptions = [0, 1, -1, 2];

	private readonly record struct Rating(double Score, double MoveScore, Board After);

	/// <summary>The best place for the piece, knowing the piece that comes after it (when given); null when the piece does
	/// not fit where it is.</summary>
	public static Placement? Choose(Board board, Piece piece, PieceKind? next = null)
	{
		if (!board.Fits(piece))
		{
			return null;
		}
		bool keepWell = StackHeight(board, WellColumn) < WellStackHeight;
		List<(Placement Placement, int Moves)> reachable = Reachable(board, piece);
		Placement? best = Best(board, reachable, keepWell, next);
		// Only the well was left: any place will do.
		return best is null && keepWell ? Best(board, reachable, false, next) : best;
	}

	private static Placement? Best(Board board, List<(Placement Placement, int Moves)> reachable, bool keepWell, PieceKind? next)
	{
		Placement? best = null;
		double bestScore = double.NegativeInfinity;
		foreach ((Placement placement, int moves) in reachable)
		{
			if (Rate(board, placement.Landed, keepWell) is not { } rating)
			{
				continue;
			}
			double score = (next is { } nextKind ? rating.MoveScore + BestFollowUp(rating.After, nextKind, keepWell) : rating.Score) -
						   (MoveCost * moves);
			if (score > bestScore)
			{
				bestScore = score;
				best = placement;
			}
		}
		return best;
	}

	// The best score of the next piece on the board the first one leaves: its move and the board after both.
	private static double BestFollowUp(Board board, PieceKind next, bool keepWell)
	{
		Piece spawned = Board.Spawn(next);
		if (!board.Fits(spawned))
		{
			return Blocked;
		}
		List<(Placement Placement, int Moves)> reachable = Reachable(board, spawned);
		double best = BestScore(board, reachable, keepWell);
		if (double.IsNegativeInfinity(best) && keepWell)
		{
			best = BestScore(board, reachable, false);
		}
		return double.IsNegativeInfinity(best) ? Blocked : best;
	}

	private static double BestScore(Board board, List<(Placement Placement, int Moves)> reachable, bool keepWell)
	{
		double best = double.NegativeInfinity;
		foreach ((Placement placement, int moves) in reachable)
		{
			if (Rate(board, placement.Landed, keepWell) is { } rating)
			{
				best = Math.Max(best, rating.Score - (MoveCost * moves));
			}
		}
		return best;
	}

	// Every place the piece reaches by turning in place, then sliding along its row, then dropping; with the moves it takes.
	private static List<(Placement Placement, int Moves)> Reachable(Board board, Piece piece)
	{
		List<(Placement, int)> found = [];
		HashSet<Piece> seen = [];
		foreach (int turns in s_TurnOptions)
		{
			Piece? turned = piece;
			for (int i = 0; i < Math.Abs(turns) && turned is { } current; i++)
			{
				turned = board.Turn(current, Math.Sign(turns));
			}
			if (turned is not { } start)
			{
				continue;
			}
			Add(start, Math.Abs(turns));
			for (int direction = -1; direction <= 1; direction += 2)
			{
				Piece slid = start;
				int moves = Math.Abs(turns);
				while (board.Fits(slid.Moved(direction, 0)))
				{
					slid = slid.Moved(direction, 0);
					moves++;
					Add(slid, moves);
				}
			}

			void Add(Piece position, int moves)
			{
				Piece landed = board.Drop(position);
				if (seen.Add(landed))
				{
					found.Add((new Placement(landed, turns), moves));
				}
			}
		}
		return found;
	}

	// The score of settling the piece where it landed; null when it would fill a kept well without clearing enough rows.
	private static Rating? Rate(Board board, Piece landed, bool keepWell)
	{
		Board after = board.Copy();
		after.Lock(landed);
		int bottom = int.MaxValue;
		int top = int.MinValue;
		bool inWell = false;
		foreach ((int x, int y) in landed.Cells)
		{
			bottom = Math.Min(bottom, y);
			top = Math.Max(top, y);
			inWell |= x == WellColumn;
		}
		// Eroded cells: the rows cleared times the piece's cells in them.
		int cleared = 0;
		int eroded = 0;
		foreach ((int _, int y) in landed.Cells)
		{
			eroded += after.IsFull(y) ? 1 : 0;
		}
		for (int y = bottom; y <= top; y++)
		{
			cleared += after.IsFull(y) ? 1 : 0;
		}
		if (keepWell && inWell && cleared < WellRows)
		{
			return null;
		}
		after.ClearRows();

		double move = (LandingHeightWeight * (((bottom + top) / 2.0) + 1.0)) + (ErodedCellsWeight * cleared * eroded);
		if (keepWell && inWell)
		{
			move += WellBonus * cleared;
		}
		return new Rating(move + RateBoard(after, keepWell), move, after);
	}

	// The board's features: row and column transitions, holes and wells. A kept well counts as a wall.
	private static double RateBoard(Board board, bool keepWell)
	{
		int columns = keepWell ? WellColumn : Board.Width;
		// Rows above the stack add nothing but the step from each column's top to the empty row above the stack.
		int height = StackHeight(board, columns);
		int top = Math.Min(height, Rows - 1);

		int rowTransitions = 0;
		for (int y = 0; y < height; y++)
		{
			bool previous = true;
			for (int x = 0; x <= columns; x++)
			{
				bool filled = x == columns || board[x, y] is not null;
				rowTransitions += filled != previous ? 1 : 0;
				previous = filled;
			}
		}

		int columnTransitions = 0;
		int holes = 0;
		int wells = 0;
		for (int x = 0; x < columns; x++)
		{
			// Up from the floor, which counts as filled.
			bool previous = true;
			for (int y = 0; y <= top; y++)
			{
				bool filled = board[x, y] is not null;
				columnTransitions += filled != previous ? 1 : 0;
				previous = filled;
			}
			// Down from the top: holes under the column's highest cell, and wells between filled neighbors (or walls), the
			// deeper the costlier (1 + 2 + ... + depth).
			bool covered = false;
			int depth = 0;
			for (int y = top; y >= 0; y--)
			{
				bool filled = board[x, y] is not null;
				covered |= filled;
				holes += covered && !filled ? 1 : 0;
				bool left = x == 0 || board[x - 1, y] is not null;
				bool right = x == columns - 1 || board[x + 1, y] is not null;
				depth = !filled && left && right ? depth + 1 : 0;
				wells += depth;
			}
		}
		return (RowTransitionsWeight * rowTransitions) + (ColumnTransitionsWeight * columnTransitions) + (HolesWeight * holes) +
			   (WellSumsWeight * wells);
	}

	// The height of the highest of the first columns.
	private static int StackHeight(Board board, int columns)
	{
		int height = 0;
		for (int x = 0; x < columns; x++)
		{
			for (int y = Rows - 1; y >= height; y--)
			{
				if (board[x, y] is not null)
				{
					height = y + 1;
					break;
				}
			}
		}
		return height;
	}
}
