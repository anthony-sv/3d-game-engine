"""Writes Differential.json beside this file: the curve cases that the engine's evaluator (StradaTests, Math/CurveTests.cpp)
and the scripting runtime's (ScriptCoreTests, CurveMathTests.cs) must both evaluate to its values.

    python Tests/Data/Curves/MakeDifferential.py [output file]

The expected values come from a third evaluator of the curve semantics (Docs/Architecture.md section 6.5), written here
apart from both evaluators under test, so that the differential test never compares an evaluator with itself: the keys'
numbers are rounded to float as a .scurve file's are, everything is computed in double, and the result is rounded to
float once. Most cases also carry values derived by hand, and the tool stops when its evaluator disagrees with one. A
change to the semantics changes both evaluators, this file's evaluator and the values (run it again) together. Standard
library only; two runs write the same bytes.
"""
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(HERE, "Differential.json")

ABOUT = ("Curve cases that the engine's evaluator (StradaTests, Math/CurveTests.cpp) and the scripting runtime's "
         "(ScriptCoreTests, CurveMathTests.cs) evaluate to the same values: each Curve is the content of a .scurve file, "
         "each sample's Value is the curve at its Time, within Tolerance times the larger of 1 and the value's magnitude. "
         "Non-finite numbers are the strings NaN, Infinity and -Infinity. Written by MakeDifferential.py beside it, whose "
         "evaluator is apart from both.")
TOLERANCE = 1e-6


def to_float(value):
    """The float nearest a double, as C++ and C# round a double to float (out of range: an infinity)."""
    if math.isnan(value) or math.isinf(value):
        return value
    try:
        return struct.unpack("f", struct.pack("f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


class Key:
    """A curve key as a .scurve file holds it: its numbers are floats; a slope of None is automatic."""

    def __init__(self, time, value, interpolation="Cubic", in_slope=None, out_slope=None):
        self.time = to_float(time)
        self.value = to_float(value)
        self.interpolation = interpolation
        self.in_slope = None if in_slope is None else to_float(in_slope)
        self.out_slope = None if out_slope is None else to_float(out_slope)
        self.written = (time, value, interpolation, in_slope, out_slope)

    def to_json(self):
        time, value, interpolation, in_slope, out_slope = self.written
        key = {"Time": time, "Value": value}
        if interpolation != "Cubic":
            key["Interpolation"] = interpolation
        if in_slope is not None:
            key["HasIn"] = True
            key["In"] = in_slope
        if out_slope is not None:
            key["HasOut"] = True
            key["Out"] = out_slope
        return key


def key_slopes(keys, index):
    """A key's in and out slopes: the set ones, else Catmull-Rom's from its neighbours (0 at the ends)."""
    automatic = 0.0
    if 0 < index < len(keys) - 1:
        span = keys[index + 1].time - keys[index - 1].time
        if span > 0.0:
            automatic = (keys[index + 1].value - keys[index - 1].value) / span
    key = keys[index]
    return (key.in_slope if key.in_slope is not None else automatic, key.out_slope if key.out_slope is not None else automatic)


def last_key_at_or_before(keys, time):
    low, high = 0, len(keys) - 1
    while low < high:
        middle = low + (high - low + 1) // 2
        if keys[middle].time <= time:
            low = middle
        else:
            high = middle - 1
    return low


def evaluate_between_keys(keys, time):
    """The curve within its keys' span (or at its ends)."""
    if len(keys) == 1 or time < keys[0].time:
        return keys[0].value
    index = last_key_at_or_before(keys, time)
    if index >= len(keys) - 1:
        return keys[-1].value
    start, end = keys[index], keys[index + 1]
    span = end.time - start.time
    s = (time - start.time) / span
    if start.interpolation == "Constant":
        return start.value
    if start.interpolation == "Linear":
        return start.value + (end.value - start.value) * s
    s2 = s * s
    s3 = s2 * s
    h00 = 2.0 * s3 - 3.0 * s2 + 1.0
    h10 = s3 - 2.0 * s2 + s
    h01 = -2.0 * s3 + 3.0 * s2
    h11 = s3 - s2
    out_slope = key_slopes(keys, index)[1]
    in_slope = key_slopes(keys, index + 1)[0]
    return h00 * start.value + h10 * span * out_slope + h01 * end.value + h11 * span * in_slope


def segment_slope(keys, index, at_start):
    start, end = keys[index], keys[index + 1]
    if start.interpolation == "Constant":
        return 0.0
    if start.interpolation == "Linear":
        return (end.value - start.value) / (end.time - start.time)
    return key_slopes(keys, index)[1] if at_start else key_slopes(keys, index + 1)[0]


def first_slope(keys):
    """The slope the curve leaves its first time with (past keys at that time); 0 for keys all at one time."""
    start = 0
    while start + 1 < len(keys) and keys[start + 1].time == keys[0].time:
        start += 1
    return 0.0 if start + 1 >= len(keys) else segment_slope(keys, start, True)


def last_slope(keys):
    end = len(keys) - 1
    while end > 0 and keys[end - 1].time == keys[-1].time:
        end -= 1
    return 0.0 if end == 0 else segment_slope(keys, end - 1, False)


def evaluate(keys, pre, post, time):
    if not keys:
        return 0.0
    if math.isnan(time):
        return math.nan
    first, last = keys[0].time, keys[-1].time
    if time < first or time > last:
        before = time < first
        end = keys[0] if before else keys[-1]
        period = last - first
        mode = pre if before else post
        if mode == "Hold":
            return end.value
        if mode in ("Loop", "PingPong"):
            if not period > 0.0:
                return end.value
            if math.isinf(time):
                return math.nan
            offset = time - first
            if mode == "Loop":
                phase = math.fmod(offset, period)
                if phase < 0.0:
                    phase += period
            else:
                cycle = 2.0 * period
                phase = math.fmod(offset, cycle)
                if phase < 0.0:
                    phase += cycle
                if phase > period:
                    phase = cycle - phase
            time = first + phase
        else:
            slope = first_slope(keys) if before else last_slope(keys)
            if slope == 0.0:
                return end.value
            return to_float(end.value + slope * (time - end.time))
    return to_float(evaluate_between_keys(keys, time))


def to_json_number(value):
    if math.isnan(value):
        return "NaN"
    if math.isinf(value):
        return "Infinity" if value > 0 else "-Infinity"
    return value


def shortest_float_text(value):
    """The shortest decimal that reads back as the float value (expected values are floats)."""
    if value == int(value) and abs(value) < 1e15:
        return float(int(value))
    for digits in range(1, 18):
        text = "%.*g" % (digits, value)
        if to_float(float(text)) == value:
            return float(text)
    return value


def make_cases():
    cases = []

    def case(name, keys, times, pre="Hold", post="Hold", expect=None):
        samples = []
        for index, time in enumerate(times):
            value = evaluate(keys, pre, post, time)
            if expect is not None and expect[index] is not None:
                derived = expect[index]
                if not ((math.isnan(derived) and math.isnan(value)) or value == derived):
                    sys.exit(f"{name} at {time}: {value} is not the value derived by hand, {derived}")
            samples.append({"Time": to_json_number(time),
                            "Value": to_json_number(value) if not math.isfinite(value) else shortest_float_text(value)})
        content = {"PreExtrapolation": pre, "PostExtrapolation": post, "Keys": [key.to_json() for key in keys]}
        cases.append({"Name": name, "Curve": content, "Samples": samples})

    inf = math.inf
    nan = math.nan
    # The visual script timeline's cases (VisualScriptTimelineTests.cpp, TimelineTests.cs).
    case("Automatic slopes between two keys", [Key(0, 0), Key(1, 1)], [0.25, 0.5, 0.75, 1.0], expect=[0.15625, 0.5, 0.84375, 1.0])
    case("Linear", [Key(0, 0, "Linear"), Key(1, 1)], [0.25, 0.5, 0.75, 1.0], expect=[0.25, 0.5, 0.75, 1.0])
    case("Constant", [Key(0, 0, "Constant"), Key(1, 1)], [0.25, 0.5, 0.75, 1.0], expect=[0.0, 0.0, 0.0, 1.0])
    case("Set slopes", [Key(0, 0, out_slope=2), Key(1, 1, in_slope=0)], [0.5], expect=[0.75])
    case("Catmull-Rom on uneven values", [Key(0, 0), Key(1, 1), Key(2, 4)], [-1.0, 0.5, 1.5, 3.0], expect=[0.0, 0.25, 2.75, 4.0])
    case("A jump between linear segments", [Key(0, 0, "Linear"), Key(1, 1), Key(1, 5, "Linear"), Key(2, 7)],
         [0.5, 0.999, 1.0, 1.5, 2.0], expect=[0.5, None, 5.0, 6.0, 7.0])
    # Ends and degenerate curves.
    case("No key", [], [-1.0, 0.0, 5.0, nan, inf], pre="Loop", post="Linear", expect=[0.0, 0.0, 0.0, 0.0, 0.0])
    case("One key holds", [Key(2, 3)], [-10.0, 2.0, 10.0, nan, -inf], expect=[3.0, 3.0, 3.0, nan, 3.0])
    case("One key under Linear and Loop", [Key(2, 3, out_slope=5)], [-10.0, 10.0, inf], pre="Linear", post="Loop",
         expect=[3.0, 3.0, 3.0])
    case("Keys at one time loop as they hold", [Key(1, 2, "Linear"), Key(1, 6)], [0.0, 1.0, 4.0], pre="Loop", post="PingPong",
         expect=[2.0, 6.0, 6.0])
    case("Hold at infinite times", [Key(0, 1, "Linear"), Key(1, 3)], [-inf, inf, nan], expect=[1.0, 3.0, nan])
    # Extrapolation.
    case("Loop a triangle", [Key(0, 0, "Linear"), Key(1, 1, "Linear"), Key(2, 0)],
         [2.5, 3.0, 4.0, 10.25, -0.5, -2.0, -3.75, 1000000.5, inf, -inf], pre="Loop", post="Loop",
         expect=[0.5, 1.0, 0.0, 0.25, 0.5, 0.0, 0.25, 0.5, nan, nan])
    case("PingPong", [Key(1, 10, "Linear"), Key(3, 20)], [3.5, 4.0, 5.0, 7.5, 0.0, -1.0, -2.5, inf], pre="PingPong",
         post="PingPong", expect=[17.5, 15.0, 10.0, 17.5, 15.0, 20.0, 12.5, nan])
    case("Linear along linear segments", [Key(0, 1, "Linear"), Key(2, 5)], [-1.0, 3.0, 102.0, inf, -inf], pre="Linear",
         post="Linear", expect=[-1.0, 7.0, 205.0, inf, -inf])
    case("Linear along set cubic slopes", [Key(0, 0, out_slope=3), Key(1, 1, in_slope=-1)], [-2.0, 0.5, 3.0], pre="Linear",
         post="Linear", expect=[-6.0, 1.0, -1.0])
    case("Linear along automatic slopes is flat", [Key(0, 0), Key(1, 1)], [-1.0, 5.0, inf], pre="Linear", post="Linear",
         expect=[0.0, 1.0, 1.0])
    case("Linear after a constant segment is flat", [Key(0, 0, "Constant"), Key(1, 4)], [-1.0, 2.0], pre="Linear",
         post="Linear", expect=[0.0, 4.0])
    case("Linear from jumps at both ends", [Key(0, 0), Key(0, 2, "Linear"), Key(1, 3, "Linear"), Key(1, 9)],
         [-1.0, 0.0, 0.5, 1.0, 2.0], pre="Linear", post="Linear", expect=[-1.0, 2.0, 2.5, 9.0, 10.0])
    case("Hold before, Loop after", [Key(0.25, 1), Key(0.75, -1, "Linear"), Key(1.5, 2)], [0.0, 1.75, 2.0, 2.4], pre="Hold",
         post="Loop")
    case("Loop before, Linear after", [Key(-2, 1, "Linear"), Key(3, 6)], [-8.0, -7.5, 4.0], pre="Loop", post="Linear",
         expect=[5.0, 5.5, 7.0])
    # Curves of the kinds games use: uneven keys, slopes set on some, numbers that are not exact in binary.
    case("Catmull-Rom on uneven times", [Key(0, 0), Key(0.5, 2), Key(2, 1), Key(3, 3)], [0.1, 0.25, 0.7, 1.3, 2.2, 2.9])
    case("A torque curve over revolutions per minute",
         [Key(1000, 50), Key(3500, 110, out_slope=0.01), Key(6000, 140), Key(8000, 120, "Linear", in_slope=-0.02), Key(9000, 0)],
         [1200.0, 2750.0, 4999.9, 7600.0, 8800.0, 9500.0], post="Linear")
    case("A response curve on negative inputs",
         [Key(-1, -1, in_slope=3, out_slope=3), Key(-0.1, 0, "Linear"), Key(0.1, 0), Key(1, 1, in_slope=3, out_slope=3)],
         [-1.5, -0.7, -0.05, 0.0, 0.33, 0.9, 1.25], pre="Linear", post="Linear")
    case("Decimal times and values", [Key(0.1, 0.3), Key(0.2, 0.7, "Constant"), Key(0.35, 0.15), Key(0.7, 0.9)],
         [0.15, 0.2, 0.3, 0.5, 1.9, -3.3], pre="PingPong", post="Loop")
    case("Mixed interpolations with set slopes",
         [Key(0, 0, out_slope=-2), Key(1, 3, "Linear", in_slope=4), Key(2, -1, "Constant"), Key(3, 2, out_slope=1),
          Key(5, 2.5, in_slope=-1)],
         [0.1, 0.9, 1.5, 2.0, 2.99, 3.0, 3.5, 4.6], pre="PingPong", post="PingPong")
    return cases


def inline(value):
    """A JSON value on one line, with spaces inside its braces."""
    return json.dumps(value, ensure_ascii=False, separators=(", ", ": ")).replace("{", "{ ").replace("}", " }")


def write_document(cases):
    """The file's text: tabs, one key or sample per line."""
    lines = ["{", "\t\"About\": " + inline(ABOUT) + ",", "\t\"Tolerance\": " + json.dumps(TOLERANCE) + ",", "\t\"Cases\": ["]
    for index, item in enumerate(cases):
        curve = item["Curve"]
        lines.append("\t\t{")
        lines.append("\t\t\t\"Name\": " + inline(item["Name"]) + ",")
        lines.append("\t\t\t\"Curve\": {")
        lines.append("\t\t\t\t\"PreExtrapolation\": " + inline(curve["PreExtrapolation"]) + ",")
        lines.append("\t\t\t\t\"PostExtrapolation\": " + inline(curve["PostExtrapolation"]) + ",")
        if curve["Keys"]:
            lines.append("\t\t\t\t\"Keys\": [")
            lines.append(",\n".join("\t\t\t\t\t" + inline(key) for key in curve["Keys"]))
            lines.append("\t\t\t\t]")
        else:
            lines.append("\t\t\t\t\"Keys\": []")
        lines.append("\t\t\t},")
        lines.append("\t\t\t\"Samples\": [")
        lines.append(",\n".join("\t\t\t\t" + inline(sample) for sample in item["Samples"]))
        lines.append("\t\t\t]")
        lines.append("\t\t}" + ("," if index + 1 < len(cases) else ""))
    lines += ["\t]", "}"]
    return "\n".join(lines) + "\n"


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else OUTPUT
    cases = make_cases()
    text = write_document(cases)
    document = {"About": ABOUT, "Tolerance": TOLERANCE, "Cases": cases}
    if json.loads(text) != json.loads(json.dumps(document)):
        sys.exit("the written text does not read back as the document")
    with open(output, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)
    print(f"{len(cases)} cases, {sum(len(item['Samples']) for item in cases)} samples")


if __name__ == "__main__":
    main()
