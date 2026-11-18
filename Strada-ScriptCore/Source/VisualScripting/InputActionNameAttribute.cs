using System;

namespace Strada.VisualScripting;

// Marks a string parameter of the scripting API that names an input action of the project (Input.GetAction's action): its
// node's input pin then names an action (PinDescription.NamesInputAction, the library's "InputAction"), which the analysis
// checks against the project's actions when it is told them (spec section 6 of IA2) and the editor offers the project's
// actions for. A member added to the API with such a parameter gets this attribute in the same change.
[AttributeUsage(AttributeTargets.Parameter, Inherited = false)]
internal sealed class InputActionNameAttribute : Attribute
{
}
