using System;
using System.Numerics;

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
/// once. It also tells when putting the piece aside brings a clearly better one.
///
/// The search works on rows of bits in buffers each player makes once, so choosing allocates nothing and takes a fraction
/// of a millisecond. Use a player from one thread at a time.</summary>
public sealed class AutoPlayer
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
	// How much better the piece that comes instead must do for the player to hold.
	private const double HoldMargin = 12.0;
	// The score of a next piece that has no room.
	private const double Blocked = -1.0e6;
	private const int WellColumn = Board.Width - 1;
	private const int Rows = Board.Height + Board.HiddenRows;
	private const int FullRow = (1 << Board.Width) - 1;
	private const int WellBit = 1 << WellColumn;
	// Four rotations, each at most at every column.
	private const int MaxCandidates = 4 * Board.Width;

	// The ways to reach each rotation: in place, a quarter turn either way, or a half turn.
	private static readonly int[] s_TurnOptions = [0, 1, -1, 2];

	private static readonly Shape[] s_Shapes = MakeShapes();

	// The board as rows of bits (bit x of row y: the cell (x, y) is settled): as it is, after the first piece, and after the
	// next one; with each column's height.
	private readonly int[] m_Rows = new int[Rows];
	private readonly int[] m_After = new int[Rows];
	private readonly int[] m_Scratch = new int[Rows];
	private readonly int[] m_Heights = new int[Board.Width];
	private readonly int[] m_AfterHeights = new int[Board.Width];
	private readonly int[] m_WellDepths = new int[Board.Width];
	private readonly Candidate[] m_First = new Candidate[MaxCandidates];
	private readonly Candidate[] m_Second = new Candidate[MaxCandidates];

	/// <summary>The best place for the piece, knowing the piece that comes after it (when given); null when the piece does
	/// not fit where it is.</summary>
	public Placement? Choose(Board board, Piece piece, PieceKind? next = null)
	{
		Read(board);
		return Search(piece, next, out _);
	}

	/// <summary>Whether to put the piece aside: true when the piece that comes instead (the held one, or the next one when
	/// none is held), followed by the piece that comes after it then, finds a place clearly better than this piece followed
	/// by the next one. False when the piece that comes instead has no room either.</summary>
	public bool ShouldHold(Board board, Piece piece, PieceKind? next, PieceKind instead, PieceKind? insteadNext)
	{
		Read(board);
		Search(piece, next, out double keep);
		Search(Board.Spawn(instead), insteadNext, out double swap);
		return swap > keep + HoldMargin;
	}

	private static Shape ShapeOf(PieceKind kind, int rotation) => s_Shapes[((int)kind * 4) + rotation];

	// Moves a shape's row of bits from its box to the board: the shape's box starts at column x.
	private static int Shift(int bits, int x) => x >= 0 ? bits << x : bits >> -x;

	private static Shape[] MakeShapes()
	{
		PieceKind[] kinds = Enum.GetValues<PieceKind>();
		Shape[] shapes = new Shape[kinds.Length * 4];
		foreach (PieceKind kind in kinds)
		{
			for (int rotation = 0; rotation < 4; rotation++)
			{
				shapes[((int)kind * 4) + rotation] = new Shape(kind, rotation);
			}
		}
		return shapes;
	}

	private void Read(Board board)
	{
		for (int y = 0; y < Rows; y++)
		{
			int row = 0;
			for (int x = 0; x < Board.Width; x++)
			{
				row |= board[x, y] is null ? 0 : 1 << x;
			}
			m_Rows[y] = row;
		}
	}

	// The best place for the piece on the board read last, with its score; null when the piece does not fit where it is.
	private Placement? Search(Piece piece, PieceKind? next, out double bestScore)
	{
		bestScore = double.NegativeInfinity;
		if (!Fits(m_Rows, ShapeOf(piece.Kind, piece.Rotation), piece.X, piece.Y))
		{
			return null;
		}
		FindHeights(m_Rows, m_Heights);
		bool keepWell = StackHeight(m_Rows, WellColumn) < WellStackHeight;
		int count = Reachable(m_Rows, m_Heights, piece, m_First);
		Placement? best = Best(count, keepWell, next, out bestScore);
		// Only the well was left: any place will do.
		return best is null && keepWell ? Best(count, false, next, out bestScore) : best;
	}

	private Placement? Best(int count, bool keepWell, PieceKind? next, out double bestScore)
	{
		Placement? best = null;
		bestScore = double.NegativeInfinity;
		for (int i = 0; i < count; i++)
		{
			Candidate candidate = m_First[i];
			if (!Rate(m_Rows, candidate.Landed, keepWell, m_After, out double move, out double board))
			{
				continue;
			}
			double score = (next is { } nextKind ? move + BestFollowUp(nextKind, keepWell) : move + board) - (MoveCost * candidate.Moves);
			if (score > bestScore)
			{
				bestScore = score;
				best = new Placement(candidate.Landed, candidate.Turns);
			}
		}
		return best;
	}

	// The best score of the next piece on the board the first one leaves (m_After): its move and the board after both.
	private double BestFollowUp(PieceKind next, bool keepWell)
	{
		Piece spawned = Board.Spawn(next);
		if (!Fits(m_After, ShapeOf(next, spawned.Rotation), spawned.X, spawned.Y))
		{
			return Blocked;
		}
		FindHeights(m_After, m_AfterHeights);
		int count = Reachable(m_After, m_AfterHeights, spawned, m_Second);
		double best = BestScore(count, keepWell);
		if (double.IsNegativeInfinity(best) && keepWell)
		{
			best = BestScore(count, false);
		}
		return double.IsNegativeInfinity(best) ? Blocked : best;
	}

	private double BestScore(int count, bool keepWell)
	{
		double best = double.NegativeInfinity;
		for (int i = 0; i < count; i++)
		{
			Candidate candidate = m_Second[i];
			if (Rate(m_After, candidate.Landed, keepWell, m_Scratch, out double move, out double board))
			{
				best = Math.Max(best, move + board - (MoveCost * candidate.Moves));
			}
		}
		return best;
	}

	// Every place the piece reaches by turning in place (with the board's kicks), then sliding along its row, then dropping,
	// into found; returns how many. A place reached several ways (turns that cover the same cells) counts once, with the
	// fewest moves.
	private static int Reachable(int[] rows, int[] heights, Piece piece, Candidate[] found)
	{
		int count = 0;
		foreach (int turns in s_TurnOptions)
		{
			int rotation = piece.Rotation;
			int x = piece.X;
			int y = piece.Y;
			bool turned = true;
			for (int i = 0; i < Math.Abs(turns) && turned; i++)
			{
				turned = TryTurn(rows, piece.Kind, Math.Sign(turns), ref rotation, ref x, ref y);
			}
			if (!turned)
			{
				continue;
			}
			Shape shape = ShapeOf(piece.Kind, rotation);
			Add(x, Math.Abs(turns));
			for (int direction = -1; direction <= 1; direction += 2)
			{
				int moves = Math.Abs(turns);
				for (int slid = x + direction; Fits(rows, shape, slid, y); slid += direction)
				{
					moves++;
					Add(slid, moves);
				}
			}

			void Add(int column, int moves)
			{
				int landed = Land(rows, heights, shape, column, y);
				long cells = CellsKey(shape, column, landed);
				for (int i = 0; i < count; i++)
				{
					if (found[i].Cells == cells)
					{
						if (moves < found[i].Moves)
						{
							found[i] = new Candidate(new Piece(piece.Kind, column, landed, rotation), turns, moves, cells);
						}
						return;
					}
				}
				found[count++] = new Candidate(new Piece(piece.Kind, column, landed, rotation), turns, moves, cells);
			}
		}
		return count;
	}

	// Turns the piece as the board does: in place, or kicked when it does not fit there. False when nothing fits.
	private static bool TryTurn(int[] rows, PieceKind kind, int quarterTurns, ref int rotation, ref int x, ref int y)
	{
		int turned = (rotation + quarterTurns) & 3;
		Shape shape = ShapeOf(kind, turned);
		if (Fits(rows, shape, x, y))
		{
			rotation = turned;
			return true;
		}
		foreach ((int kickX, int kickY) in Board.Kicks)
		{
			if (Fits(rows, shape, x + kickX, y + kickY))
			{
				rotation = turned;
				x += kickX;
				y += kickY;
				return true;
			}
		}
		return false;
	}

	// Whether the shape with its box at (x, y) lies inside the board on empty cells.
	private static bool Fits(int[] rows, Shape shape, int x, int y)
	{
		if (x + shape.Left < 0 || x + shape.Right >= Board.Width || y - shape.Bottom < 0 || y - shape.Top >= Rows)
		{
			return false;
		}
		for (int row = shape.Top; row <= shape.Bottom; row++)
		{
			if ((rows[y - row] & Shift(shape.Rows[row], x)) != 0)
			{
				return false;
			}
		}
		return true;
	}

	// The row of the box's top edge where the shape lands when it drops from (x, y). Above the stack in each of its columns
	// it comes to rest on their tops; under an overhang it drops row by row.
	private static int Land(int[] rows, int[] heights, Shape shape, int x, int y)
	{
		int landing = int.MinValue;
		for (int column = shape.Left; column <= shape.Right; column++)
		{
			int lowest = shape.Lowest[column];
			if (lowest < 0)
			{
				continue;
			}
			int height = heights[x + column];
			if (y - lowest < height)
			{
				while (Fits(rows, shape, x, y - 1))
				{
					y--;
				}
				return y;
			}
			landing = Math.Max(landing, height + lowest);
		}
		return landing;
	}

	// The cells the shape covers with its box at (x, y), as one number: the lowest row and the bits of the rows from there.
	private static long CellsKey(Shape shape, int x, int y)
	{
		long key = y - shape.Bottom;
		for (int row = shape.Bottom; row >= shape.Top; row--)
		{
			key |= (long)Shift(shape.Rows[row], x) << (5 + (Board.Width * (shape.Bottom - row)));
		}
		return key;
	}

	// The height of each column: one above its highest settled cell.
	private static void FindHeights(int[] rows, int[] heights)
	{
		Array.Clear(heights);
		int remaining = FullRow;
		for (int y = Rows - 1; y >= 0 && remaining != 0; y--)
		{
			int found = rows[y] & remaining;
			for (int bits = found; bits != 0; bits &= bits - 1)
			{
				heights[BitOperations.TrailingZeroCount(bits)] = y + 1;
			}
			remaining &= ~found;
		}
	}

	// The height of the highest of the first columns.
	private static int StackHeight(int[] rows, int columns)
	{
		int mask = (1 << columns) - 1;
		for (int y = Rows - 1; y >= 0; y--)
		{
			if ((rows[y] & mask) != 0)
			{
				return y + 1;
			}
		}
		return 0;
	}

	// Settles the piece where it landed on a copy of the rows (after, where its full rows are cleared) and rates the move
	// (how high it lands, the rows it clears, a kept well's bonus) and the board it leaves. False when it would fill a kept
	// well without clearing enough rows.
	private bool Rate(int[] rows, Piece landed, bool keepWell, int[] after, out double move, out double board)
	{
		Shape shape = ShapeOf(landed.Kind, landed.Rotation);
		Array.Copy(rows, after, Rows);
		int bottom = landed.Y - shape.Bottom;
		int top = landed.Y - shape.Top;
		// Eroded cells: the rows cleared times the piece's cells in them.
		int cleared = 0;
		int eroded = 0;
		bool inWell = false;
		for (int row = shape.Top; row <= shape.Bottom; row++)
		{
			int cells = Shift(shape.Rows[row], landed.X);
			int y = landed.Y - row;
			after[y] |= cells;
			inWell |= (cells & WellBit) != 0;
			if (after[y] == FullRow)
			{
				cleared++;
				eroded += BitOperations.PopCount((uint)cells);
			}
		}
		move = 0.0;
		board = 0.0;
		if (keepWell && inWell && cleared < WellRows)
		{
			return false;
		}
		if (cleared > 0)
		{
			ClearFullRows(after, bottom, top);
		}
		move = (LandingHeightWeight * (((bottom + top) / 2.0) + 1.0)) + (ErodedCellsWeight * cleared * eroded);
		if (keepWell && inWell)
		{
			move += WellBonus * cleared;
		}
		board = RateBoard(after, keepWell);
		return true;
	}

	// Removes the full rows between bottom and top; the rows above move down.
	private static void ClearFullRows(int[] rows, int bottom, int top)
	{
		int write = bottom;
		for (int read = bottom; read < Rows; read++)
		{
			if (read > top || rows[read] != FullRow)
			{
				rows[write++] = rows[read];
			}
		}
		Array.Clear(rows, write, Rows - write);
	}

	// The board's features: row and column transitions, holes and wells. A kept well counts as a wall.
	private double RateBoard(int[] rows, bool keepWell)
	{
		int columns = keepWell ? WellColumn : Board.Width;
		int mask = (1 << columns) - 1;
		int height = StackHeight(rows, columns);
		// Rows above the stack add nothing but the step from each column's top to the empty row above the stack.
		int top = Math.Min(height, Rows - 1);

		// Along each row between walls (bit 0 and bit columns + 1 of the walled row): a change between neighbors is a set
		// bit of the row and the row shifted by one.
		int walls = 1 | (1 << (columns + 1));
		int pairs = (1 << (columns + 1)) - 1;
		int rowTransitions = 0;
		for (int y = 0; y < height; y++)
		{
			int walled = ((rows[y] & mask) << 1) | walls;
			rowTransitions += BitOperations.PopCount((uint)((walled ^ (walled >> 1)) & pairs));
		}

		// Up each column from the floor, which counts as filled.
		int columnTransitions = 0;
		int below = mask;
		for (int y = 0; y <= top; y++)
		{
			int row = rows[y] & mask;
			columnTransitions += BitOperations.PopCount((uint)(row ^ below));
			below = row;
		}

		// Down from the top: holes under each column's highest cell, and wells (empty cells between filled neighbors or
		// walls), the deeper the costlier (1 + 2 + ... + depth).
		int holes = 0;
		int wells = 0;
		int covered = 0;
		int previousWells = 0;
		int rightWall = 1 << (columns - 1);
		for (int y = top; y >= 0; y--)
		{
			int row = rows[y] & mask;
			holes += BitOperations.PopCount((uint)(covered & ~row));
			covered |= row;
			int wellCells = ~row & ((row << 1) | 1) & ((row >> 1) | rightWall) & mask;
			for (int bits = wellCells; bits != 0; bits &= bits - 1)
			{
				int x = BitOperations.TrailingZeroCount(bits);
				m_WellDepths[x] = (previousWells & (1 << x)) != 0 ? m_WellDepths[x] + 1 : 1;
				wells += m_WellDepths[x];
			}
			previousWells = wellCells;
		}
		return (RowTransitionsWeight * rowTransitions) + (ColumnTransitionsWeight * columnTransitions) + (HolesWeight * holes) +
			   (WellSumsWeight * wells);
	}

	// A place a piece reaches: where it lands, the turns and moves it takes, and the cells it covers (CellsKey).
	private readonly record struct Candidate(Piece Landed, int Turns, int Moves, long Cells);

	// A kind's shape in a rotation as rows of bits: bit c of Rows[r] is the cell in column c and row r of its box (rows count
	// down from the top), with the columns and rows its cells span and the row of each column's lowest cell (-1: none).
	private sealed class Shape
	{
		public readonly int[] Rows = new int[4];
		public readonly int[] Lowest = [-1, -1, -1, -1];
		public readonly int Left = 4;
		public readonly int Right = -1;
		public readonly int Top = 4;
		public readonly int Bottom = -1;

		public Shape(PieceKind kind, int rotation)
		{
			foreach ((int column, int row) in Piece.ShapeCells(kind, rotation))
			{
				Rows[row] |= 1 << column;
				Lowest[column] = Math.Max(Lowest[column], row);
				Left = Math.Min(Left, column);
				Right = Math.Max(Right, column);
				Top = Math.Min(Top, row);
				Bottom = Math.Max(Bottom, row);
			}
		}
	}
}
