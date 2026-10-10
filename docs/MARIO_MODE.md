# Mario Mode

Play Ocarina of Time as Mario, with his real Super Mario 64 moveset, physics, model and voice.

Mario comes from **libsm64**, a library built from the Super Mario 64 decompilation. Hyrule is the Ship of
Harkinian engine, built from the Ocarina of Time decompilation. Neither includes any game files: you supply both
ROMs yourself.

## Setup (Windows)

1. Unzip the build into its own folder. It contains `soh.exe` and `sm64.dll`.
2. Put your two ROMs in the same folder:
   - The Legend of Zelda: Ocarina of Time (any version Ship of Harkinian supports; check at https://ship.equipment/)
   - Super Mario 64 (USA), as `.z64`, `.n64` or `.v64`. The file name doesn't matter.
3. Run `soh.exe`. The first launch converts the Zelda ROM, which takes a minute or two.
4. Start or load a save file. Mario is on by default.

If Link shows up instead of Mario, press **F1** to open the menu and go to **Enhancements > Extra Modes**. The
status line under **Play as Mario** says what's missing (usually the SM64 ROM isn't in the folder).

## Controls

| Button | Mario |
| --- | --- |
| Stick | Run / walk |
| A | Jump (double, triple, long, side-flip, wall kick: all the SM64 moves) |
| B | Punch, kick, dive, slide kick |
| Z or R | Crouch, ground pound, long jump, backflip |
| C buttons | Link's items (bombs, bow, ocarina, hookshot...) |

When the A button on screen shows an action (Open, Speak, Check, Grab, Climb, Throw...), A does that instead of
jumping. Walk into vines and ladders to climb them, and into crawlspaces (as a kid) to crawl.

Mario's punches, kicks and dives hit like a sword; landing on enemies or ground pounding them hurts too. Your
hearts are Zelda's hearts.

## Known limits

- During doors, chests, cutscenes, climbing, crawling and knockback, Link's game code is in charge and Mario just
  follows along, so Mario's pose won't always match what's happening.
- Riding Epona, the sword (B) and the shield aren't available as Mario.
- Mario's jumps can reach places Link can't, so some puzzles can be skipped.
- Turn **Play as Mario** off at any time to get Link back.

## Settings

- **Mario Size**: lower makes Mario bigger. 3.2 is a bit shorter than adult Link.
- **Mario Voice & Sound Volume**: Mario's own sounds (wahoo!).
