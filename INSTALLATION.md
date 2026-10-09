# Installing StremiBrew

StremiBrew is homebrew. It runs only on a console that can already run homebrew, which is something you'll have to do on your own.

Builds are on the [releases page](https://github.com/stupiditytries/StremiBrew/releases): a zip holding the `PPSA99710` folder for the PS5, and `StremiBrew.nro` for the Switch. To build them yourself, see [BUILD.md](BUILD.md).

## What you need

- A Stremio account, with the add-ons you want already installed on it. StremiBrew
  installs none itself.
- A second device with a browser (a phone will do) to sign in with.
- A network connection on the console.

## PS5

I've tested the software on a firmware 13.20 PS5 Digital. Theoretically it should work across firmwares, but please feel free to raise an issue if you run into any major problems you suspect are firmware related. 

**Needs**

- A PS5 that runs homebrew, with a loader that can start an app from a folder.
- A way to copy files to the console/mount point, such as an FTP payload.

**Steps**

1. Copy the whole `PPSA99710` folder to your mount point (usually `/data/homebrew/`), so that
   `/data/homebrew/PPSA99710/eboot.bin` exists. The folder also holds `sce_sys`,
   `sce_module` and `assets`; all of them are needed.
2. Let the loader pick the folder up. StremiBrew then appears on the home screen.
3. Start it.

**Updating**: close StremiBrew, then replace `eboot.bin` (and `assets`, if they changed)
in the same folder. Do not copy over an app that is running.

**Removing**: delete the `PPSA99710` folder.

## Switch

Runs on a Switch with Atmosphère. Tested on an original model.

**Steps**

1. Copy `StremiBrew.nro` to the `switch` folder on the SD card.
2. Open the Homebrew Menu and start StremiBrew. It is highly recommended that you run the app through Title Takeover (holding R when launching a game).

StremiBrew keeps its data in `/switch/StremiBrew/` on the SD card: your sign-in, settings
and a log (`log.txt`) that is useful when reporting a problem.

**Updating**: replace `StremiBrew.nro`.

**Removing**: delete `StremiBrew.nro` and the `/switch/StremiBrew/` folder.

### Things to know on the Switch

- Video is limited to 1080p. A 4K stream is played at 1080p, and Settings has a limit on
  the stream quality that is fetched.
- The interface switches to a larger handheld layout when undocked. Settings can turn
  that off or keep it on.
- If the console's clock is wrong, StremiBrew corrects the time for itself when it
  starts, so secure connections still work. It does not change the console's clock.
- Trailers on the home screen and subtitle auto-calibration are PS5 only. They may be implemented at a later date.

## Signing in

1. Open Settings in StremiBrew and choose to sign in. A link code appears.
2. On your other device, open the address shown on screen, sign in to Stremio there and
   enter the code.
3. StremiBrew signs in by itself a few seconds later, and your library, add-ons and watch
   progress appear.

Your password is never typed on the console.

## If something goes wrong

- **Nothing plays**: StremiBrew plays direct HTTP and HTTPS streams only. Streams marked
  "Torrent", "YouTube" or "Opens elsewhere" cannot be played.
- **No add-ons or an empty home screen**: check that the console is online and that you
  are signed in; add-ons are managed from your Stremio account on another device.
- **A crash or anything else**: open an issue on this repository, say which console and
  firmware, and attach the log if there is one. Please do not ask the Stremio team: this
  is not their app.
