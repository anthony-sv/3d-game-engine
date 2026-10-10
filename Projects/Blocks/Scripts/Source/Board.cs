using System;
using System.Collections.Generic;

namespace Blocks;

/// <summary>The seven tetrominoes.</summary>
public enum PieceKind
{
	I = 0,
	O,
	T,
	S,
	Z,
	J,
	L,
}

/// <summary>A piece on the board: its kind, the top-left cell of the 4x4 box its shape is drawn in, and its rotation
/// (clockwise quarter turns, 0 to 3). Pieces are values: moving or turning one makes another.</summary>
public readonly struct Piece : IEquatable<Piece>
{
	// The shapes in their four rotations, rows from the top of the 4x4 box (the usual orientation of each kind).
	private static readonly string[][][] s_Shapes =
	[
		[["....", "####", "....", "...."], ["..#.", "..#.", "..#.", "..#."], ["....", "....", "####", "...."], [".#..", ".#..", ".#..", ".#.."]],
		[[".##.", ".##.", "....", "...."], [".##.", ".##.", "....", "...."], [".##.", ".##.", "....", "...."], [".##.", ".##.", "....", "...."]],
		[[".#..", "###.", "....", "...."], [".#..", ".##.", ".#..", "...."], ["....", "###.", ".#..", "...."], [".#..", "##..", ".#..", "...."]],
		[[".##.", "##..", "....", "...."], [".#..", ".##.", "..#.", "...."], ["....", ".##.", "##..", "...."], ["#...", "##..", ".#..", "...."]],
		[["##..", ".##.", "....", "...."], ["..#.", ".##.", ".#..", "...."], ["....", "##..", ".##.", "...."], [".#..", "##..", "#...", "...."]],
		[["#...", "###.", "....", "...."], [".##.", ".#..", ".#..", "...."], ["....", "###.", "..#.", "...."], [".#..", ".#..", "##..", "...."]],
		[["..#.", "###.", "....", "...."], [".#..", ".#..", ".##.", "...."], ["....", "###.", "#...", "...."], ["##..", ".#..", ".#..", "...."]],
	];

	public Piece(PieceKind kind, int x, int y, int rotation)
	{
		Kind = kind;
		X = x;
		Y = y;
		Rotation = ((rotation % 4) + 4) % 4;
	}

	public PieceKind Kind { get; }
	/// <summary>The column of the shape box's left edge.</summary>
	public int X { get; }
	/// <summary>The row of the shape box's top edge (rows count up from the bottom of the board).</summary>
	public int Y { get; }
	public int Rotation { get; }

	/// <summary>The four board cells the piece covers.</summary>
	public IEnumerable<(int X, int Y)> Cells
	{
		get
		{
			string[] rows = s_Shapes[(int)Kind][Rotation];
			for (int row = 0; row < 4; row++)
			{
				for (int column = 0; column < 4; column++)
				{
					if (rows[row][column] == '#')
					{
						yield return (X + column, Y - row);
					}
				}
			}
		}
	}

	public Piece Moved(int columns, int rows) => new(Kind, X + columns, Y + rows, Rotation);

	public Piece Turned(int quarterTurns) => new(Kind, X, Y, Rotation + quarterTurns);

	public bool Equals(Piece other) => Kind == other.Kind && X == other.X && Y == other.Y && Rotation == other.Rotation;

	public override bool Equals(object? obj) => obj is Piece other && Equals(other);

	public override int GetHashCode() => HashCode.Combine(Kind, X, Y, Rotation);

	public static bool operator ==(Piece left, Piece right) => left.Equals(right);

	public static bool operator !=(Piece left, Piece right) => !left.Equals(right);
}

/// <summary>The rules of the board: 10 columns, 20 visible rows and hidden rows above them where pieces appear. Rows count
/// up from the bottom; settled cells remember the kind of the piece they came from.</summary>
public sealed class Board
{
	public const int Width = 10;
	public const int Height = 20;
	public const int HiddenRows = 4;

	// Where turns that do not fit in place are tried, in order: sideways, then up (kicks off a wall or the floor).
	private static readonly (int X, int Y)[] s_Kicks = [(-1, 0), (1, 0), (0, 1), (-2, 0), (2, 0), (0, 2)];

	private readonly PieceKind?[,] m_Cells = new PieceKind?[Width, Height + HiddenRows];

	/// <summary>The kind of the piece the settled cell came from, or null when it is empty.</summary>
	public PieceKind? this[int x, int y] => IsInside(x, y) ? m_Cells[x, y] : null;

	public static bool IsInside(int x, int y) => x >= 0 && x < Width && y >= 0 && y < Height + HiddenRows;

	/// <summary>Where new pieces appear: centered, their shape's top row in the first hidden row.</summary>
	public static Piece Spawn(PieceKind kind) => new(kind, 3, Height, 0);

	/// <summary>The points for clearing rows at once on a level.</summary>
	public static int Score(int rows, int level) => level * rows switch
	{
		1 => 100,
		2 => 300,
		3 => 500,
		4 => 800,
		_ => 0,
	};

	/// <summary>Whether the piece lies inside the board on empty cells.</summary>
	public bool Fits(Piece piece)
	{
		foreach ((int x, int y) in piece.Cells)
		{
			if (!IsInside(x, y) || m_Cells[x, y] is not null)
			{
				return false;
			}
		}
		return true;
	}

	/// <summary>The piece turned, or kicked sideways or up when it does not fit in place; null when nothing fits.</summary>
	public Piece? Turn(Piece piece, int quarterTurns)
	{
		Piece turned = piece.Turned(quarterTurns);
		if (Fits(turned))
		{
			return turned;
		}
		foreach ((int x, int y) in s_Kicks)
		{
			if (Fits(turned.Moved(x, y)))
			{
				return turned.Moved(x, y);
			}
		}
		return null;
	}

	/// <summary>Where the piece lands when it drops straight down.</summary>
	public Piece Drop(Piece piece)
	{
		while (Fits(piece.Moved(0, -1)))
		{
			piece = piece.Moved(0, -1);
		}
		return piece;
	}

	/// <summary>Settles the piece. False when all of it stays above the visible rows: the stack reached the top.</summary>
	public bool Lock(Piece piece)
	{
		bool visible = false;
		foreach ((int x, int y) in piece.Cells)
		{
			m_Cells[x, y] = piece.Kind;
			visible |= y < Height;
		}
		return visible;
	}

	/// <summary>Removes the full rows; the rows above move down. Returns how many were removed.</summary>
	public int ClearRows()
	{
		int cleared = 0;
		for (int y = 0; y < Height + HiddenRows; y++)
		{
			if (!IsFull(y))
			{
				// Rows above cleared ones move down by the count so far.
				if (cleared > 0)
				{
					for (int x = 0; x < Width; x++)
					{
						m_Cells[x, y - cleared] = m_Cells[x, y];
						m_Cells[x, y] = null;
					}
				}
				continue;
			}
			for (int x = 0; x < Width; x++)
			{
				m_Cells[x, y] = null;
			}
			cleared++;
		}
		return cleared;
	}

	public void Clear() => Array.Clear(m_Cells);

	/// <summary>Settles a cell directly (to set up situations, as tests do).</summary>
	public void Set(int x, int y, PieceKind? kind) => m_Cells[x, y] = kind;

	private bool IsFull(int y)
	{
		for (int x = 0; x < Width; x++)
		{
			if (m_Cells[x, y] is null)
			{
				return false;
			}
		}
		return true;
	}
}

/// <summary>The order pieces come in: each bag holds all seven kinds in random order, so no kind is missing for long.</summary>
public sealed class PieceBag
{
	private readonly List<PieceKind> m_Bag = [];
	private readonly Func<int, int, int> m_Range;

	/// <param name="range">A random whole number in [min, max), such as Strada.Random.Range.</param>
	public PieceBag(Func<int, int, int> range)
	{
		m_Range = range;
	}

	/// <summary>The kind that <see cref="Next"/> returns next.</summary>
	public PieceKind Peek()
	{
		Fill();
		return m_Bag[0];
	}

	public PieceKind Next()
	{
		Fill();
		PieceKind kind = m_Bag[0];
		m_Bag.RemoveAt(0);
		return kind;
	}

	private void Fill()
	{
		if (m_Bag.Count > 0)
		{
			return;
		}
		m_Bag.AddRange(Enum.GetValues<PieceKind>());
		// Fisher-Yates shuffle.
		for (int i = m_Bag.Count - 1; i > 0; i--)
		{
			int j = m_Range(0, i + 1);
			(m_Bag[i], m_Bag[j]) = (m_Bag[j], m_Bag[i]);
		}
	}
}
