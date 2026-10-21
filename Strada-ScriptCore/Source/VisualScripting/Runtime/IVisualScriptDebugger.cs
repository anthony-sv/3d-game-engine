namespace Strada.VisualScripting;

// The debug hook (spec section 5.5): an attached debugger chooses the executions it traces by entity (one graph serves
// many instances), then hears of every instruction they run and of every value their pure nodes compute. Breakpoints,
// stepping and the editor's debugger build on it (spec section 14). Without a debugger an instruction costs one null
// check more. Main thread only, like the runtime.
internal interface IVisualScriptDebugger
{
	// Whether executions of the entity's instance are traced: asked when an execution starts (an event, a Tick, a Send or
	// SendEvent) and when a fiber resumes.
	bool IsTarget(ulong entity);

	// A traced execution ran an instruction at the node (its document ID) and continues from the exec output linkTaken of
	// that node: the flow follows that output's link, if it has one. linkTaken is null when the flow leaves the node
	// otherwise: its chain ended there, the execution suspended there, or it entered the node's function. A call that
	// returns (at a Function.Return, or when its chain ends) is reported as its Function.Call continuing at Then. A
	// continuation taken from the flow stack is reported as its Sequence or loop node with the output it continues at, a
	// resumed fiber as its latent node with the continuation, and a Tick execution starts with its Tween or Interval and
	// the Tick output.
	void OnStep(GraphInstance instance, uint node, string? linkTaken);

	// A pure node of a traced execution computed the value of one of its outputs.
	void OnValue(GraphInstance instance, uint node, string pin, object? value);
}
