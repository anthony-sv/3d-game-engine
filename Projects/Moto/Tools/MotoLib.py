"""Moto's graph library (Scripts/MotoLib.sgraph): the pure functions the ride's graphs share, each the formula of a figure
Vehicle.py or Ride.py states, so that a graph and the tool that checks it compute it alike.

- AssistSteer(Request, Speed) -> Steer: the rider assist's mapping from a lean request (-1 to 1, a share of the lean
  limit) to the steering input that holds that lean at the speed (Vehicle.assist_steer).
- CurvatureLean(Curvature, Speed) -> Request: the lean request whose lean turns the bike on a curvature (1/m, positive to
  the right) at the speed (taken at the assist's slowest at least, so that the assist's steering for it turns on the
  curvature at a crawl too): atan(v^2 kappa / g) over the lean limit, within -1 and 1.
- BrakeEase(Lean) -> Share: the linked brakes' share of each lever at a lean (degrees; Vehicle.brake_ease).
- LineDistance(Position, From, To, FromDistance, ToDistance) -> Distance: the distance along the lap of a point's foot on
  the stretch of the line between two of its points, from their distances (the stretch's ends, the point projected on
  the ground and kept within the stretch).
- Kmh(Speed) -> Kmh: meters a second in kilometers an hour.

A library has no variables: the line's points live in the graph that rides it (Ride.sgraph), which calls these.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Graphs  # noqa: E402  (writing graphs)
import Vehicle  # noqa: E402  (the assist's figures)
from Graphs import RULES  # noqa: E402

LIB = Graphs.script("MotoLib")
VECTOR = "Strada.Vector3"
FLOAT = "float"


def declare(author):
    author.declare(LIB, "Moto's library: the rider assist's mapping, the lean a curvature takes, the linked brakes' easing "
                   "by the lean, a point's distance along the lap and units. Tools/MotoLib.py writes it.", functions=(
                       ("AssistSteer", True, (("Request", FLOAT), ("Speed", FLOAT)), (("Steer", FLOAT),), ()),
                       ("CurvatureLean", True, (("Curvature", FLOAT), ("Speed", FLOAT)), (("Request", FLOAT),), ()),
                       ("BrakeEase", True, (("Lean", FLOAT),), (("Share", FLOAT),), ()),
                       ("LineDistance", True, (("Position", VECTOR), ("From", VECTOR), ("To", VECTOR), ("FromDistance", FLOAT),
                                               ("ToDistance", FLOAT)), (("Distance", FLOAT),), ()),
                       ("Kmh", True, (("Speed", FLOAT),), (("Kmh", FLOAT),), ()),
                   ))


def write(author):
    k = round(Vehicle.ASSIST_WHEELBASE * Vehicle.GRAVITY / Vehicle.CASTER_COS, 6)
    with author.region(LIB, "Assist steer: the steering that holds a lean request at the speed", RULES, "AssistSteer") as b:
        b.link(b.expr(f"clamp(degrees(asin(clamp({k} * tan(radians(request * {Vehicle.MAX_LEAN})) / pow(max(speed, "
                      f"{Vehicle.ASSIST_MIN_SPEED}), 2), -1, 1))) / {Vehicle.STEER_LOCK}, -1, 1)",
                      request=b.entry["Request"], speed=b.entry["Speed"]), b.ret["Steer"])
    with author.region(LIB, "Curvature lean: the lean request that turns on a curvature", RULES, "CurvatureLean") as b:
        # At the assist's slowest speed at least, so that the assist's steering for the request turns on the curvature at a
        # crawl too.
        b.link(b.expr(f"clamp(degrees(atan(pow(max(speed, {Vehicle.ASSIST_MIN_SPEED}), 2) * curvature / {Vehicle.GRAVITY})) / "
                      f"{Vehicle.MAX_LEAN}, -1, 1)", speed=b.entry["Speed"], curvature=b.entry["Curvature"]), b.ret["Request"])
    with author.region(LIB, "Brake ease: the linked brakes' share of a lever at a lean", RULES, "BrakeEase") as b:
        b.link(b.expr("pow(1 - abs(sin(radians(lean))), 2)", lean=b.entry["Lean"]), b.ret["Share"])
    with author.region(LIB, "Line distance: a point's foot on a stretch of the line, along the lap", RULES, "LineDistance") as b:
        b.link(b.expr("start + (end - start) * clamp(dot(vec2(p.X - a.X, p.Z - a.Z), vec2(c.X - a.X, c.Z - a.Z)) / "
                      "max(dot(vec2(c.X - a.X, c.Z - a.Z), vec2(c.X - a.X, c.Z - a.Z)), 0.000001), 0, 1)",
                      start=b.entry["FromDistance"], end=b.entry["ToDistance"], p=b.entry["Position"], a=b.entry["From"],
                      c=b.entry["To"]), b.ret["Distance"])
    with author.region(LIB, "Kmh: meters a second in kilometers an hour", RULES, "Kmh") as b:
        b.link(b.expr("speed * 3.6", speed=b.entry["Speed"]), b.ret["Kmh"])
