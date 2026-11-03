namespace Strada.VisualScripting;

// The two ways the interpreter runs (spec-slice4 section 7.3): the JIT compiles each generic interpreter method once per
// mode and folds IsTraced away, so an untraced execution pays nothing per instruction for the debugger. The modes are
// structs: generic code over a struct type argument is compiled for that type alone, never shared.
internal interface ITraceMode
{
	static abstract bool IsTraced { get; }
}

// An execution no debugger traces: no stop points, no stamps, no reports.
internal readonly struct Untraced : ITraceMode
{
	public static bool IsTraced => false;
}

// An execution whose run modes (Execution.Mode) the attached debugger chose: Break checks stop points, Record and Values
// report steps and values, and every traced run stamps the frames it computes in.
internal readonly struct Traced : ITraceMode
{
	public static bool IsTraced => true;
}
