"""Moto's menus (Docs/Architecture.md section 15, "The race"), written by Author.py's hud part with the HUD (Hud.py): the
Menus entity under the Screen canvas, after the HUD's groups, which runs Scripts/Menu.sgraph, and its four screens, each
saved hidden and shown by the graph as the race director's state and pause ask:

- the title (the director's Title, over the attract mode): the game's name, the laps, the best lap, and RACE (the
  director is sent StartRace), SETTINGS and QUIT;
- the results (Results): each lap's time, the total, the best lap (NEW BEST when the race made one), and CONTINUE (back to
  the title: AbandonRace) and RACE AGAIN (StartRace);
- the pause screen (while the director's Paused reads true, in any state): RESUME (SetPaused off), SETTINGS and QUIT TO
  TITLE (AbandonRace, and the game goes on);
- the settings screen, from the title or the pause screen: rows of stepped buttons. Each setting has a "<" and a ">" that
  step it down or up and the value it has between them, read back from Settings (the quality, the window's mode and size,
  the master volume, the volumes of the Engine, Tyres and Ambience buses as shares of the mix the game makes, and the
  linked brakes with their share, the game's own named values Settings keeps); each riding action has a button for its
  keyboard binding and one for its gamepad binding, which listen for the next control (Input.StartRebind; the lean's keys
  one direction after the other), refuse a pad's control for the keyboard and a key for the pad, and name the actions the
  new control also presses; then RESET CONTROLS and BACK, which saves the settings (Settings.Save: the bindings are kept
  by the input system's profile as each rebind ends).

Every screen is a list of buttons (UIButton): the mouse clicks them, and the pad moves a selection the graph keeps through
them with the menu actions (Controls.py: MenuUp and MenuDown move it, MenuLeft and MenuRight step the selected setting or
choose a control row's keyboard or pad button, Confirm presses the selected button by UIButton.Click(), Back leaves the
screen); the selected buttons show in SELECTED. While a binding listens the menus take no action and no click: the answer
is the player's. Each screen draws in two batches when shown: its rectangles and buttons, then its texts.

The assists apply to the riding graph of the menu's Rider (the bike): its LinkedBrakes and LinkedBrakeShare, set when the
scene starts and after each step from the values the player set (one never set leaves the rider's own), through
ASSIST_GRAPH, which declares those two names: on this tree it is
Scripts/AssistsStandIn.sgraph (the ride test scene's stand-in rider runs it), since the bike's riding graph gets the
rider assists from the ride's track; ShowMenus off shows no screen (the captures of the circuit).

    python Projects/Moto/Tools/Menu.py      (prints the figures check() asserts)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Graphs  # noqa: E402  (writing graphs)
import Hud  # noqa: E402  (the screen's layout helpers and its Clock)
import Race  # noqa: E402  (the race director)
from Graphs import EFFECTS, ENTITY, EVENTS, RULES, private, public  # noqa: E402

ROOT = "Menus"
MENU = Graphs.script("Menu")
ASSISTS_STAND_IN = Graphs.script("AssistsStandIn")
# The graph whose public LinkedBrakes and LinkedBrakeShare the assists' settings write: the bike's riding graph takes them
# when the ride's rider assists meet the race (Vehicle.BIKE).
ASSIST_GRAPH = ASSISTS_STAND_IN
LINKED = "LinkedBrakes"
SHARE = "LinkedBrakeShare"
# The linked brakes' defaults, the riding graph's own: on, the rear given 0.3 of the front.
LINKED_ON = True
LINKED_SHARE = 0.3
NAME = "STRADA MOTO"

# --- The screens and their buttons --------------------------------------------------------------------------------------

TITLE, RESULTS, PAUSE, SETTINGS = "Title", "Results", "Pause", "Settings"
SCREENS = (TITLE, RESULTS, PAUSE, SETTINGS)
# What a button does (its Kinds entry); Rows and Arguments say for which row and how.
KINDS = ("Race", "Settings", "Quit", "Title", "Resume", "Step", "Bind", "Reset", "Back")
KIND = {name: index for index, name in enumerate(KINDS)}
# The settings screen's rows: steppers, then a row for each riding action, then the two buttons.
STEPPER, CONTROL, ACTION = 0, 1, 2
STEPPERS = ("QUALITY", "WINDOW", "SIZE", "MASTER", "ENGINE", "TYRES", "AMBIENCE", "LINKED BRAKES", "REAR SHARE")
QUALITY_ROW, WINDOW_ROW, SIZE_ROW, MASTER_ROW = 0, 1, 2, 3
BUS_ROWS = (4, 5, 6)
BUSES = ("Engine", "Tyres", "Ambience")
LINKED_ROW, SHARE_ROW = 7, 8
CONTROLS = (("THROTTLE", "Throttle"), ("FRONT BRAKE", "FrontBrake"), ("REAR BRAKE", "RearBrake"), ("LEAN", "Lean"),
            ("SHIFT UP", "ShiftUp"), ("SHIFT DOWN", "ShiftDown"))
FIRST_CONTROL = len(STEPPERS)
# The lean's keyboard binding is two keys (left, right): it listens for one, then the other.
COMPOSITE = "Lean"
RESET_ROW = FIRST_CONTROL + len(CONTROLS)
BACK_ROW = RESET_ROW + 1
ROWS = BACK_ROW + 1
KEYBOARD, GAMEPAD = 0, 1
# The window sizes SIZE steps through.
SIZES = ((1280, 720), (1600, 900), (1920, 1080), (2560, 1440))
QUALITIES = ("LOW", "MEDIUM", "HIGH")
MODES = ("WINDOWED", "FULLSCREEN")
# A volume steps by a tenth of the mix the game makes; silence is the lowest a bus takes (dB).
SILENCE = -80.0

NORMAL = [0.022, 0.1473, 0.7106, 1.0]
SELECTED = [0.95, 0.5, 0.08, 1.0]
DIM = [0.0, 0.0, 0.02, 0.62]
SHADE = [0.0, 0.0, 0.02, 0.45]
PLATE = Hud.PLATE
VALUE = Hud.VALUE
CAPTION = Hud.CAPTION
CENTER = Hud.CENTER


def rect(x, y, width, height):
    return Hud.anchored(CENTER, x, y, width, height)


def plate(x, y, width, height, color=PLATE, radius=18.0):
    return {"UIRect": rect(x, y, width, height), "UIImage": Hud.image(color, radius)}


def button(x, y, width, height):
    return {"UIRect": rect(x, y, width, height), "UIImage": Hud.image(NORMAL, 10.0),
            "UIButton": {"Interactable": True, "NormalColor": list(NORMAL), "HoveredColor": [0.055, 0.2429, 0.9331, 1.0],
                         "PressedColor": [0.01, 0.0732, 0.3931, 1.0], "DisabledColor": [0.1065, 0.1193, 0.1473, 0.6]}}


def words(x, y, width, height, value, size, color=VALUE, alignment="Center"):
    return {"UIRect": rect(x, y, width, height), "UIText": Hud.label(value, size, color, alignment)}


def screen(color=None):
    components = {"UIRect": Hud.fill(visible=False)}
    if color is not None:
        components["UIImage"] = Hud.image(color)
    return components


def title_case(text):
    return " ".join(word.capitalize() for word in text.split())


# The settings rows' places (canvas units): each row ROW_STEP below the one before, its label on the left, then its
# buttons: a stepper's "<" and ">" with its value between, a control row's keyboard and gamepad buttons, or one button.
ROW_TOP = 170.0
ROW_STEP = 44.0
ROW_HEIGHT = 38.0
LABEL_X, LABEL_WIDTH = 380.0, 420.0
BUTTONS_X, BUTTONS_WIDTH = 820.0, 680.0
STEP_WIDTH = 60.0
COLUMN_WIDTH = 330.0
COLUMN_GAP = BUTTONS_WIDTH - 2.0 * COLUMN_WIDTH


def row_y(row):
    return ROW_TOP + ROW_STEP * row


def settings_rows():
    """The settings screen's rows: (label, kind, the lower or only button's name, the upper button's name, the value text's
    name or None, the keyboard and pad texts' names or None)."""
    rows = []
    for label in STEPPERS:
        name = title_case(label)
        rows.append((label, STEPPER, f"{name} Lower", f"{name} Upper", f"{name} Value", None))
    for label, _ in CONTROLS:
        name = title_case(label)
        rows.append((label, CONTROL, f"{name} Key", f"{name} Pad", None, (f"{name} Key Text", f"{name} Pad Text")))
    for label in ("RESET CONTROLS", "BACK"):
        name = f"{title_case(label)} Button"
        rows.append((label, ACTION, name, name, None, None))
    return rows


def elements():
    """The menus' elements in the order they draw: (name, parent (None for the Menus entity), components, the graph's
    variable that names it or None). Each screen holds its rectangles and buttons first, then its texts."""
    rows = []
    menu_button_width, menu_button_height = 400.0, 56.0

    def menu_buttons(screen_name, x, top, labels):
        made, texts = [], []
        for index, (name, label) in enumerate(labels):
            y = top + index * 70.0
            made.append((name, screen_name, button(x, y, menu_button_width, menu_button_height), None))
            texts.append((f"{name} Text", screen_name, words(x, y, menu_button_width, menu_button_height, label, 32.0), None))
        return made, texts

    # The title, over the attract mode.
    rows.append(("Title Screen", None, screen(), "TitleScreen"))
    rows.append(("Title Plate", "Title Screen", plate(560.0, 250.0, 800.0, 600.0), None))
    title_buttons, title_texts = menu_buttons("Title Screen", 760.0, 560.0,
                                              (("Race Button", "RACE"), ("Settings Button", "SETTINGS"), ("Quit Button", "QUIT")))
    rows += title_buttons
    rows += [("Title Name", "Title Screen", words(560.0, 290.0, 800.0, 110.0, NAME, 96.0), None),
             ("Title Laps", "Title Screen", words(560.0, 410.0, 800.0, 44.0, "TIME TRIAL", 36.0, CAPTION), "TitleLaps"),
             ("Title Best", "Title Screen", words(560.0, 470.0, 800.0, 44.0, "", 34.0), "TitleBest")]
    rows += title_texts
    rows.append(("Title Hint", "Title Screen", words(560.0, 790.0, 800.0, 36.0, "ARROWS OR D-PAD TO CHOOSE   ENTER OR A TO PRESS",
                                                     22.0, CAPTION), None))
    # The results.
    rows.append(("Results Screen", None, screen(SHADE), "ResultsScreen"))
    rows.append(("Results Plate", "Results Screen", plate(560.0, 230.0, 800.0, 620.0), None))
    result_buttons, result_texts = menu_buttons("Results Screen", 760.0, 690.0,
                                                (("Continue Button", "CONTINUE"), ("Again Button", "RACE AGAIN")))
    rows += result_buttons
    rows += [("Results Title", "Results Screen", words(560.0, 260.0, 800.0, 80.0, "RESULTS", 72.0), None),
             ("Results Laps", "Results Screen", words(560.0, 350.0, 800.0, 150.0, "", 40.0), "ResultsLaps"),
             ("Results Total", "Results Screen", words(560.0, 520.0, 800.0, 50.0, "", 44.0), "ResultsTotal"),
             ("Results Best", "Results Screen", words(560.0, 590.0, 800.0, 44.0, "", 34.0, CAPTION), "ResultsBest")]
    rows += result_texts
    # The pause screen, over the race.
    rows.append(("Pause Screen", None, screen(DIM), "PauseScreen"))
    pause_buttons, pause_texts = menu_buttons("Pause Screen", 760.0, 620.0,
                                              (("Resume Button", "RESUME"), ("Pause Settings Button", "SETTINGS"),
                                               ("Pause Title Button", "QUIT TO TITLE")))
    rows += pause_buttons
    rows += [("Pause Title", "Pause Screen", words(460.0, 380.0, 1000.0, 150.0, "PAUSED", 132.0), None),
             ("Pause Hint", "Pause Screen", words(460.0, 540.0, 1000.0, 50.0, "ESC OR START TO RESUME", 34.0, CAPTION), None)]
    rows += pause_texts
    # The settings: every rectangle and button, then every text.
    rows.append(("Settings Screen", None, screen(DIM), "SettingsScreen"))
    rows.append(("Settings Plate", "Settings Screen", plate(330.0, 60.0, 1260.0, 960.0), None))
    texts = [("Settings Title", "Settings Screen", words(330.0, 80.0, 1260.0, 70.0, "SETTINGS", 56.0), None)]
    for index, (label, kind, lower, upper, value, controls) in enumerate(settings_rows()):
        y = row_y(index)
        if kind != ACTION:
            # A setting's or a riding action's name; a row of one button is named by its button.
            texts.append((f"{title_case(label)} Row", "Settings Screen",
                          words(LABEL_X, y, LABEL_WIDTH, ROW_HEIGHT, label, 28.0, CAPTION, "Left"), None))
        if kind == STEPPER:
            rows.append((lower, "Settings Screen", button(BUTTONS_X, y, STEP_WIDTH, ROW_HEIGHT), None))
            rows.append((upper, "Settings Screen", button(BUTTONS_X + BUTTONS_WIDTH - STEP_WIDTH, y, STEP_WIDTH, ROW_HEIGHT), None))
            texts.append((f"{lower} Text", "Settings Screen", words(BUTTONS_X, y, STEP_WIDTH, ROW_HEIGHT, "<", 30.0), None))
            texts.append((value, "Settings Screen", words(BUTTONS_X + STEP_WIDTH, y, BUTTONS_WIDTH - 2.0 * STEP_WIDTH, ROW_HEIGHT, "", 30.0),
                          None))
            texts.append((f"{upper} Text", "Settings Screen",
                          words(BUTTONS_X + BUTTONS_WIDTH - STEP_WIDTH, y, STEP_WIDTH, ROW_HEIGHT, ">", 30.0), None))
        elif kind == CONTROL:
            rows.append((lower, "Settings Screen", button(BUTTONS_X, y, COLUMN_WIDTH, ROW_HEIGHT), None))
            rows.append((upper, "Settings Screen", button(BUTTONS_X + COLUMN_WIDTH + COLUMN_GAP, y, COLUMN_WIDTH, ROW_HEIGHT), None))
            texts.append((controls[0], "Settings Screen", words(BUTTONS_X, y, COLUMN_WIDTH, ROW_HEIGHT, "", 26.0), None))
            texts.append((controls[1], "Settings Screen",
                          words(BUTTONS_X + COLUMN_WIDTH + COLUMN_GAP, y, COLUMN_WIDTH, ROW_HEIGHT, "", 26.0), None))
        else:
            rows.append((lower, "Settings Screen", button(BUTTONS_X, y, BUTTONS_WIDTH, ROW_HEIGHT), None))
            texts.append((f"{lower} Text", "Settings Screen", words(BUTTONS_X, y, BUTTONS_WIDTH, ROW_HEIGHT, label, 28.0), None))
    status_y = row_y(ROWS) + 6.0
    texts.append(("Settings Status", "Settings Screen", words(380.0, status_y, 1160.0, 36.0, "", 26.0), "SettingsStatus"))
    texts.append(("Settings Hint", "Settings Screen",
                  words(380.0, status_y + 42.0, 1160.0, 30.0, "ARROWS OR D-PAD TO CHOOSE AND STEP   ENTER OR A TO PRESS   BACKSPACE OR B "
                        "TO SAVE AND LEAVE", 20.0, CAPTION), None))
    rows += texts
    return rows


def tables():
    """The graph's lists, by its variable: (its type, the names of the elements, or the values, in order)."""
    rows = settings_rows()
    buttons, kinds, row_numbers, arguments = [], [], [], []

    def add(name, kind, row=0, argument=0):
        buttons.append(name)
        kinds.append(KIND[kind])
        row_numbers.append(row)
        arguments.append(argument)

    for name, kind in (("Race Button", "Race"), ("Settings Button", "Settings"), ("Quit Button", "Quit"),
                       ("Continue Button", "Title"), ("Again Button", "Race"),
                       ("Resume Button", "Resume"), ("Pause Settings Button", "Settings"), ("Pause Title Button", "Title")):
        add(name, kind)
    for index, (label, kind, lower, upper, _, _) in enumerate(rows):
        if kind == STEPPER:
            add(lower, "Step", index, -1)
            add(upper, "Step", index, 1)
        elif kind == CONTROL:
            add(lower, "Bind", index, KEYBOARD)
            add(upper, "Bind", index, GAMEPAD)
        else:
            add(lower, "Reset" if index == RESET_ROW else "Back", index)
    return {
        "Buttons": ("Entity[]", buttons), "Kinds": ("Int32[]", kinds), "Rows": ("Int32[]", row_numbers),
        "Arguments": ("Int32[]", arguments),
        "TitleItems": ("Entity[]", ["Race Button", "Settings Button", "Quit Button"]),
        "ResultsItems": ("Entity[]", ["Continue Button", "Again Button"]),
        "PauseItems": ("Entity[]", ["Resume Button", "Pause Settings Button", "Pause Title Button"]),
        "Lower": ("Entity[]", [lower for _, _, lower, _, _, _ in rows]),
        "Upper": ("Entity[]", [upper for _, _, _, upper, _, _ in rows]),
        "RowKinds": ("Int32[]", [kind for _, kind, _, _, _, _ in rows]),
        "Values": ("Entity[]", [value for _, kind, _, _, value, _ in rows if kind == STEPPER]),
        "KeyTexts": ("Entity[]", [controls[0] for _, kind, _, _, _, controls in rows if kind == CONTROL]),
        "PadTexts": ("Entity[]", [controls[1] for _, kind, _, _, _, controls in rows if kind == CONTROL]),
    }


TOOLTIPS = {
    "TitleScreen": "The title's screen",
    "TitleLaps": "The title's line that says how many laps the time trial has",
    "TitleBest": "The title's line that shows the best lap",
    "ResultsScreen": "The results' screen",
    "ResultsLaps": "The results' lap times, a line each",
    "ResultsTotal": "The results' total",
    "ResultsBest": "The results' best lap",
    "PauseScreen": "The pause screen",
    "SettingsScreen": "The settings screen",
    "SettingsStatus": "The settings screen's line for a binding that listens, its answer and its conflicts",
}
TABLE_TOOLTIPS = {
    "Buttons": "Every button of the menus, in the order Kinds, Rows and Arguments give what each does",
    "Kinds": "What each button does: 0 race, 1 settings, 2 quit, 3 title, 4 resume, 5 step, 6 bind, 7 reset, 8 back",
    "Rows": "The settings row each button steps or binds",
    "Arguments": "A step's direction (-1 or 1), or a binding's index (0 the keyboard's, 1 the gamepad's)",
    "TitleItems": "The title's buttons, in the order the pad moves through them",
    "ResultsItems": "The results' buttons, in the order the pad moves through them",
    "PauseItems": "The pause screen's buttons, in the order the pad moves through them",
    "Lower": "Each settings row's lower button: a setting's <, a riding action's keyboard binding, or the row's button",
    "Upper": "Each settings row's upper button: a setting's >, a riding action's gamepad binding, or the row's button",
    "RowKinds": "Each settings row's kind: 0 a setting, 1 a riding action's bindings, 2 a button",
    "Values": "Each setting's value text",
    "KeyTexts": "Each riding action's keyboard binding text",
    "PadTexts": "Each riding action's gamepad binding text",
}
TABLE_TYPES = {"Entity[]": "Strada.Entity[]", "Int32[]": "int[]"}


def check():
    """Asserts what the menus promise; returns a summary."""
    rows = elements()
    names = [name for name, *_ in rows]
    assert len(set(names)) == len(names), "the menus' elements share names"
    hud_names = {name for name, *_ in Hud.elements()}
    assert not hud_names & set(names), "the menus and the HUD share names"
    variables = [variable for *_, variable in rows if variable is not None]
    assert sorted(variables) == sorted(TOOLTIPS), "every element the graph changes has a variable with a tooltip"
    lists = tables()
    assert sorted(lists) == sorted(TABLE_TOOLTIPS), "every list has a tooltip"
    for variable, (_, values) in lists.items():
        if lists[variable][0] == "Entity[]":
            missing = [value for value in values if value not in names]
            assert not missing, f"{variable} names no element {missing}"
    assert len(lists["Buttons"][1]) == sum(1 for _, _, components, _ in rows if "UIButton" in components), "every button is listed"
    assert len(lists["Lower"][1]) == ROWS and len(lists["Values"][1]) == len(STEPPERS)
    # Each screen: its rectangles and buttons, then its texts.
    batches = {}
    for name in ("Title Screen", "Results Screen", "Pause Screen", "Settings Screen"):
        own = [row for row in rows if row[0] == name or row[1] == name]
        batches[name] = Hud.batches_of(own)
        assert batches[name] == 2, f"the {name} draws in {batches[name]} batches"
    bottom = row_y(ROWS) + 6.0 + 42.0 + 30.0
    assert bottom <= 60.0 + 960.0, f"the settings rows reach {bottom:.0f}, past their plate"
    buttons = len(lists["Buttons"][1])
    return f"{len(rows)} elements, {buttons} buttons, {ROWS} settings rows; each screen in 2 batches"


# --- The graphs ---------------------------------------------------------------------------------------------------------------

def declare(author):
    author.declare(ASSISTS_STAND_IN, "What the settings screen's assists write of a riding graph: the linked brakes and their "
                   "share. The ride test scene's stand-in rider runs it.", variables=(
        public(LINKED, "bool", LINKED_ON, "The linked brakes: the front brake also applies the rear by LinkedBrakeShare"),
        public(SHARE, "float", LINKED_SHARE, "How much of the front brake the linked brakes give the rear, 0 to 1", (0.0, 1.0)),
    ))
    entity_variables = tuple(public(variable, ENTITY, None, TOOLTIPS[variable]) for *_, variable in elements() if variable is not None)
    list_variables = tuple(public(variable, TABLE_TYPES[kind], [], TABLE_TOOLTIPS[variable]) for variable, (kind, _) in tables().items())
    author.declare(MENU, "The menus: the title, the results, the pause and the settings screens, shown as the race director's "
                   "state and pause ask; the pad moves a selection through their buttons and presses it, the mouse clicks "
                   "them, and the settings are kept through Settings and the bindings' profile.", variables=(
        public("Race", ENTITY, None, "The race director, whose state and pause choose the screen and whose events the buttons send"),
        public("Rider", ENTITY, None, "The bike whose riding graph takes the linked brakes' settings"),
        public("ShowMenus", "bool", True, "On: the screens show as the race asks. Off: none shows (pictures of the circuit)"),
    ) + entity_variables + list_variables + (
        public("Screen", "string", "", "The screen shown: Title, Results, Pause, Settings, or empty for none (a read-out)"),
        public("Selected", "int", 0, "The selected button of the screen, or row of the settings (a read-out)"),
        public("Column", "int", 0, "A riding action's row: 0 its keyboard button selected, 1 its gamepad's (a read-out)"),
        public("Rebinding", "bool", False, "Whether a binding listens for the player's control (a read-out)"),
        public("Saves", "int", 0, "How many times leaving the settings saved them (a read-out)"),
        public("Rebinds", "int", 0, "How many rebinds through the screen ended with a new control (a read-out)"),
        public("Conflicts", "string", "", "The actions the last rebind's control also presses (a read-out)"),
        public("Applied", "int", 0, "How many times the linked brakes' settings were given to the rider (a read-out)"),
        private("State", "string", ""),
        private("Paused", "bool", False),
        private("SettingsOpen", "bool", False),
        private("SettingsFrom", "string", ""),
        private("LapLines", "string[]", []),
        private("NewBest", "bool", False),
        private("BindAction", "string", ""),
        private("BindName", "string", ""),
        private("BindBinding", "int", 0),
        private("BindPart", "int", -1),
        private("Before", "string", ""),
        private("Refused", "string", ""),
        private("Widths", "int[]", [width for width, _ in SIZES]),
        private("Heights", "int[]", [height for _, height in SIZES]),
        private("BusNames", "string[]", list(BUSES)),
        private("RowActions", "string[]", [action for _, action in CONTROLS]),
        private("RowNames", "string[]", [label for label, _ in CONTROLS]),
    ), events=tuple((name, Race.BROADCASTS[name]) for name in ("RaceStarted", "LapFinished", "RaceFinished")), functions=(
        ("Clock", True, (("Seconds", "float"),), (("Text", "string"),), ()),
        ("ControlName", True, (("Controls", "string"),), (("Name", "string"),), ()),
        ("Desired", True, (), (("Name", "string"),), ()),
        ("Item", False, (("Index", "int"), ("Upper", "bool")), (("Button", ENTITY),), (("Found", ENTITY, None),)),
        ("Count", False, (), (("Count", "int"),), (("Found", "int", 0),)),
        ("ShowTitle", False, (), (), ()),
        ("ShowValues", False, (), (), ()),
        ("ShowControls", False, (), (), ()),
        ("Tint", False, (), (), ()),
        ("ShowScreen", False, (("Name", "string"),), (), ()),
        ("ApplyAssists", False, (), (), ()),
        ("CloseSettings", False, (), (), ()),
        ("Step", False, (("Row", "int"), ("Direction", "int")), (), (("Index", "int", 0), ("Share", "int", 0))),
        ("ShowPrompt", False, (), (), ()),
        ("StartBind", False, (("Row", "int"), ("Binding", "int")), (), ()),
        ("PollRebind", False, (), (), (("Controls", "string", ""), ("Unfit", "bool", False))),
        ("ShowResults", False, (("Total", "float"),), (), ()),
        ("Navigate", False, (("Delta", "int"),), (), (("Items", "int", 1),)),
        ("Side", False, (("Direction", "int"),), (), ()),
        ("Press", False, (), (), ()),
        ("GoBack", False, (), (), ()),
        ("Command", False, (("Button", ENTITY),), (), (("Index", "int", 0),)),
    ))


def director(b, name):
    return b.once(("director", name), lambda: b.get(name, Race.DIRECTOR, b.var("Race")))


def element(b, variable, index):
    return b.add("Array.Get", None, "Element", Array=b.var(variable), Index=index).out


def show(b, target, visible):
    return b.call("Set:Strada.UIRectComponent.Visible", Target=target, Value=visible)


def text(b, target, value):
    return b.call("Set:Strada.UITextComponent.Text", Target=target, Value=value)


def string(b, value):
    return b.lit("string", value)


def select(b, condition, when_true, when_false):
    return b.pure("Select", Condition=condition, **{"True": when_true, "False": when_false})


def is_screen(b, name):
    return b.once(("screen", name), lambda: b.equal(b.var("Screen"), string(b, name)))


def tint(b, target, color):
    return b.call("Set:Strada.UIButtonComponent.NormalColor", Target=target, Value=b.lit("Strada.Color", color))


def click(b, target):
    return b.call("Call:Strada.UIButtonComponent.Click()", Target=target)


def settings_get(b, name, type_id, fallback):
    return b.pure("Call:Strada.Settings.Get<T>(string,T)", Name=name, Fallback=b.lit(type_id, fallback))


def settings_set(b, name, value):
    return b.call("Call:Strada.Settings.Set<T>(string,T)", Name=name, Value=value)


def to_int(b, value):
    return b.add("Convert", {"To": "int"}, "Result", Value=value).out


def binding_controls(b, action, binding):
    return b.pure("Call:Strada.Input.GetBindingControls(string,int)", Action=action, Binding=binding)


def rebind_is(b, rebind, status):
    return b.equal(b.pure("Get:Strada.RebindResult.Status", "Value", Target=rebind), b.lit("Strada.RebindStatus", status))


def send(b, name, comment=None, **arguments):
    return b.send(name, b.var("Race"), graph=Race.DIRECTOR, comment=comment, **arguments)


def percent(b, share):
    return b.fmt("{Percent}%", Percent=b.expr("int(round(share * 100))", share=share))


def bus_share(b, bus):
    """A bus's player volume as a share of the mix the game makes: 1 at 0 dB, 0 at silence."""
    return b.expr("pow(10, db / 20)", db=b.pure("Call:Strada.Settings.GetBusVolume(string)", Bus=bus))


def write_formats(author):
    Hud.write_clock(author, MENU)
    with author.region(MENU, "Control names: Key/W reads W, Gamepad/A reads PAD A", RULES, "ControlName") as b:
        controls = b.entry["Controls"]
        plain = b.pure("String.Replace", Text=b.pure("String.Replace", Text=b.pure("String.Replace", Text=controls, Old="Key/", New=""),
                                                     Old="Mouse/", New="MOUSE "), Old="Gamepad/", New="PAD ")
        b.link(select(b, b.pure("String.IsEmpty", Text=controls), string(b, "NONE"), b.pure("String.ToUpper", Text=plain)),
               b.ret["Name"])
    with author.region(MENU, "Desired: the screen the race director's state and pause ask for", RULES, "Desired") as b:
        state = director(b, "State")
        none = string(b, "")
        by_state = select(b, b.equal(state, string(b, "Title")), string(b, TITLE),
                          select(b, b.equal(state, string(b, "Results")), string(b, RESULTS), none))
        paused = select(b, director(b, "Paused"), string(b, PAUSE), by_state)
        b.link(select(b, b.var("ShowMenus"), paused, none), b.ret["Name"])


def write_items(author):
    with author.region(MENU, "Item: the screen's button at an index (a row's lower or upper)", RULES, "Item") as b:
        screens = b.add("Flow.Switch", {"Type": "string", "Cases": list(SCREENS)}, Selection=b.var("Screen"))
        index = b.entry["Index"]
        found = {
            TITLE: b.set("Found", element(b, "TitleItems", index)),
            RESULTS: b.set("Found", element(b, "ResultsItems", index)),
            PAUSE: b.set("Found", element(b, "PauseItems", index)),
            SETTINGS: b.set("Found", select(b, b.entry["Upper"], element(b, "Upper", index), element(b, "Lower", index))),
        }
        b.chain(b.entry, screens)
        for name, setter in found.items():
            b.chain(screens[f"Case:{name}"], setter, b.ret)
        b.flow(screens["Default"], b.ret)
        b.link(b.var("Found"), b.ret["Button"])
    with author.region(MENU, "Count: how many buttons or rows the screen has", RULES, "Count") as b:
        screens = b.add("Flow.Switch", {"Type": "string", "Cases": list(SCREENS)}, Selection=b.var("Screen"))
        counts = {
            TITLE: b.set("Found", b.pure("Array.Length", "Length", Array=b.var("TitleItems"))),
            RESULTS: b.set("Found", b.pure("Array.Length", "Length", Array=b.var("ResultsItems"))),
            PAUSE: b.set("Found", b.pure("Array.Length", "Length", Array=b.var("PauseItems"))),
            SETTINGS: b.set("Found", b.pure("Array.Length", "Length", Array=b.var("RowKinds"))),
        }
        b.chain(b.entry, screens)
        for name, setter in counts.items():
            b.chain(screens[f"Case:{name}"], setter, b.ret)
        b.flow(screens["Default"], b.ret)
        b.link(b.var("Found"), b.ret["Count"])


def write_shows(author):
    with author.region(MENU, "Show title: the laps and the best lap", EFFECTS, "ShowTitle") as b:
        laps = text(b, b.var("TitleLaps"), b.fmt("TIME TRIAL   {Laps} LAPS", Laps=director(b, "Laps")))
        best = director(b, "BestLapTime")
        line = select(b, b.expr("best > 0", best=best), b.fmt("BEST LAP {Time}", Time=b.own("Clock", "Text", Seconds=best).out),
                      string(b, "NO BEST LAP YET"))
        b.chain(b.entry, laps, text(b, b.var("TitleBest"), line), b.ret)
    with author.region(MENU, "Show values: each setting as Settings reads it", RULES, "ShowValues") as b:
        def value(row):
            return element(b, "Values", row)

        quality = to_int(b, b.pure("Get:Strada.Settings.Quality", "Value"))
        quality_text = select(b, b.expr("q == 0", q=quality), string(b, QUALITIES[0]),
                              select(b, b.expr("q == 1", q=quality), string(b, QUALITIES[1]), string(b, QUALITIES[2])))
        mode = to_int(b, b.pure("Get:Strada.Settings.WindowMode", "Value"))
        mode_text = select(b, b.expr("m == 0", m=mode), string(b, MODES[0]), string(b, MODES[1]))
        size_text = b.fmt("{Width} x {Height}", Width=b.pure("Get:Strada.Settings.WindowWidth", "Value"),
                          Height=b.pure("Get:Strada.Settings.WindowHeight", "Value"))
        writes = [text(b, value(QUALITY_ROW), quality_text), text(b, value(WINDOW_ROW), mode_text), text(b, value(SIZE_ROW), size_text),
                  text(b, value(MASTER_ROW), percent(b, b.pure("Get:Strada.Settings.MasterVolume", "Value")))]
        for row, bus in zip(BUS_ROWS, BUSES):
            writes.append(text(b, value(row), percent(b, bus_share(b, bus))))
        linked = select(b, settings_get(b, LINKED, "bool", LINKED_ON), string(b, "ON"), string(b, "OFF"))
        writes.append(text(b, value(LINKED_ROW), linked))
        writes.append(text(b, value(SHARE_ROW), percent(b, settings_get(b, SHARE, "float", LINKED_SHARE))))
        b.chain(b.entry, *writes, b.ret)
    with author.region(MENU, "Show controls: each riding action's keyboard and gamepad binding", RULES, "ShowControls") as b:
        each = b.add("Flow.ForEach", Array=b.var("RowActions"))
        action = each["Element"]
        key = text(b, element(b, "KeyTexts", each["Index"]),
                   b.own("ControlName", "Name", Controls=binding_controls(b, action, KEYBOARD)).out)
        pad = text(b, element(b, "PadTexts", each["Index"]),
                   b.own("ControlName", "Name", Controls=binding_controls(b, action, GAMEPAD)).out)
        b.chain(b.entry, each)
        b.chain(each["Body"], key, pad)
        b.flow(each["Completed"], b.ret)
    with author.region(MENU, "Tint: the selected buttons in their own colour", EFFECTS, "Tint") as b:
        each = b.add("Flow.ForEach", Array=b.var("Buttons"))
        plain = tint(b, each["Element"], NORMAL)
        shown = b.branch(b.pure("Operator.Not", A=b.pure("String.IsEmpty", Text=b.var("Screen"))), comment="A screen is shown")
        kind = element(b, "RowKinds", b.var("Selected"))
        one_column = b.expr(f"inside && kind == {CONTROL}", inside=is_screen(b, SETTINGS), kind=kind)
        lower = b.own("Item", "Button", Index=b.var("Selected"), Upper=False)
        upper = b.own("Item", "Button", Index=b.var("Selected"), Upper=True)
        both = b.branch(b.pure("Operator.Not", A=one_column), comment="A setting's < and >, or a button: both; a riding action: "
                        "its chosen binding")
        lower_tint = tint(b, lower.out, SELECTED)
        upper_tint = tint(b, upper.out, SELECTED)
        column = tint(b, select(b, b.expr("column == 1", column=b.var("Column")), upper.out, lower.out), SELECTED)
        b.chain(b.entry, each)
        b.flow(each["Body"], plain)
        b.chain(each["Completed"], shown)
        b.chain(shown["True"], lower, upper, both)
        b.chain(both["True"], lower_tint, upper_tint, b.ret)
        b.chain(both["False"], column, b.ret)
        b.flow(shown["False"], b.ret)
    with author.region(MENU, "Show screen: one screen, its first button selected", EFFECTS, "ShowScreen") as b:
        name = b.entry["Name"]
        kept = b.set("Screen", name)
        visible = [show(b, b.var(variable), b.equal(b.var("Screen"), string(b, screen_name)))
                   for variable, screen_name in (("TitleScreen", TITLE), ("ResultsScreen", RESULTS), ("PauseScreen", PAUSE),
                                                 ("SettingsScreen", SETTINGS))]
        reset = [b.set("Selected", 0), b.set("Column", 0)]
        title = b.branch(is_screen(b, TITLE))
        settings = b.branch(is_screen(b, SETTINGS))
        tinted = b.own("Tint")
        b.chain(b.entry, kept, *visible, *reset, title)
        b.chain(title["True"], b.own("ShowTitle"), settings)
        b.flow(title["False"], settings)
        b.chain(settings["True"], b.own("ShowValues"), b.own("ShowControls"), text(b, b.var("SettingsStatus"), ""), tinted)
        b.flow(settings["False"], tinted)
        b.chain(tinted, b.ret)


def write_settings(author):
    with author.region(MENU, "Apply assists: the linked brakes' settings to the rider", EFFECTS, "ApplyAssists") as b:
        rider = b.var("Rider")
        rides = b.branch(b.runs(ASSIST_GRAPH, rider), comment="A rider whose riding graph takes them")
        # A value the player never set leaves the rider's own (the scene's), which stands in as Get's fallback.
        linked = b.set(LINKED, b.pure("Call:Strada.Settings.Get<T>(string,T)", Name=LINKED, Fallback=b.get(LINKED, ASSIST_GRAPH, rider)),
                       graph=ASSIST_GRAPH, target=rider)
        share = b.set(SHARE, b.pure("Call:Strada.Settings.Get<T>(string,T)", Name=SHARE, Fallback=b.get(SHARE, ASSIST_GRAPH, rider)),
                      graph=ASSIST_GRAPH, target=rider)
        counted = b.set("Applied", b.expr("applied + 1", applied=b.var("Applied")))
        b.chain(b.entry, rides)
        b.chain(rides["True"], linked, share, counted, b.ret)
        b.flow(rides["False"], b.ret)
    with author.region(MENU, "Close settings: saved, then the screen the race asks for", EFFECTS, "CloseSettings") as b:
        saved = b.call("Call:Strada.Settings.Save()", "Result", comment="Waits for the disk: a menu, never while riding")
        counted = b.set("Saves", b.expr("saves + (saved ? 1 : 0)", saves=b.var("Saves"), saved=saved.out))
        closed = b.set("SettingsOpen", False)
        shown = b.own("ShowScreen", Name=b.own("Desired", "Name").out)
        b.chain(b.entry, saved, counted, closed, shown, b.ret)
    with author.region(MENU, "Step: a setting one step down or up, then shown as it reads", RULES, "Step") as b:
        direction = b.entry["Direction"]
        rows = b.add("Flow.Switch", {"Type": "int", "Cases": list(range(len(STEPPERS)))}, Selection=b.entry["Row"])
        quality = b.expr("clamp(q + d, 0, 2)", q=to_int(b, b.pure("Get:Strada.Settings.Quality", "Value")), d=direction)
        set_quality = b.call("Set:Strada.Settings.Quality",
                             Value=b.add("Convert", {"To": "Strada.QualityPreset"}, "Result", Value=quality).out)
        mode = b.expr("clamp(m + d, 0, 1)", m=to_int(b, b.pure("Get:Strada.Settings.WindowMode", "Value")), d=direction)
        set_mode = b.call("Set:Strada.Settings.WindowMode", Value=b.add("Convert", {"To": "Strada.WindowMode"}, "Result", Value=mode).out)
        found = b.add("Array.IndexOf", None, "Index", Array=b.var("Widths"), Value=b.pure("Get:Strada.Settings.WindowWidth", "Value")).out
        count = b.pure("Array.Length", "Length", Array=b.var("Widths"))
        index = b.set("Index", b.expr("i < 0 ? (d > 0 ? 0 : n - 1) : clamp(i + d, 0, n - 1)", i=found, d=direction, n=count),
                      comment="A size not listed steps to the first or the last")
        sized = b.call("Call:Strada.Settings.SetWindowSize(int,int)", Width=element(b, "Widths", b.var("Index")),
                       Height=element(b, "Heights", b.var("Index")))
        master = b.call("Set:Strada.Settings.MasterVolume",
                        Value=b.expr("float(clamp(int(round(v * 10)) + d, 0, 10)) / 10.0",
                                     v=b.pure("Get:Strada.Settings.MasterVolume", "Value"), d=direction))
        bus = element(b, "BusNames", b.expr(f"row - {BUS_ROWS[0]}", row=b.entry["Row"]))
        share = b.set("Share", b.expr("clamp(int(round(share * 10)) + d, 0, 10)", share=bus_share(b, bus), d=direction))
        volume = b.call("Call:Strada.Settings.SetBusVolume(string,float)", Bus=bus,
                        Decibels=b.expr(f"share <= 0 ? {SILENCE} : 20 * log(float(share) / 10.0) / log(10)", share=b.var("Share")),
                        comment="A tenth of the game's mix a step; none is silence")
        linked = settings_set(b, LINKED, b.pure("Operator.Not", A=settings_get(b, LINKED, "bool", LINKED_ON)))
        rear = settings_set(b, SHARE, b.expr("float(clamp(int(round(s * 10)) + d, 0, 10)) / 10.0",
                                             s=settings_get(b, SHARE, "float", LINKED_SHARE), d=direction))
        apply = b.own("ApplyAssists")
        values = b.own("ShowValues")
        b.chain(b.entry, rows)
        b.chain(rows[f"Case:{QUALITY_ROW}"], set_quality, values)
        b.chain(rows[f"Case:{WINDOW_ROW}"], set_mode, values)
        b.chain(rows[f"Case:{SIZE_ROW}"], index, sized, values)
        b.chain(rows[f"Case:{MASTER_ROW}"], master, values)
        for row in BUS_ROWS:
            b.flow(rows[f"Case:{row}"], share)
        b.chain(share, volume, values)
        b.chain(rows[f"Case:{LINKED_ROW}"], linked, apply)
        b.chain(rows[f"Case:{SHARE_ROW}"], rear, apply)
        b.chain(apply, values)
        b.chain(values, b.ret)
        b.flow(rows["Default"], b.ret)


def write_rebinding(author):
    with author.region(MENU, "Show prompt: what the listening binding waits for", EFFECTS, "ShowPrompt") as b:
        keyboard = b.expr(f"binding == {KEYBOARD}", binding=b.var("BindBinding"))
        side = select(b, b.expr("part == 0", part=b.var("BindPart")), string(b, " LEFT"),
                      select(b, b.expr("part == 1", part=b.var("BindPart")), string(b, " RIGHT"), string(b, "")))
        ask = select(b, keyboard, b.fmt("PRESS A KEY FOR {Name}{Side}   ESC CANCELS", Name=b.var("BindName"), Side=side),
                     b.fmt("PRESS A PAD CONTROL FOR {Name}   ESC CANCELS", Name=b.var("BindName")))
        line = select(b, b.pure("String.IsEmpty", Text=b.var("Refused")), ask, b.fmt("{Refused}   {Ask}", Refused=b.var("Refused"), Ask=ask))
        b.chain(b.entry, text(b, b.var("SettingsStatus"), line), b.ret)
    with author.region(MENU, "Start bind: a riding action's binding listens for a control", EFFECTS, "StartBind") as b:
        offset = b.expr(f"row - {FIRST_CONTROL}", row=b.entry["Row"])
        action = b.set("BindAction", element(b, "RowActions", offset))
        name = b.set("BindName", element(b, "RowNames", offset))
        binding = b.set("BindBinding", b.entry["Binding"])
        part = b.set("BindPart", b.expr(f"composite && binding == {KEYBOARD} ? 0 : -1",
                                        composite=b.equal(b.var("BindAction"), string(b, COMPOSITE)), binding=b.entry["Binding"]),
                     comment="The lean's two keys one after the other: left, then right")
        before = b.set("Before", binding_controls(b, b.var("BindAction"), b.var("BindBinding")),
                       comment="Kept to give back an answer the binding cannot take")
        fresh = b.set("Refused", "")
        listen = b.call("Call:Strada.Input.StartRebind(string,int,int)", Action=b.var("BindAction"), Binding=b.var("BindBinding"),
                        Part=b.var("BindPart"))
        listening = b.set("Rebinding", True)
        b.chain(b.entry, action, name, binding, part, before, fresh, listen, listening, b.own("ShowPrompt"), b.ret)
    with author.region(MENU, "Poll rebind: a binding's answer taken, refused or kept", RULES, "PollRebind") as b:
        rebind = b.once("rebind", lambda: b.pure("Get:Strada.Input.CurrentRebind", "Value"))
        listening = b.branch(rebind_is(b, rebind, "Listening"), comment="The answer is polled; there is no event")
        done = b.branch(rebind_is(b, rebind, "Done"))
        controls = b.set("Controls", b.pure("Get:Strada.RebindResult.Controls", "Value", Target=rebind))

        def has(word):
            return b.pure("String.Contains", Text=b.var("Controls"), Substring=word)

        keyboard = b.expr(f"binding == {KEYBOARD}", binding=b.var("BindBinding"))
        unfit = b.set("Unfit", b.pure("Select", Condition=keyboard, **{"True": has("Gamepad/"),
                                                                     "False": b.pure("Operator.Or", A=has("Key/"), B=has("Mouse/"))}),
                      comment="The keyboard's binding takes keys, the gamepad's its controls")
        refused = b.branch(b.var("Unfit"))
        note = b.set("Refused", select(b, keyboard, string(b, "THE KEYBOARD TAKES KEYS"), string(b, "THE PAD TAKES ITS OWN CONTROLS")))
        back = b.call("Call:Strada.Input.Rebind(string,int,string,int)", Action=b.var("BindAction"), Binding=b.var("BindBinding"),
                      Controls=b.var("Before"), Part=-1, comment="The binding is given back as it was")
        again = b.call("Call:Strada.Input.StartRebind(string,int,int)", Action=b.var("BindAction"), Binding=b.var("BindBinding"),
                       Part=b.var("BindPart"))
        first_part = b.branch(b.expr("part == 0", part=b.var("BindPart")), comment="The lean's left key taken: now its right")
        right = b.set("BindPart", 1)
        now = b.set("Before", binding_controls(b, b.var("BindAction"), b.var("BindBinding")))
        next_key = b.call("Call:Strada.Input.StartRebind(string,int,int)", Action=b.var("BindAction"), Binding=b.var("BindBinding"),
                          Part=1)
        named = b.set("Conflicts", b.pure("String.ToUpper", Text=b.pure("String.Join", Parts=b.pure("Get:Strada.RebindResult.Conflicts",
                                                                                                      "Value", Target=rebind),
                                                                      Separator=", ")))
        ended = b.set("Rebinding", False)
        counted = b.set("Rebinds", b.expr("rebinds + 1", rebinds=b.var("Rebinds")))
        current = b.own("ControlName", "Name", Controls=binding_controls(b, b.var("BindAction"), b.var("BindBinding"))).out
        answer = select(b, b.pure("String.IsEmpty", Text=b.var("Conflicts")), b.fmt("{Name}: {Control}", Name=b.var("BindName"), Control=current),
                        b.fmt("{Name}: {Control}   ALSO BOUND TO {Others}", Name=b.var("BindName"), Control=current,
                              Others=b.var("Conflicts")))
        answered = text(b, b.var("SettingsStatus"), answer)
        stopped = b.set("Rebinding", False, comment="Cancelled, timed out or refused: the binding keeps its control")
        kept = text(b, b.var("SettingsStatus"), b.fmt("{Name} KEPT: {Control}", Name=b.var("BindName"), Control=current))
        shown = b.own("ShowControls")
        b.chain(b.entry, listening)
        b.flow(listening["True"], b.ret)
        b.flow(listening["False"], done)
        b.chain(done["True"], controls, unfit, refused)
        b.chain(refused["True"], note, back, again, b.own("ShowPrompt"), b.ret)
        b.flow(refused["False"], first_part)
        b.chain(first_part["True"], right, now, next_key, b.own("ShowPrompt"), b.ret)
        b.chain(first_part["False"], named, ended, counted, answered, shown)
        b.chain(done["False"], stopped, kept, shown)
        b.chain(shown, b.ret)


def write_navigation(author):
    with author.region(MENU, "Show results: each lap, the total and the best lap", EFFECTS, "ShowResults") as b:
        laps = text(b, b.var("ResultsLaps"), b.pure("String.Join", Parts=b.var("LapLines"), Separator="\n"))
        total = text(b, b.var("ResultsTotal"), b.fmt("TOTAL {Time}", Time=b.own("Clock", "Text", Seconds=b.entry["Total"]).out))
        best_time = b.own("Clock", "Text", Seconds=director(b, "BestLapTime")).out
        best = text(b, b.var("ResultsBest"), select(b, b.var("NewBest"), b.fmt("NEW BEST LAP {Time}", Time=best_time),
                                                    b.fmt("BEST LAP {Time}", Time=best_time)))
        b.chain(b.entry, laps, total, best, b.ret)
    with author.region(MENU, "Navigate: the selection up or down the screen, round", RULES, "Navigate") as b:
        counted = b.own("Count", "Count")
        kept = b.set("Items", counted.out)
        moved = b.set("Selected", b.expr("(selected + delta + count) % count", selected=b.var("Selected"), delta=b.entry["Delta"],
                                         count=b.var("Items")))
        some = b.branch(b.expr("count > 0", count=b.var("Items")))
        b.chain(b.entry, counted, kept, some)
        b.chain(some["True"], moved, b.own("Tint"), b.ret)
        b.flow(some["False"], b.ret)
    with author.region(MENU, "Side: a setting stepped, or a riding action's binding chosen", RULES, "Side") as b:
        settings = b.branch(is_screen(b, SETTINGS))
        kind = element(b, "RowKinds", b.var("Selected"))
        kinds = b.add("Flow.Switch", {"Type": "int", "Cases": [STEPPER, CONTROL]}, Selection=kind)
        upper = b.expr("direction > 0", direction=b.entry["Direction"])
        stepper = b.own("Item", "Button", Index=b.var("Selected"), Upper=upper)
        stepped = click(b, stepper.out)
        column = b.set("Column", b.expr("direction > 0 ? 1 : 0", direction=b.entry["Direction"]))
        b.chain(b.entry, settings)
        b.chain(settings["True"], kinds)
        b.chain(kinds[f"Case:{STEPPER}"], stepper, stepped, b.ret)
        b.chain(kinds[f"Case:{CONTROL}"], column, b.own("Tint"), b.ret)
        b.flow(kinds["Default"], b.ret)
        b.flow(settings["False"], b.ret)
    with author.region(MENU, "Press: the selected button clicked, as the mouse clicks it", RULES, "Press") as b:
        kind = element(b, "RowKinds", b.var("Selected"))
        upper = b.expr(f"inside && (kind == {STEPPER} || (kind == {CONTROL} && column == 1))", inside=is_screen(b, SETTINGS), kind=kind,
                       column=b.var("Column"))
        item = b.own("Item", "Button", Index=b.var("Selected"), Upper=upper)
        known = b.branch(b.pure("IsValid", Value=item.out))
        b.chain(b.entry, item, known)
        b.chain(known["True"], click(b, item.out), b.ret)
        b.flow(known["False"], b.ret)
    with author.region(MENU, "Go back: the settings saved, the game resumed, the title", RULES, "GoBack") as b:
        screens = b.add("Flow.Switch", {"Type": "string", "Cases": [SETTINGS, PAUSE, RESULTS]}, Selection=b.var("Screen"))
        b.chain(b.entry, screens)
        b.chain(screens[f"Case:{SETTINGS}"], b.own("CloseSettings"), b.ret)
        b.chain(screens[f"Case:{PAUSE}"], send(b, "SetPaused", Paused=False), b.ret)
        b.chain(screens[f"Case:{RESULTS}"], send(b, "AbandonRace"), b.ret)
        b.flow(screens["Default"], b.ret)
    with author.region(MENU, "Command: what a clicked button does", RULES, "Command") as b:
        index = b.set("Index", b.add("Array.IndexOf", None, "Index", Array=b.var("Buttons"), Value=b.entry["Button"]).out)
        listed = b.branch(b.expr("index >= 0", index=b.var("Index")), comment="One of the menus' buttons")
        kinds = b.add("Flow.Switch", {"Type": "int", "Cases": list(range(len(KINDS)))},
                      Selection=element(b, "Kinds", b.var("Index")))
        row = element(b, "Rows", b.var("Index"))
        argument = element(b, "Arguments", b.var("Index"))
        opened = [b.set("SettingsFrom", b.var("Screen")), b.set("SettingsOpen", True)]
        resume = b.branch(b.var("Paused"), comment="Quit to the title: the game goes on")
        b.chain(b.entry, index, listed)
        b.chain(listed["True"], kinds)
        b.flow(listed["False"], b.ret)
        b.chain(kinds[f"Case:{KIND['Race']}"], send(b, "StartRace"), b.ret)
        b.chain(kinds[f"Case:{KIND['Settings']}"], *opened, b.own("ShowScreen", Name=SETTINGS), b.ret)
        b.chain(kinds[f"Case:{KIND['Quit']}"], b.call("Call:Strada.Application.Quit()"), b.ret)
        b.chain(kinds[f"Case:{KIND['Title']}"], resume)
        resumed = send(b, "SetPaused", Paused=False)
        abandoned = send(b, "AbandonRace")
        b.chain(resume["True"], resumed, abandoned)
        b.flow(resume["False"], abandoned)
        b.chain(abandoned, b.ret)
        b.chain(kinds[f"Case:{KIND['Resume']}"], send(b, "SetPaused", Paused=False), b.ret)
        b.chain(kinds[f"Case:{KIND['Step']}"], b.own("Step", Row=row, Direction=argument), b.ret)
        b.chain(kinds[f"Case:{KIND['Bind']}"], b.own("StartBind", Row=row, Binding=argument), b.ret)
        b.chain(kinds[f"Case:{KIND['Reset']}"], b.call("Call:Strada.Input.ResetBindings()", "Result"), b.own("ShowControls"),
                text(b, b.var("SettingsStatus"), "CONTROLS RESET"), b.ret)
        b.chain(kinds[f"Case:{KIND['Back']}"], b.own("CloseSettings"), b.ret)
        b.flow(kinds["Default"], b.ret)


def write_events(author):
    with author.region(MENU, "Start: the settings shown, and the linked brakes given to the rider", EVENTS) as b:
        settle = b.add("Flow.WaitFrames", Frames=1, comment="Every entity has run its Create first")
        b.chain(b.event("Event.Create"), settle)
        b.chain(settle["Completed"], b.own("ApplyAssists"), b.own("ShowValues"), b.own("ShowControls"))
    with author.region(MENU, "Read the race: each frame, the screen it asks for", EVENTS) as b:
        update = b.event("Event.Update")
        state = b.set("State", director(b, "State"))
        paused = b.set("Paused", director(b, "Paused"))
        listening = b.branch(b.var("Rebinding"), comment="While a binding listens, the answer is the player's")
        open_settings = b.branch(b.var("SettingsOpen"))
        origin = b.branch(b.expr("pause ? paused : title", pause=b.equal(b.var("SettingsFrom"), string(b, PAUSE)), paused=b.var("Paused"),
                                 title=b.equal(b.var("State"), string(b, "Title"))),
                          comment="The screen the settings came from still shows: they stay")
        desired = b.own("Desired", "Name").out
        changed = b.branch(b.pure("Operator.NotEqual", A=desired, B=b.var("Screen")))
        known = b.branch(b.runs(Race.DIRECTOR, b.var("Race")))
        b.chain(update, known)
        b.chain(known["True"], state, paused, listening)
        b.flow(listening["True"], b.own("PollRebind"))
        b.flow(listening["False"], open_settings)
        b.flow(open_settings["True"], origin)
        b.flow(origin["False"], b.own("CloseSettings"))
        b.flow(open_settings["False"], changed)
        b.flow(changed["True"], b.own("ShowScreen", Name=desired))
    for action, function, argument, title in (
            ("MenuUp", "Navigate", ("Delta", -1), "Up: the selection moves up"),
            ("MenuDown", "Navigate", ("Delta", 1), "Down: the selection moves down"),
            ("MenuLeft", "Side", ("Direction", -1), "Left: a setting steps down, or the keyboard's binding"),
            ("MenuRight", "Side", ("Direction", 1), "Right: a setting steps up, or the gamepad's binding"),
            ("Confirm", "Press", None, "Confirm: the selected button is pressed"),
            ("Back", "GoBack", None, "Back: the screen is left")):
        with author.region(MENU, title, EVENTS) as b:
            pressed = b.event("Event.ActionPressed", {"Action": action})
            ready = b.branch(b.expr("!listening && shown", listening=b.var("Rebinding"),
                                    shown=b.pure("Operator.Not", A=b.pure("String.IsEmpty", Text=b.var("Screen")))),
                             comment="A screen shows and no binding listens")
            call = b.own(function, **({argument[0]: argument[1]} if argument else {}))
            b.chain(pressed, ready)
            b.flow(ready["True"], call)
    with author.region(MENU, "Click: a button the mouse clicked, or the pad pressed", EVENTS) as b:
        clicked = b.event("Event.Click")
        ready = b.branch(b.pure("Operator.Not", A=b.var("Rebinding")), comment="While a binding listens, a click is no command")
        b.chain(clicked, ready)
        b.flow(ready["True"], b.own("Command", Button=clicked["Button"]))
    with author.region(MENU, "Race started: no laps yet", EVENTS) as b:
        b.chain(b.custom("RaceStarted"), b.set("LapLines", b.lit("string[]", [])), b.set("NewBest", False))
    with author.region(MENU, "Lap finished: its time, a line of the results", EVENTS) as b:
        finished = b.custom("LapFinished")
        line = b.fmt("LAP {Lap}   {Time}", Lap=finished["Lap"], Time=b.own("Clock", "Text", Seconds=finished["Time"]).out)
        added = b.set("LapLines", b.pure("Array.Add", Array=b.var("LapLines"), Value=line))
        best = b.set("NewBest", b.pure("Operator.Or", A=b.var("NewBest"), B=finished["Best"]))
        b.chain(finished, added, best)
    with author.region(MENU, "Race finished: the results", EVENTS) as b:
        finished = b.custom("RaceFinished")
        b.chain(finished, b.own("ShowResults", Total=finished["Total"]))


def write(author):
    write_formats(author)
    write_items(author)
    write_shows(author)
    write_settings(author)
    write_rebinding(author)
    write_navigation(author)
    write_events(author)


if __name__ == "__main__":
    print(check())
