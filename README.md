# Claude Agent for Miyoo Mini

Let Claude look at, fix and set up your Miyoo Mini Plus or Mini Flip, from the Claude app on your phone or computer.

Ask things like *"my game does not open"*, *"why is the battery draining so fast?"* or *"install this port and set it up for me"*: Claude checks the device, explains what it found, and, when you allow it, makes the changes itself. Every change is kept as a copy and can be undone.

For developers, it is a remote test bench: Claude can download builds to the SD card, launch your game, replay button presses with exact timing, take timed screenshots, watch logs, measure performance and catch crashes, all by itself.

> Unofficial community project. Not made or endorsed by Anthropic. Claude is an AI and can make mistakes: **back up your SD card** before asking it to change things.

## What you need

- A **Miyoo Mini Plus** or **Miyoo Mini Flip** (the models with Wi-Fi), with **OnionOS** or **RedOnion**.
- Wi-Fi turned on in the Miyoo's settings.
- A **Claude** account, on the Claude app (Android, iPhone) or on claude.ai. Nothing to install on a computer.

## Install

1. Download `ClaudeAgent-vX.Y.Z.zip` from the [latest release](../../releases/latest).
2. Extract it to the **root of the SD card** (it adds the folder `App/ClaudeAgent`).
3. On the Miyoo, open **Apps > Claude Agent**.

## Connect (only once)

The app shows a QR code, a site address and a 6-digit code.

1. Scan the QR code with your phone (or open the site on any device).
2. Follow the steps on that page: it adds the Miyoo connector to your Claude account.
3. When asked, type the code shown on the Miyoo.

That's it. Now open a chat in Claude with the Miyoo connector turned on and ask anything about your Miyoo. The app on the Miyoo shows Claude's answers and, step by step, everything it does.

Tip: in Claude's settings, the connector has an "Always allow" option, if you do not want Claude to ask you before each action on the chat side too.

## You stay in control

- **Permissions by type** — see the screen, read files, change files, press buttons, run commands. Each one can be set to *Ask every time*, *Ask once* (until you close the app) or *Don't ask*. The questions appear on the Miyoo; changes that lower the security show a warning first.
- **Claude's plan first** — before a big change, Claude shows its plan on the Miyoo and waits for your OK.
- **Copies and undo** — every file Claude changes or deletes is kept in the app's `backups` folder. *Settings > Undo changes made by Claude* puts any of them back. Old copies can be deleted automatically (after 30 days by default), or all at once to free space.
- **Only while the app is open** — by default Claude can work only while the app is on screen. *Keep working after leaving the app* (Off / Until restart / Always) lets it keep going, for example to open and test a game; with *Off*, Claude asks on the Miyoo when it needs it.
- **Disconnect** — *Settings > Disconnect every Claude* removes access from every connected Claude account.

## Plugins

Plugins add new tools to Claude, made for one job: a test routine for your game, a log analyzer, a benchmark, a tool to check or fix a specific port, anything a script or a program can do on the Miyoo.

- **Anyone can make one.** A plugin is a folder in `App/ClaudeAgent/plugins/<name>/` with a script or a program and a `plugin.json` that says what it does, which permission it needs and what inputs it takes. It runs on the Miyoo with full access, receives Claude's arguments and returns text or pictures.
- **Claude can make them too.** With *Developer tools* on, when a task needs a tool that does not exist yet, Claude can write the plugin itself and suggest it. Nothing is installed until you approve it on the Miyoo.
- **Same control as everything else.** Each plugin appears in *Settings > Developer options > Plugins*, where it can be turned off, removed, or set to ask every time, ask once or not ask.
- **Share them.** A plugin is just a folder: copy it to another SD card and it works there too.

To see the full format, ask Claude to show the plugin docs (the `plugins` tool with `docs`).

## For developers

*Settings > Developer options > Developer tools* adds these tools for Claude:

| Tool | What it does |
| --- | --- |
| `download_file` | Downloads a file (a new build, a big asset) straight to the SD card, with SHA-256 check |
| `run_app` | Launches a game or app the way OnionOS does, with a time limit, a CPU clock, words to watch for in the output, screenshots, and crash reports (signal, last log lines, last frame, core dump) |
| `input_record` / `input_play` | Records the buttons you press and replays them with exact timing, plus screenshots at set moments: the same test, the same way, every time |
| `perf_monitor` | CPU use, real CPU clock, memory, battery, saved as CSV |
| `screen_capture` | Screenshots every N ms, for a while |
| `watch_file` | Follows a log file and stops on the words you choose |
| `snapshot` | Saves a folder (like a save folder) and restores it later |
| `plugin_create` / `plugins` | Creates and manages [plugins](#plugins) |

## Languages

The app follows the language set in the Miyoo's menu. Translations live in `app/lang/<language>.txt` as pairs of lines (the English text, then the translation). Right now: English, Português (Brasil), Español. New translations are welcome: copy `pt-BR.txt`, translate the second line of each pair, and open a pull request. Latin, Greek and Cyrillic letters are supported.

## Updates

The app checks for a new version once each time the Miyoo is turned on (with Wi-Fi), and asks before installing it. You can also check in *Settings > Check for updates*. Your settings and connection are kept.

## How it works

The Miyoo cannot be reached directly from the internet, so the app keeps one secure connection open to a small relay (a Cloudflare Worker, in [`server/`](server)). Claude's connector talks to the same relay, which passes the requests to the Miyoo and the answers back. The relay keeps only what pairing needs (an ID and a hashed key for each device); it does not keep your files or messages.

## Building

The app is written in C, with no libraries besides the ones `build.sh` downloads (at fixed versions, checked).

1. Get the Miyoo Mini toolchain (for example the [union-miyoomini-toolchain](https://github.com/shauninman/union-miyoomini-toolchain) Docker image).
2. Inside it, run `sh build.sh`. The app folder is written to `package/App/ClaudeAgent/`.
3. `make_release.sh` builds and packs the release zip.

To use your own relay: `server/deploy.sh` publishes it on your Cloudflare account (the free plan is enough) and writes its address to `server.txt`; the next build uses that address.

## Credits

- [cJSON](https://github.com/DaveGamble/cJSON) (MIT)
- [stb_truetype and stb_image_write](https://github.com/nothings/stb) (public domain)
- [QR Code generator](https://github.com/nayuki/QR-Code-generator) by Project Nayuki (MIT)
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0)
- Certificate authorities from [certifi](https://github.com/certifi/python-certifi) (MPL-2.0)
- [workers-oauth-provider](https://github.com/cloudflare/workers-oauth-provider) by Cloudflare (MIT)
- [OnionOS](https://github.com/OnionUI/Onion), whose code showed how to launch apps, read the battery and the CPU clock

## License

MIT, see [LICENSE](LICENSE).
