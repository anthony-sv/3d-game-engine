"""Moto's controls: the project's input actions (Input/Moto.sinput, which Moto.sproj names), written by Author.py's input
part. Each action is bound on the keyboard and on any gamepad. Graphs read them by name (Event.ActionPressed, Get Action)
and never a key, so a player's bindings can change without a change to a graph.

- Throttle and Brake (axes, 0 to 1): W and S, the right and the left trigger.
- Lean (an axis, -1 left to 1 right): A and D, the left stick's X.
- ShiftUp and ShiftDown (buttons): E and Q, the right and the left bumper.
- Pause (a button): Escape, the gamepad's Start. The audio director blends the mix to the Paused snapshot and back.
- NextCamera and ResetCamera (buttons): C and R, the gamepad's Y and Back. The camera director cuts to the next trackside
  camera, and back to the tour.

The riding actions wait for the ridden bike (slice 4); the last three are used now.
"""

INPUT_ACTIONS = "Input/Moto.sinput"
# A trigger's or a stick's rest: what it reads below this is taken as nothing.
STICK_DEAD_ZONE = 0.15
TRIGGER_DEAD_ZONE = 0.05


def binding(controls, dead_zone=0.0):
    return {"Controls": controls, "Gamepad": -1, "DeadZone": dead_zone, "ResponseCurve": "0", "Invert": False,
            "InvertY": False, "Scale": 1.0}


def action(name, kind, *bindings):
    return {"Name": name, "Type": kind, "PressThreshold": 0.5, "ReleaseThreshold": 0.4, "Bindings": list(bindings)}


ACTIONS = (
    action("Throttle", "Axis", binding("Key/W"), binding("Gamepad/RightTrigger", TRIGGER_DEAD_ZONE)),
    action("Brake", "Axis", binding("Key/S"), binding("Gamepad/LeftTrigger", TRIGGER_DEAD_ZONE)),
    action("Lean", "Axis", binding("Key/A Key/D"), binding("Gamepad/LeftX", STICK_DEAD_ZONE)),
    action("ShiftUp", "Button", binding("Key/E"), binding("Gamepad/RightBumper")),
    action("ShiftDown", "Button", binding("Key/Q"), binding("Gamepad/LeftBumper")),
    action("Pause", "Button", binding("Key/Escape"), binding("Gamepad/Start")),
    action("NextCamera", "Button", binding("Key/C"), binding("Gamepad/Y")),
    action("ResetCamera", "Button", binding("Key/R"), binding("Gamepad/Back")),
)


def input_actions():
    """The asset's fields, all of them, so that writing them again over the asset gives the same file."""
    return {"Actions": list(ACTIONS)}
