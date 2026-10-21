using System;

namespace Strada.VisualScripting;

// Marks a method of the scripting API as a pure visual-script node: a query without side effects, which graphs evaluate
// without exec pins, once for every node that consumes its result. A method returning void without out parameters is
// never pure, whatever it is marked with. (System.Diagnostics.Contracts.PureAttribute cannot serve: it is conditional on
// CONTRACTS_FULL and never reaches the assembly.) A query added to the API gets this attribute in the same change.
[AttributeUsage(AttributeTargets.Method, Inherited = false)]
internal sealed class PureNodeAttribute : Attribute
{
}
