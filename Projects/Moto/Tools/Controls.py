"""Moto's controls: the project's input actions (Input/Moto.sinput, which Moto.sproj names), written by Author.py's input
part. Each action is bound on the keyboard and on any gamepad. Graphs read them by name (Event.ActionPressed, Get Action)
and never a key, so a player's bindings can change without a change to a graph. The actions come in groups, one for each
graph that reads them, each registered by one line of GROUPS; no control is bound to two actions.

- Riding (the bike's riding graph, Vehicle.py):
  - Throttle (an axis, 0 to 1): W, the right trigger.
  - FrontBrake and RearBrake (axes, 0 to 1): S and the left trigger for the front brake, Space and the gamepad's X for the
    rear one, each wheel braked by its own (a bike's two brakes).
  - Lean (an axis, -1 left to 1 right): A and D, the left stick's X.
  - ShiftUp and ShiftDown (buttons): E and Q, the right and the left bumper.
- Pause (a button): Escape, the gamepad's Start. The audio director blends the mix to the Paused snapshot and back.
- NextCamera and ResetCamera (buttons): C and R, the gamepad's Y and Back. The camera director cuts to the next trackside
  camera, and back to the tour.
- Confirm (a button): Enter, the gamepad's A. The race director starts the race from its title, and goes back to the title
  from the results.

A player's profile written before the two brakes (it names Brake, which the actions no longer have) loses that entry,
with one line in the log, when the game loads its actions; the rest of the profile holds.
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


RIDING = (
    action("Throttle", "Axis", binding("Key/W"), binding("Gamepad/RightTrigger", TRIGGER_DEAD_ZONE)),
    action("FrontBrake", "Axis", binding("Key/S"), binding("Gamepad/LeftTrigger", TRIGGER_DEAD_ZONE)),
    action("RearBrake", "Axis", binding("Key/Space"), binding("Gamepad/X")),
    action("Lean", "Axis", binding("Key/A Key/D"), binding("Gamepad/LeftX", STICK_DEAD_ZONE)),
    action("ShiftUp", "Button", binding("Key/E"), binding("Gamepad/RightBumper")),
    action("ShiftDown", "Button", binding("Key/Q"), binding("Gamepad/LeftBumper")),
)
PAUSE = (
    action("Pause", "Button", binding("Key/Escape"), binding("Gamepad/Start")),
)
CAMERAS = (
    action("NextCamera", "Button", binding("Key/C"), binding("Gamepad/Y")),
    action("ResetCamera", "Button", binding("Key/R"), binding("Gamepad/Back")),
)
RACE = (
    action("Confirm", "Button", binding("Key/Enter"), binding("Gamepad/A")),
)

# The groups of actions, one line each: (the graph that reads them, its actions), in the order the asset lists them.
GROUPS = (
    ("the bike's riding graph", RIDING),
    ("the audio director", PAUSE),
    ("the camera director", CAMERAS),
    ("the race director", RACE),
)
ACTIONS = tuple(action for _, actions in GROUPS for action in actions)


def check():
    """No two actions read one control: every key and gamepad control is bound once. Returns a summary."""
    seen = {}
    for entry in ACTIONS:
        for bound in entry["Bindings"]:
            for control in bound["Controls"].split():
                assert control not in seen, f"{control} is bound to both {seen[control]} and {entry['Name']}"
                seen[control] = entry["Name"]
    return f"{len(ACTIONS)} actions in {len(GROUPS)} groups, {len(seen)} controls, each bound once"


def input_actions():
    """The asset's fields, all of them, so that writing them again over the asset gives the same file."""
    return {"Actions": list(ACTIONS)}
