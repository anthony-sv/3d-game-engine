using System.Collections.Generic;

namespace Strada.VisualScripting;

// A pin renamed by a redirect or reported by analysis for a node resolved inexactly (spec sections 4.5 and 6.2).
internal readonly record struct PinRename(PinDirection Direction, string From, string To);

// An entry of the redirect table: the current type string and pin names of a node type that was renamed or re-signed
// (spec section 4.5, resolution step 2). Settings keys never change across redirects.
internal sealed record NodeRedirect(string OldType, string NewType, IReadOnlyList<PinRename> PinRenames);

// The redirect table of the scripting API. Renaming or re-signing a member of the API adds an entry here in the same
// change, so graphs that use the old member keep resolving.
internal static class NodeRedirects
{
	internal static IReadOnlyList<NodeRedirect> Current { get; } = [];
}
