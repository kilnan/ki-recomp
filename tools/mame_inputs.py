"""Scripted inputs for MAME runs of kinst (shared by the capture/coverage tools).

play_script() inserts a coin, starts a 1-player game, picks a character and
then presses random (seeded) directions and buttons, which reaches fights.
input_lua() renders a Lua frame callback that applies the events.
"""
import random


def play_script(frames, snap_every, seed, start=1500):
    rng = random.Random(seed)
    ev = []  # (frame, port, field, value)

    def press(f, port, field, length=6):
        ev.append((f, port, field, 1))
        ev.append((f + length, port, field, 0))

    # First coin. 1500 frames (~25 s) is well into attract mode with the DRC;
    # MAME's interpreter enters the game only around frame 6900, so use ~8000.
    t = start
    press(t, ":P1", "Coin 1")
    press(t + 60, ":P1", "Coin 1")
    press(t + 120, ":P1", "1 Player Start")
    # character select: wander a bit, then confirm
    for i in range(6):
        press(t + 200 + 20 * i, ":P1", rng.choice(["P1 Left", "P1 Right"]))
    press(t + 340, ":P1", "P1 High Attack - Quick")
    buttons = ["P1 High Attack - Quick", "P1 High Attack - Medium", "P1 High Attack - Fierce",
               "P1 Low Attack - Quick", "P1 Low Attack - Medium", "P1 Low Attack - Fierce",
               "P1 Left", "P1 Right", "P1 Up", "P1 Down", "1 Player Start"]
    f = t + 400
    while f < frames - 10:
        press(f, ":P1", rng.choice(buttons), rng.randint(3, 15))
        f += rng.randint(5, 25)
    snaps = list(range(snap_every, frames, snap_every))
    return sorted(ev), snaps



def input_lua(events, snaps=(), frames=None):
    ev = ",\n".join(f'{{{f},"{p}","{n}",{v}}}' for f, p, n, v in events)
    sn = ",".join(str(s) for s in snaps)
    stop = f"if frame >= {frames} then manager.machine:exit() end" if frames else ""
    return f"""
local ports = manager.machine.ioport.ports
local ki_ev = {{
{ev}
}}
local ki_snapset = {{}}
for _, s in ipairs({{ {sn} }}) do ki_snapset[s] = true end
-- Count emulated frames from the screen, not frame_done calls: under host
-- load MAME skips frame updates, so a call counter drifts against emulated
-- time and the inputs land at different points of the game.
local ki_screen = manager.machine.screens[":screen"]
local ki_i = 1
emu.register_frame_done(function()
  local frame = ki_screen:frame_number()
  while ki_i <= #ki_ev and ki_ev[ki_i][1] <= frame do
    local e = ki_ev[ki_i]
    ports[e[2]].fields[e[3]]:set_value(e[4])
    ki_i = ki_i + 1
  end
  for f, _ in pairs(ki_snapset) do
    if frame >= f then manager.machine.video:snapshot(); ki_snapset[f] = nil end
  end
  {stop}
end)
"""
