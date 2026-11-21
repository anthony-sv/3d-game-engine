"""The menus' checks, a module of Checks.MODULES: they join the flow test scene (Scenes/Flow.sscene) after the HUD's, when the
race director is back at its title (HudChecks abandons its last race), and they drive the menus as a player does: the
menu actions pressed by action overrides (one press each), and buttons pressed through UIButton.Click(), which is what
Confirm and a mouse click come to. One after another:

1. the title: its screen shows over the attract mode with RACE selected, the HUD hidden and the best lap the races before
   left (the second scripted race's, RaceChecks.SCRIPT); Confirm presses RACE and the countdown begins, the title gone;
2. the results: the race the countdown began is timed through three laps by crossings sent the director as its gates
   send them (FLOW, slower than the best lap, so nothing is saved), and the results screen shows each lap, the total and
   the best lap; Confirm presses CONTINUE, back to the title;
3. the settings: SETTINGS clicked, the screen's buttons clicked to step the quality down twice, the window's size up, the
   master volume and the Engine bus a tenth down, the linked brakes off and their share a tenth up, the automatic shifts
   off; Settings reads each back, the menus' rider (the scene's bike, whose riding graph is Menu.ASSIST_GRAPH, standing
   on the grid since the ride ended) has the three
   assists, the screen shows them, and BACK saves the settings and shows the title again (the window's mode is not
   clicked: a test run takes no display; the editor's case MotoPlayTests clicks it, which play mode keeps unapplied);
4. a rebinding: the throttle's keyboard button clicked listens for a control and says so, Input.CancelRebind() ends it
   with the throttle's key kept and named, and BACK leaves (the capture of a real key is an editor case's, which presses
   one through input.set);
5. the pad: MenuDown selects SETTINGS, Confirm presses it, MenuRight steps the selected quality up, MenuUp goes round to
   BACK, and Back saves and leaves;
6. the pause screen: Pause shows it, time stands still, and Confirm presses RESUME while it does;
7. a best lap kept from the pause screen: RACE pressed, a lap faster than the best sent the director (QUICK_LAP), then
   Pause and the pause screen's QUIT TO TITLE clicked before the race's end: the director saves the lap it made its best
   as it goes back to the title, and the save slot opened again holds it.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import Checks  # noqa: E402  (the test scenes, reporting and waiting)
import Hud  # noqa: E402  (the HUD's groups)
import Menu  # noqa: E402  (the menus)
import Race  # noqa: E402  (the race director)
import RaceChecks  # noqa: E402  (the best lap the scripted races left)
from Checks import Suite, run  # noqa: E402
from Graphs import private  # noqa: E402

RIDER = "Bike"
# How long a check waits for the screen or the state it presses for (seconds of the game), and how many frames it lets go
# by after a press or a click before it reads (a click lands at the next frame's start, its command then runs).
WAIT = 5
SETTLE = 3
# The race the title's RACE begins: (gate, steps after the start signal) of each crossing sent the director, three laps of
# about 95, 96 and 97 s, each slower than the best lap, so the race makes none and saves nothing.
LAP_STEPS = (5700, 5760, 5820)
LINE_STEP = 30
# The last race's lap 1, faster than every lap before it (steps), and its two sector gates' steps into it.
QUICK_LAP = 4980
QUICK_SECTORS = (1660, 3320)


def _float(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def flow():
    """The crossings of the race the title begins: the line, then each lap's two sector gates and the line."""
    crossings, at = [(0, LINE_STEP)], LINE_STEP
    for steps in LAP_STEPS:
        crossings += [(1, at + steps // 3), (2, at + 2 * steps // 3), (0, at + steps)]
        at += steps
    return crossings


def clock(seconds):
    """Menu.sgraph's Clock, in the floats the graph computes in."""
    total = round(_float(max(seconds, 0.0) * 1000.0))
    return f"{total // 60000}:{total // 1000 % 60:02d}.{total % 1000:03d}"


def expected_results():
    """The results' texts: each lap's line, the total and the best lap, as the director times the crossings and the menus
    write them."""
    dt = _float(Race.STEP)
    times = [_float(_float(float(steps)) * dt) for _, steps in flow() if _ == 0]
    laps = [_float(after - before) for before, after in zip(times, times[1:])]
    best = RaceChecks.expected_races()["races"][-1]["best"]
    assert all(lap > best for lap in laps), "the title's race beats the best lap"
    lines = "\n".join(f"LAP {number}   {clock(lap)}" for number, lap in enumerate(laps, 1))
    return lines, f"TOTAL {clock(_float(times[-1] - times[0]))}", f"BEST LAP {clock(best)}", f"BEST LAP {clock(best)}"


def quick_lap():
    """The last race's lap 1 as the director times it in floats; it beats the scripted races' best."""
    dt = _float(Race.STEP)
    lap = _float(_float(_float(float(LINE_STEP + QUICK_LAP)) * dt) - _float(_float(float(LINE_STEP)) * dt))
    assert lap < RaceChecks.expected_races()["races"][-1]["best"], "the last race's lap beats no best lap"
    return lap


RESULTS_LAPS, RESULTS_TOTAL, RESULTS_BEST, TITLE_BEST = expected_results()
QUICK = quick_lap()
# The settings the clicks leave: the quality two steps down from High, the window's size the next listed past the game's
# 1600 by 900, the master volume and the Engine bus a tenth down, the linked brakes off and their share a tenth up, the
# automatic shifts off.
CLICKS = (("Lower", Menu.QUALITY_ROW), ("Lower", Menu.QUALITY_ROW), ("Upper", Menu.SIZE_ROW), ("Lower", Menu.MASTER_ROW),
          ("Lower", Menu.BUS_ROWS[0]), ("Upper", Menu.LINKED_ROW), ("Upper", Menu.SHARE_ROW), ("Upper", Menu.AUTO_ROW))
SIZE = Menu.SIZES[2]
VOLUME_TOLERANCE = 0.001
DECIBEL_TOLERANCE = 0.01
VALUES = ((Menu.QUALITY_ROW, Menu.QUALITIES[0]), (Menu.SIZE_ROW, f"{SIZE[0]} x {SIZE[1]}"), (Menu.MASTER_ROW, "90%"),
          (Menu.BUS_ROWS[0], "90%"), (Menu.LINKED_ROW, "OFF"), (Menu.SHARE_ROW, "40%"), (Menu.AUTO_ROW, "OFF"))
THROTTLE_ROW = Menu.FIRST_CONTROL
BACK_ROW = Menu.BACK_ROW


def checks():
    return (
        ("CheckTitleMenu", "Title: its menu over the attract mode, RACE pressed by Confirm",
         "the title shows its menu with RACE selected, and Confirm presses RACE, which begins the countdown"),
        ("CheckResultsScreen", "Results: three laps, then CONTINUE pressed by Confirm",
         "the results show each lap, the total and the best lap, and Confirm presses CONTINUE, back to the title"),
        ("CheckSettingsClicks", "Settings: each button stepped through Click(), read back, saved",
         "the settings screen's buttons step each setting, which Settings reads back, and BACK saves them"),
        ("CheckRebindListens", "Rebind: a binding's button listens, a cancel keeps it",
         "a binding's button listens for the player's control, and a cancel keeps the binding and names it"),
        ("CheckMenuPad", "Pad: the selection moved, a setting stepped, the screen left",
         "the menu actions move the selection, step the selected setting, press the selected button and leave"),
        ("CheckPauseMenu", "Pause menu: RESUME pressed while time stands still",
         "the pause screen shows its menu while time stands still, and Confirm presses RESUME"),
        ("CheckQuitKeepsBest", "Quit to title: a best lap the race made is saved",
         "a race left by the pause screen's QUIT TO TITLE before its end saves the best lap it made"),
    )


# --- Reading and pressing ------------------------------------------------------------------------------------------------------

def menus(b):
    return b.once("menus", lambda: b.var("Menus"))


def menu(b, name):
    return b.get(name, Menu.MENU, menus(b))


def race(b):
    return b.once("race", lambda: menu(b, "Race"))


def state(b):
    return b.get("State", Race.DIRECTOR, race(b))


def visible(b, target):
    return b.pure("Get:Strada.UIRectComponent.Visible", "Value", Target=target)


def text_of(b, target):
    return b.pure("Get:Strada.UITextComponent.Text", "Value", Target=target)


def listed(b, variable, index):
    return b.add("Array.Get", None, "Element", Array=menu(b, variable), Index=index).out


def string(b, value):
    return b.lit("string", value)


def frames(b, count=SETTLE):
    return b.add("Flow.WaitFrames", Frames=count, comment="The press or the click has been taken")


def settle(b):
    """Lets SETTLE frames go by: (first, the last node's output)."""
    waited = frames(b)
    return waited, waited["Completed"]


def press(b, action):
    """Presses an action as a player does, once: (first, the last node's output) of the chain."""
    down = b.call("Call:Strada.Input.SetActionOverride(string,float)", Action=action, Value=1.0,
                  comment="Pressed from the next frame's evaluation")
    held = b.add("Flow.WaitFrames", Frames=1)
    up = b.call("Call:Strada.Input.ClearActionOverride(string)", Action=action)
    settled = frames(b)
    b.chain(down, held)
    b.flow(held["Completed"], up)
    b.chain(up, settled)
    return down, settled["Completed"]


def clicked(b, target):
    """Clicks a button through UIButton.Click(), as Confirm does: (first, the last node's output)."""
    click = b.call("Call:Strada.UIButtonComponent.Click()", Target=target, comment="Lands at the next frame's start")
    settled = frames(b)
    b.chain(click, settled)
    return click, settled["Completed"]


def waiting(b, done):
    return Checks.wait_until(b, done, WAIT)


def on_screen(b, name):
    return b.equal(menu(b, "Screen"), string(b, name))


def report(b, name, passed, detail):
    return b.library(Checks.CHECKS, "Check", Name=f"Moto: {name}", Passed=passed, Detail=detail)


# --- The checks ----------------------------------------------------------------------------------------------------------------

def write_title_menu(b, name):
    shown = waiting(b, b.both(on_screen(b, Menu.TITLE), visible(b, menu(b, "TitleScreen"))))
    hud = b.pure("Get:Strada.Entity.Parent", "Value", Target=menus(b))
    noted = [b.set("TitleShown", visible(b, menu(b, "TitleScreen"))),
             b.set("FirstSelected", menu(b, "Selected")),
             b.set("HudShown", b.pure("Operator.Or", A=visible(b, b.get("RidingPlates", Hud.HUD, hud)),
                                      B=visible(b, b.get("TimingPlates", Hud.HUD, hud)))),
             b.set("BestShown", text_of(b, menu(b, "TitleBest")))]
    pressed = press(b, "Confirm")
    counting = waiting(b, b.equal(state(b), string(b, "Countdown")))
    after = [b.set("CountingState", state(b)), b.set("TitleAfter", visible(b, menu(b, "TitleScreen")))]
    passed = b.both(b.expr("title && selected == 0 && !hud && !after", title=b.var("TitleShown"), selected=b.var("FirstSelected"),
                           hud=b.var("HudShown"), after=b.var("TitleAfter")),
                    b.both(b.equal(b.var("BestShown"), string(b, TITLE_BEST)), b.equal(b.var("CountingState"), string(b, "Countdown"))))
    detail = b.fmt(f"the title shown {{Title}} with button {{Selected}} selected, the HUD shown {{Hud}}, its best lap '{{Best}}' "
                   f"('{TITLE_BEST}'); after Confirm the race {{State}}, the title shown {{After}}", Title=b.var("TitleShown"),
                   Selected=b.var("FirstSelected"), Hud=b.var("HudShown"), Best=b.var("BestShown"), State=b.var("CountingState"),
                   After=b.var("TitleAfter"))
    run(b, shown, *noted, pressed, counting, settle(b), *after, report(b, name, passed, detail))


def write_results(b, name):
    racing = Checks.wait_until(b, b.equal(state(b), string(b, "Racing")), WAIT)
    start = b.get("StartStep", Race.DIRECTOR, race(b))
    sent = [b.send("GateCrossed", race(b), graph=Race.DIRECTOR, Gate=gate, Step=b.expr(f"start + {at}", start=start), Fraction=0.0)
            for gate, at in flow()]
    noted = [b.set("ResultsShown", visible(b, menu(b, "ResultsScreen"))), b.set("ScreenShown", menu(b, "Screen")),
             b.set("LapsShown", text_of(b, menu(b, "ResultsLaps"))), b.set("TotalShown", text_of(b, menu(b, "ResultsTotal"))),
             b.set("ResultBest", text_of(b, menu(b, "ResultsBest")))]
    pressed = press(b, "Confirm")
    back = waiting(b, b.equal(state(b), string(b, "Title")))
    titled = b.set("TitleAgain", b.both(visible(b, menu(b, "TitleScreen")), on_screen(b, Menu.TITLE)))
    passed = b.both(b.both(b.var("ResultsShown"), b.equal(b.var("ScreenShown"), string(b, Menu.RESULTS))),
                    b.both(b.both(b.equal(b.var("LapsShown"), string(b, RESULTS_LAPS)), b.equal(b.var("TotalShown"), string(b, RESULTS_TOTAL))),
                           b.both(b.equal(b.var("ResultBest"), string(b, RESULTS_BEST)), b.var("TitleAgain"))))
    shown_laps = RESULTS_LAPS.replace("\n", " / ")
    detail = b.fmt(f"the results shown {{Shown}} ({{Screen}}): laps '{{Laps}}' ('{shown_laps}'), '{{Total}}' ('{RESULTS_TOTAL}'), "
                   f"'{{Best}}' ('{RESULTS_BEST}'); after Confirm the title shown {{Title}}", Shown=b.var("ResultsShown"),
                   Screen=b.var("ScreenShown"), Laps=b.pure("String.Replace", Text=b.var("LapsShown"), Old="\n", New=" / "),
                   Total=b.var("TotalShown"), Best=b.var("ResultBest"), Title=b.var("TitleAgain"))
    run(b, racing, *sent, settle(b), *noted, pressed, back, settle(b), titled, report(b, name, passed, detail))


def write_settings_clicks(b, name):
    opened = clicked(b, listed(b, "TitleItems", 1))
    shown = waiting(b, on_screen(b, Menu.SETTINGS))
    applied = b.set("AppliedBefore", menu(b, "Applied"))
    presses = [clicked(b, listed(b, side, row)) for side, row in CLICKS]
    quality = b.add("Convert", {"To": "int"}, "Result", Value=b.pure("Get:Strada.Settings.Quality", "Value")).out
    linked = b.pure("Call:Strada.Settings.Get<T>(string,T)", Name=Menu.LINKED, Fallback=b.lit("bool", True))
    share = b.pure("Call:Strada.Settings.Get<T>(string,T)", Name=Menu.SHARE, Fallback=b.lit("float", -1.0))
    auto = b.pure("Call:Strada.Settings.Get<T>(string,T)", Name=Menu.AUTO, Fallback=b.lit("bool", True))
    engine = b.pure("Call:Strada.Settings.GetBusVolume(string)", Bus=Menu.BUSES[0])
    rider = b.var("Rider")
    read = b.set("SettingsRead", b.expr(
        f"q == 0 && w == {SIZE[0]} && h == {SIZE[1]} && abs(master - 0.9) <= {VOLUME_TOLERANCE} && "
        f"abs(engine - 20 * log(0.9) / log(10)) <= {DECIBEL_TOLERANCE} && !linked && abs(share - 0.4) <= {VOLUME_TOLERANCE} && "
        "!auto", q=quality, w=b.pure("Get:Strada.Settings.WindowWidth", "Value"),
        h=b.pure("Get:Strada.Settings.WindowHeight", "Value"), master=b.pure("Get:Strada.Settings.MasterVolume", "Value"),
        engine=engine, linked=linked, share=share, auto=auto))
    given = b.set("RiderGiven", b.expr(f"!linked && abs(share - 0.4) <= {VOLUME_TOLERANCE} && !auto && applied > before",
                                       linked=b.get(Menu.LINKED, Menu.ASSIST_GRAPH, rider), share=b.get(Menu.SHARE, Menu.ASSIST_GRAPH, rider),
                                       auto=b.get(Menu.AUTO, Menu.ASSIST_GRAPH, rider), applied=menu(b, "Applied"),
                                       before=b.var("AppliedBefore")))
    texts = [text_of(b, listed(b, "Values", row)) for row, _ in VALUES]
    shown_values = b.set("ValuesShown", b.pure("String.Join", Parts=b.add("Array.Make", {"Type": "string", "Count": len(texts)}, "Array",
                                                                           **{f"Element{index}": value for index, value in enumerate(texts)}).out,
                                               Separator=", "))
    saved_before = b.set("SavesBefore", menu(b, "Saves"))
    back = clicked(b, listed(b, "Lower", BACK_ROW))
    titled = waiting(b, on_screen(b, Menu.TITLE))
    saved = b.set("SavedOnce", b.expr("saves == before + 1", saves=menu(b, "Saves"), before=b.var("SavesBefore")))
    expected = ", ".join(value for _, value in VALUES)
    passed = b.both(b.both(b.var("SettingsRead"), b.var("RiderGiven")),
                    b.both(b.equal(b.var("ValuesShown"), string(b, expected)), b.var("SavedOnce")))
    detail = b.fmt(f"Settings read back {{Read}} (quality {{Quality}}, {{Width}} x {{Height}}, master {{Master}}, engine {{Engine}} dB, "
                   f"linked {{Linked}}, share {{Share}}, auto shift {{Auto}}); the rider given them {{Given}}; the screen '{{Values}}' "
                   f"('{expected}'); "
                   f"saved once {{Saved}}", Read=b.var("SettingsRead"), Quality=quality,
                   Width=b.pure("Get:Strada.Settings.WindowWidth", "Value"), Height=b.pure("Get:Strada.Settings.WindowHeight", "Value"),
                   Master=b.pure("Get:Strada.Settings.MasterVolume", "Value"), Engine=engine, Linked=linked, Share=share, Auto=auto,
                   Given=b.var("RiderGiven"), Values=b.var("ValuesShown"), Saved=b.var("SavedOnce"))
    run(b, opened, shown, applied, *presses, read, given, shown_values, saved_before, back, titled, saved, report(b, name, passed, detail))


def write_rebind_listens(b, name):
    opened = clicked(b, listed(b, "TitleItems", 1))
    shown = waiting(b, on_screen(b, Menu.SETTINGS))
    listen = clicked(b, listed(b, "Lower", THROTTLE_ROW))
    status = b.pure("Get:Strada.RebindResult.Status", "Value", Target=b.pure("Get:Strada.Input.CurrentRebind", "Value"))
    noted = [b.set("Listening", b.both(menu(b, "Rebinding"), b.equal(status, b.lit("Strada.RebindStatus", "Listening")))),
             b.set("Asked", text_of(b, menu(b, "SettingsStatus")))]
    cancel = b.call("Call:Strada.Input.CancelRebind()", comment="As Escape cancels a capture")
    kept = [b.set("StillListening", menu(b, "Rebinding")), b.set("Kept", text_of(b, menu(b, "SettingsStatus")))]
    back = clicked(b, listed(b, "Lower", BACK_ROW))
    titled = waiting(b, on_screen(b, Menu.TITLE))
    asked, answered = "PRESS A KEY FOR THROTTLE   ESC CANCELS", "THROTTLE KEPT: W"
    passed = b.both(b.both(b.var("Listening"), b.equal(b.var("Asked"), string(b, asked))),
                    b.both(b.pure("Operator.Not", A=b.var("StillListening")), b.equal(b.var("Kept"), string(b, answered))))
    detail = b.fmt(f"listening {{Listening}}, the screen '{{Asked}}' ('{asked}'); cancelled, listening {{Still}}, the screen "
                   f"'{{Kept}}' ('{answered}')", Listening=b.var("Listening"), Asked=b.var("Asked"), Still=b.var("StillListening"),
                   Kept=b.var("Kept"))
    run(b, opened, shown, listen, *noted, cancel, settle(b), *kept, back, titled, report(b, name, passed, detail))


def write_menu_pad(b, name):
    down = press(b, "MenuDown")
    selected = b.set("PadSelected", menu(b, "Selected"))
    confirm = press(b, "Confirm")
    shown = waiting(b, on_screen(b, Menu.SETTINGS))
    quality = b.add("Convert", {"To": "int"}, "Result", Value=b.pure("Get:Strada.Settings.Quality", "Value")).out
    before = b.set("QualityBefore", quality)
    right = press(b, "MenuRight")
    after = b.set("QualityAfter", quality)
    up = press(b, "MenuUp")
    round_ = b.set("RoundSelected", menu(b, "Selected"))
    saves = b.set("SavesBefore", menu(b, "Saves"))
    leave = press(b, "Back")
    titled = waiting(b, on_screen(b, Menu.TITLE))
    saved = b.set("SavedOnce", b.expr("saves == before + 1", saves=menu(b, "Saves"), before=b.var("SavesBefore")))
    passed = b.both(b.expr(f"selected == 1 && after == before + 1 && round == {BACK_ROW}", selected=b.var("PadSelected"),
                           before=b.var("QualityBefore"), after=b.var("QualityAfter"), round=b.var("RoundSelected")),
                    b.both(b.var("SavedOnce"), on_screen(b, Menu.TITLE)))
    detail = b.fmt(f"MenuDown selected {{Selected}} (1); MenuRight stepped the quality {{Before}} to {{After}}; MenuUp went round "
                   f"to row {{Round}} ({BACK_ROW}); Back saved once {{Saved}}", Selected=b.var("PadSelected"),
                   Before=b.var("QualityBefore"), After=b.var("QualityAfter"), Round=b.var("RoundSelected"), Saved=b.var("SavedOnce"))
    run(b, down, selected, confirm, shown, before, right, after, up, round_, saves, leave, titled, saved, report(b, name, passed, detail))


def write_pause_menu(b, name):
    paused = press(b, Race.PAUSE)
    shown = waiting(b, on_screen(b, Menu.PAUSE))
    noted = [b.set("PauseShown", visible(b, menu(b, "PauseScreen"))), b.set("Scale", b.pure("Get:Strada.Time.TimeScale", "Value")),
             b.set("PauseSelected", menu(b, "Selected"))]
    resumed = press(b, "Confirm")
    back = waiting(b, on_screen(b, Menu.TITLE))
    after = [b.set("ResumedScale", b.pure("Get:Strada.Time.TimeScale", "Value")),
             b.set("DirectorPaused", b.get("Paused", Race.DIRECTOR, race(b)))]
    passed = b.both(b.expr("shown && scale == 0 && selected == 0 && resumed == 1 && !paused", shown=b.var("PauseShown"),
                           scale=b.var("Scale"), selected=b.var("PauseSelected"), resumed=b.var("ResumedScale"),
                           paused=b.var("DirectorPaused")), on_screen(b, Menu.TITLE))
    detail = b.fmt("paused: the screen shown {Shown}, time scale {Scale}, button {Selected} selected; Confirm: time scale "
                   "{Resumed}, the director paused {Paused}", Shown=b.var("PauseShown"), Scale=b.var("Scale"),
                   Selected=b.var("PauseSelected"), Resumed=b.var("ResumedScale"), Paused=b.var("DirectorPaused"))
    run(b, paused, shown, *noted, resumed, back, *after, report(b, name, passed, detail))


def write_quit_keeps_best(b, name):
    director = race(b)
    before = b.set("BestSavesBefore", b.get("Saves", Race.DIRECTOR, director))
    raced = clicked(b, listed(b, "TitleItems", 0))
    racing = Checks.wait_until(b, b.equal(state(b), string(b, "Racing")), WAIT)
    start = b.get("StartStep", Race.DIRECTOR, director)
    crossings = [(0, LINE_STEP)] + [(gate, LINE_STEP + steps) for gate, steps in zip((1, 2), QUICK_SECTORS)] + \
        [(0, LINE_STEP + QUICK_LAP)]
    sent = [b.send("GateCrossed", director, graph=Race.DIRECTOR, Gate=gate, Step=b.expr(f"start + {at}", start=start), Fraction=0.0)
            for gate, at in crossings]
    best = b.set("BestKept", b.get("BestLapTime", Race.DIRECTOR, director))
    unsaved = b.set("SavesRacing", b.get("Saves", Race.DIRECTOR, director))
    paused = press(b, Race.PAUSE)
    shown = waiting(b, on_screen(b, Menu.PAUSE))
    quit_ = clicked(b, listed(b, "PauseItems", 2))
    titled = waiting(b, b.equal(state(b), string(b, "Title")))
    opened = b.call("Call:Strada.SaveSlot.Open(string)", "Result", Name=Race.SLOT)
    slot = b.set("QuitSlot", opened.out)
    read = b.set("QuitSaved", b.pure("Call:Strada.SaveSlot.Get<T>(string,T)", Target=b.var("QuitSlot"), Name="LapTime",
                                     Fallback=b.lit("float", -1.0)))
    after = [b.set("SavesAfter", b.get("Saves", Race.DIRECTOR, director)),
             b.set("ScaleAfter", b.pure("Get:Strada.Time.TimeScale", "Value"))]
    passed = b.expr(f"abs(best - {round(QUICK, 5)}) <= {RaceChecks.TIME_TOLERANCE} && abs(saved - best) <= {RaceChecks.TIME_TOLERANCE} "
                    "&& racing == before && after == before + 1 && scale == 1", best=b.var("BestKept"), saved=b.var("QuitSaved"),
                    racing=b.var("SavesRacing"), before=b.var("BestSavesBefore"), after=b.var("SavesAfter"), scale=b.var("ScaleAfter"))
    detail = b.fmt(f"lap 1 made the best {{Best}} s (the lap's {QUICK:.3f}), saved {{Racing}} times while racing (from "
                   "{Before}); quit to the title, saved {After} times, the slot keeps {Saved} s, time scale {Scale}",
                   Best=b.var("BestKept"), Racing=b.var("SavesRacing"), Before=b.var("BestSavesBefore"), After=b.var("SavesAfter"),
                   Saved=b.var("QuitSaved"), Scale=b.var("ScaleAfter"))
    run(b, before, raced, racing, *sent, best, unsaved, paused, shown, quit_, titled, settle(b), opened, slot, read, *after,
        report(b, name, passed, detail))


WRITERS = {
    "CheckTitleMenu": write_title_menu,
    "CheckResultsScreen": write_results,
    "CheckSettingsClicks": write_settings_clicks,
    "CheckRebindListens": write_rebind_listens,
    "CheckMenuPad": write_menu_pad,
    "CheckPauseMenu": write_pause_menu,
    "CheckQuitKeepsBest": write_quit_keeps_best,
}


def write_check(b, function, name):
    WRITERS[function](b, name)


# --- The scene ---------------------------------------------------------------------------------------------------------------

ENTITIES = (("Menus", Menu.ROOT, "The screen's menus, which the checks press as a player does"),
            ("Rider", RIDER, "The bike, the menus' rider, whose riding graph takes the assists' settings"))
VARIABLES = (
    private("TitleShown", "bool", False), private("FirstSelected", "int", -1), private("HudShown", "bool", True),
    private("BestShown", "string", ""), private("CountingState", "string", ""), private("TitleAfter", "bool", True),
    private("ResultsShown", "bool", False), private("ScreenShown", "string", ""), private("LapsShown", "string", ""),
    private("TotalShown", "string", ""), private("ResultBest", "string", ""), private("TitleAgain", "bool", False),
    private("AppliedBefore", "int", 0), private("SettingsRead", "bool", False), private("RiderGiven", "bool", False),
    private("ValuesShown", "string", ""), private("SavesBefore", "int", 0), private("SavedOnce", "bool", False),
    private("Listening", "bool", False), private("Asked", "string", ""), private("StillListening", "bool", True),
    private("Kept", "string", ""), private("PadSelected", "int", -1), private("QualityBefore", "int", -1),
    private("QualityAfter", "int", -1), private("RoundSelected", "int", -1), private("PauseShown", "bool", False),
    private("Scale", "float", -1.0), private("PauseSelected", "int", -1), private("ResumedScale", "float", -1.0),
    private("DirectorPaused", "bool", True), private("BestKept", "float", -1.0), private("BestSavesBefore", "int", -1),
    private("SavesRacing", "int", -1),
    private("QuitSlot", Race.SAVE_SLOT), private("QuitSaved", "float", -1.0), private("SavesAfter", "int", -1),
    private("ScaleAfter", "float", -1.0),
)


def prepare(session, found):
    """The menus give the assists' settings to the scene's bike, whatever the main scene's menus have."""
    Checks.set_variables(session, found[Menu.ROOT], {"Rider": Checks.entity_value(found[RIDER])})


SUITE = Suite(checks, write_check, ENTITIES, VARIABLES, prepare=prepare)
