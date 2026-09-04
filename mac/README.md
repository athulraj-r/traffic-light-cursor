# Traffic Light Cursor (macOS)

An intentionally useless desktop app: a small traffic light that has somehow
gained authority over your **real, physical OS mouse cursor**.

- 🔴 **RED** — cursor is physically frozen (cannot move at all)
- 🟡 **YELLOW** — cursor movement slowed to a crawl (plus drift & erratic jitter)
- 🟢 **GREEN** — full natural cursor speed (unless Reverse Gear strikes)
- 🚨 **SPEEDING PENALTY** — moving cursor too fast freezes the mouse completely and launches an unclosable mandatory traffic safety video (`video.mp4`) that only closes when finished
- 🔄 **REVERSE GEAR** — random event inverting all mouse axes (up is down, left is right)
- 🌀 **HYPER-CHAOS MODE** — erratic fast flickers, micro-jitters, wildly swinging speed multipliers, and frequent control reversals
- 🤡 **FAKE BUTTONS** — close button ("X") and DISABLE button are completely fake and throw ragebait troll popups ("Error 418: I am a coconut", "Please wait while we do nothing", etc.)

## How it works

- A native macOS CoreGraphics event tap (`warp.c` → `warp.node`) intercepts
  mouse movement **inside the OS input pipeline** before the cursor moves,
  enabling true freeze (RED), true fractional slowdown (YELLOW), axis inversion
  (REVERSE GEAR), and complete immobilization (SPEEDING PENALTY).
- [`uiohook-napi`](https://www.npmjs.com/package/uiohook-napi) provides global
  keyboard hook for hotkeys (`ESC`, `S`, `R`) and mouse velocity tracking.
- Electron hosts the traffic light UI and orchestrates the cycle/chaos/penalty logic.

## Requirements

- Node.js 18+
- macOS (primary target)
- `video.mp4` placed in the `mac/` folder for the speeding penalty video

## Install

```bash
cd mac
npm install
npm run build:native   # compile the CoreGraphics warp module
npm start
```

## macOS permissions (required)

macOS blocks both global mouse listening and programmatic cursor control
unless you grant Accessibility (and on newer macOS, Input Monitoring)
permission to the app that's running Electron (during development this is
usually your terminal, or "Electron" itself the first time you run `npm
start`).

1. Run `npm start` once — macOS will prompt you, or the hook will silently do
   nothing if it isn't authorized yet.
2. Go to **System Settings → Privacy & Security → Accessibility** and enable
   your terminal app (Terminal/iTerm) or "Electron".
3. Go to **System Settings → Privacy & Security → Input Monitoring** and do
   the same.
4. Restart the app (`npm start` again).

Without these permissions the traffic light will still animate, but it won't
actually be able to see or move your cursor.

## Controls

| Control | Action |
|---|---|
| **DISABLE** button | Fake! Triggers random troll popups |
| **Close ("X")** button | Fake! Triggers random troll popups |
| **CHAOS MODE** button | Unleashes rapid flickering, reverse controls, and jitters |
| `S` key (Global) | **Force Speeding Violation** (plays video immediately) |
| `R` key (Global) | **Toggle Reverse Gear** (inverts mouse steering) |
| `ESC` key (Global) | **EMERGENCY EXIT** — The only way to stop the program and restore system cursor |

## Project structure

```
mac/
├── package.json      dependencies + start script
├── main.js           Electron main process: mouse hook, cursor math, cycle/chaos/penalty logic, safety shutdown
├── preload.js        contextBridge IPC surface
├── index.html        traffic light window markup
├── styles.css        dark, glowing traffic-light styling
├── renderer.js       wires IPC state → UI
├── penalty.html      unclosable speeding penalty video player
├── video.mp4         mandatory traffic safety school video
├── native/
│   ├── warp.c        native macOS CoreGraphics event tap + N-API bindings
│   ├── warp.node     compiled native module
│   └── build.js      build script for warp.c
└── README.md
```
