# Super Mario Sunshine for New 3DS

An unofficial port of Super Mario Sunshine to the New Nintendo 3DS family (New 3DS, New 3DS XL, New 2DS XL).
It runs the game from **your own copy of the disc**: no game files are included or downloadable here.

This is an early hobby project. Expect bugs; see [Known issues](#known-issues).

## What you need

- A **New** 3DS, New 3DS XL or New 2DS XL with custom firmware and the Homebrew Launcher.
  The original 3DS / 2DS models are not supported.
- Your own disc image of **Super Mario Sunshine, USA/Canada (GMSE01), revision 0**.
- The 3DS DSP firmware, for sound (see step 3 below).
- `sms_3ds.3dsx` from the [latest release](https://github.com/crmsnbleyd/sms-3ds/releases/latest).

## Setting up

1. **Get an ISO or CISO of the game.**
   - `.iso` / `.gcm` or `.ciso`: works as is.
   - `.rvz`, `.wia` or `.gcz` (the formats Dolphin uses for its own dumps) doesn't work and needs converting:
     open [Dolphin](https://dolphin-emu.org), right-click the game in the game list, choose **Convert File...**,
     set **Format** to **ISO**, and click **Convert...**.

2. **Copy the game to the SD card** and rename it:

   - an ISO to `/3ds/sms-3ds/game.iso`
   - a CISO to `/3ds/sms-3ds/game.ciso`

3. **For sound, you need the DSP firmware** at `/3ds/dspfirm.cdc` on the SD card. Without it the game runs
   without sound. You probably have it already since the [3DS Hacks Guide](https://3ds.hacks.guide) has you dump it while
   setting up custom firmware. If the file isn't there, dump it from Luma's Rosalina menu:
   1. Press **L + D-Pad Down + SELECT** at the same time to open Rosalina (works from the HOME Menu or in any
      game).
   2. Choose **Miscellaneous options** and press **A**.
   3. Choose **Dump DSP firmware** and press **A**.
   4. Press **B** to continue, then **B** again to leave Rosalina.

4. **Copy `sms_3ds.3dsx`** to `/3ds/` on the SD card and start it from the Homebrew Launcher.

Your saves go to `/3ds/sms-3ds/card-a/` on the SD card.

## Controls

| 3DS | GameCube |
| --- | --- |
| Circle Pad | Control Stick |
| C-Stick | C-Stick (camera) |
| A / B / X / Y | A / B / X / Y |
| L / R | L / R |
| ZL | Z |
| ZR | R pressed part of the way: spray while walking |
| START | START |
| D-Pad | D-Pad |
| L + R + X | Swap the lower screen between the HUD and debug information |
| START + SELECT | Quit |
| Touch screen | Tap Skip to skip a movie; tap YES / NO (or another choice) to answer a question |

R is a full press, as when the GameCube's R clicks: Mario stands still to spray. ZR presses R part of the way, so
he sprays while he walks.

## Known issues

- The game runs at up to 30 fps and can slow down in busy scenes.
- The picture keeps the GameCube's 4:3 shape, with black bars at the sides of the top screen. Widescreen isn't
  implemented yet.

## Contributing

If you'd like to work on the port or build it yourself, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits and licence

Built on [sms-pc-port](https://github.com/chasem-dev/sms-pc-port) and the
[Super Mario Sunshine decompilation](https://github.com/doldecomp/sms). See [NOTICE](NOTICE).
Not affiliated with or endorsed by Nintendo.
