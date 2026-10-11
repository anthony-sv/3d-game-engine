namespace Strada;

/// <summary>How a <see cref="UICanvasComponent"/>'s units relate to the screen's pixels.</summary>
public enum UIScaleMode
{
	/// <summary>A canvas unit is a pixel: the UI keeps its size in pixels on every screen.</summary>
	ConstantPixelSize = 0,
	/// <summary>The canvas is <see cref="UICanvasComponent.ReferenceResolution"/>.Y units high on every screen: the UI grows
	/// and shrinks with the screen's height.</summary>
	ScaleWithScreenHeight = 1,
}

/// <summary>Where a <see cref="UITextComponent"/>'s text block sits in its rectangle.</summary>
public enum TextVerticalAlignment
{
	/// <summary>At the top edge.</summary>
	Top = 0,
	/// <summary>Centered between the top and bottom edges.</summary>
	Middle = 1,
	/// <summary>At the bottom edge.</summary>
	Bottom = 2,
}

/// <summary>The state a <see cref="UIButtonComponent"/> is drawn in.</summary>
public enum UIButtonState
{
	/// <summary>At rest.</summary>
	Normal = 0,
	/// <summary>Under the mouse.</summary>
	Hovered = 1,
	/// <summary>Held down by the mouse.</summary>
	Pressed = 2,
	/// <summary>Not interactable.</summary>
	Disabled = 3,
}
