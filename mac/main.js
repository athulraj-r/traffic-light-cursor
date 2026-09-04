'use strict';

const { app, BrowserWindow, ipcMain, globalShortcut, screen, dialog } = require('electron');
const path = require('path');
const fs = require('fs');
const { uIOhook, UiohookKey } = require('uiohook-napi');
const { mouse, Point } = require('@nut-tree-fork/nut-js');

// nut-js moves the mouse instantly by default — no easing/interpolation
mouse.config.mouseSpeed = 20000;

// ---------------------------------------------------------------------------
// TROLL POPUPS & CHAOS MESSAGES (Identical to Windows version)
// ---------------------------------------------------------------------------
const TROLL_POPUPS = [
  'Error 418: I am a coconut.',
  'Please wait while we do nothing...',
  'Task failed successfully.',
  'Your disable request has been submitted to the Department of Motor Vehicles.\nEstimated wait time: 4 to 6 weeks.',
  'Access Denied: Traffic rules are eternal.',
  'Are you sure you want to not close this application?',
  'Section 4(a) Violation: Attempting to bypass a traffic signal is a federal misdemeanor.',
  'Mouse cursor is currently in transit. Please pull over first.',
  'Nice try. The intersection remains under active surveillance.',
  'Error 404: Exit button not found.',
  'Movement permit denied by Regional Traffic Authority.',
  'Road work ahead. Yeah, I sure hope it does.',
];

const CHAOS_MESSAGES = [
  'Traffic authority lost control.',
  'All lanes open in all directions.',
  'Your cursor has been detained.',
  'Movement permit revoked.',
  'Gravity compromised.',
  'Speed limit: -45 px/sec.',
  'Opposite Day on Interstate 95.',
  'Roundabout detected: spin indefinitely.',
  'Pothole density critical.',
  'Nobody asked for this.',
];

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
let trafficState = 'RED'; // 'RED' | 'YELLOW' | 'GREEN'
let cursorControlEnabled = true;
let chaosMode = false;
let reverseControls = false;
let speedingPenalized = false;
let videoPlaying = false;
let cycleCount = 0;

let statusLine1 = 'RED';
let statusLine2 = 'CURSOR: STOPPED';
let statusColor = 'RED';

let frozenPosition = null;      // { x: number, y: number } — float
let lastPhysicalPosition = null; // { x: number, y: number } — integer
let pendingCorrections = 0;      // how many setPosition echoes we expect
let lastCommandedPos = null;     // { x: number, y: number } — integer (rounded)
let correctionInFlight = false;  // true while a correction is pending

let mainWindow = null;
let penaltyWindow = null;
let cycleTimer = null;
let chaosTimer = null;
let probationTimer = null;

const NORMAL_TIMING = { RED: 3000, GREEN: 4000, YELLOW: 2000 };
const YELLOW_MULTIPLIER = 0.15; // 15% of physical movement
let currentChaosYellowMultiplier = YELLOW_MULTIPLIER;

// Speed tracking variables: accumulate over 100ms window
let lastRawX = -1;
let lastRawY = -1;
let speedAccumDist = 0.0;
let speedWindowStart = Date.now();

// ---------------------------------------------------------------------------
// NATIVE MODULE
// ---------------------------------------------------------------------------
let nativeWarp = null;
try {
  nativeWarp = require('./native/warp.node');
  console.log('[traffic-light-cursor] High-performance native macOS CoreGraphics warp loaded.');
} catch (err) {
  console.warn('[traffic-light-cursor] native warp.node not available, falling back to nut-js:', err);
}

// ---------------------------------------------------------------------------
// SPEEDING VIOLATION DETECTION
// ---------------------------------------------------------------------------
function checkSpeedViolation(rawX, rawY) {
  if (speedingPenalized || videoPlaying || !cursorControlEnabled) return;

  if (lastRawX === -1) {
    lastRawX = rawX;
    lastRawY = rawY;
    speedAccumDist = 0.0;
    speedWindowStart = Date.now();
    return;
  }

  const dx = rawX - lastRawX;
  const dy = rawY - lastRawY;
  lastRawX = rawX;
  lastRawY = rawY;

  const stepDist = Math.hypot(dx, dy);
  speedAccumDist += stepDist;

  const now = Date.now();
  const elapsed = now - speedWindowStart;

  // Speed limits enforced during YELLOW and GREEN
  if (trafficState === 'YELLOW' || trafficState === 'GREEN') {
    if (elapsed >= 100) {
      const speedPxS = (speedAccumDist * 1000.0) / elapsed;
      speedAccumDist = 0.0;
      speedWindowStart = now;

      const limit = (trafficState === 'YELLOW') ? 700.0 : 1100.0;
      if (speedPxS > limit) {
        triggerSpeedingPenalty();
      }
    } else if (speedAccumDist > 120.0) {
      // Sudden fast swipe
      speedAccumDist = 0.0;
      speedWindowStart = now;
      triggerSpeedingPenalty();
    }
  } else {
    if (elapsed >= 100) {
      speedAccumDist = 0.0;
      speedWindowStart = now;
    }
  }
}

// ---------------------------------------------------------------------------
// SPEEDING PENALTY VIDEO PLAYER
// ---------------------------------------------------------------------------
function triggerSpeedingPenalty() {
  if (speedingPenalized || videoPlaying) return;
  speedingPenalized = true;
  videoPlaying = true;

  const prevState = trafficState;
  clearTimeout(cycleTimer);

  if (nativeWarp) {
    nativeWarp.setSpeedingPenalized(true);
  }

  statusLine1 = 'SPEEDING VIOLATION!';
  statusLine2 = 'MANDATORY TRAFFIC SCHOOL';
  statusColor = 'SPEEDING';
  broadcastState();

  const videoCandidates = [
    path.join(__dirname, 'video.mp4'),
    path.join(__dirname, '../windows/video.mp4'),
    path.join(__dirname, '../windows/build/video.mp4')
  ];
  const videoPath = videoCandidates.find(p => fs.existsSync(p));

  if (!videoPath) {
    // Missing video fallback detention
    if (mainWindow && !mainWindow.isDestroyed()) {
      dialog.showMessageBoxSync(mainWindow, {
        type: 'warning',
        title: 'Traffic Enforcement Citation',
        message: 'SPEEDING VIOLATION DETECTED!\n\nvideo.mp4 was not found in directory.\nSentence commuted to 5 seconds detention.',
        buttons: ['Accept Sentence']
      });
    }

    setTimeout(() => {
      endSpeedingPenalty(prevState);
    }, 5000);
    return;
  }

  createPenaltyWindow(prevState);
}

function createPenaltyWindow(prevState) {
  if (penaltyWindow && !penaltyWindow.isDestroyed()) {
    penaltyWindow.focus();
    return;
  }

  const primaryDisplay = screen.getPrimaryDisplay();
  const { width: sw, height: sh } = primaryDisplay.workAreaSize;
  const vw = 854;
  const vh = 512;
  const vx = Math.round((sw - vw) / 2);
  const vy = Math.round((sh - vh) / 2);

  penaltyWindow = new BrowserWindow({
    width: vw,
    height: vh,
    x: vx,
    y: vy,
    closable: false,
    minimizable: false,
    maximizable: false,
    resizable: false,
    alwaysOnTop: true,
    hasShadow: true,
    title: 'MANDATORY TRAFFIC SAFETY SCHOOL — SPEEDING PENALTY',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  penaltyWindow.setAlwaysOnTop(true, 'screen-saver');
  penaltyWindow.loadFile('penalty.html');

  // Prevent window close until video finishes
  penaltyWindow.on('close', (e) => {
    if (speedingPenalized && videoPlaying) {
      e.preventDefault();
    }
  });
}

function endSpeedingPenalty(restoreState = 'GREEN') {
  if (penaltyWindow && !penaltyWindow.isDestroyed()) {
    penaltyWindow.destroy();
    penaltyWindow = null;
  }

  speedAccumDist = 0.0;
  speedWindowStart = Date.now();
  speedingPenalized = false;
  videoPlaying = false;

  if (nativeWarp) {
    nativeWarp.setSpeedingPenalized(false);
  }

  // 2 seconds probation
  statusLine1 = 'PROBATION';
  statusLine2 = 'OBEY SPEED LIMIT';
  statusColor = 'PROBATION';
  broadcastState();

  clearTimeout(probationTimer);
  probationTimer = setTimeout(() => {
    setState(restoreState);
    scheduleNormalCycle();
  }, 2000);
}

// ---------------------------------------------------------------------------
// REVERSE GEAR
// ---------------------------------------------------------------------------
function startReverseGear(title = 'REVERSE GEAR!', subtitle = 'STEERING INVERTED (DRIVE IN REVERSE)') {
  if (reverseControls) return;
  reverseControls = true;

  if (nativeWarp) {
    nativeWarp.setReverseControls(true);
  }

  statusLine1 = title;
  statusLine2 = subtitle;
  statusColor = 'MAGENTA';
  broadcastState();
}

function stopReverseGear() {
  if (!reverseControls) return;
  reverseControls = false;

  if (nativeWarp) {
    nativeWarp.setReverseControls(false);
  }

  statusLine1 = 'GREEN';
  statusLine2 = 'CURSOR: GO';
  statusColor = 'GREEN';
  broadcastState();
}

// ---------------------------------------------------------------------------
// FALLBACK CURSOR CONTROL (nut-js)
// ---------------------------------------------------------------------------
function isEchoOfCommand(ex, ey) {
  if (!lastCommandedPos) return false;
  return Math.abs(ex - lastCommandedPos.x) <= 1 && Math.abs(ey - lastCommandedPos.y) <= 1;
}

function setCursorProgrammatically(x, y) {
  const rx = Math.round(x);
  const ry = Math.round(y);

  if (nativeWarp) {
    nativeWarp.warpMouse(rx, ry);
    return;
  }

  lastCommandedPos = { x: rx, y: ry };
  pendingCorrections++;
  correctionInFlight = true;

  mouse.setPosition(new Point(rx, ry)).catch((err) => {
    pendingCorrections = Math.max(0, pendingCorrections - 1);
    correctionInFlight = false;
  });
}

function onGlobalMouseMove(e) {
  if (!cursorControlEnabled) return;

  if (!nativeWarp && pendingCorrections > 0 && isEchoOfCommand(e.x, e.y)) {
    pendingCorrections--;
    if (pendingCorrections === 0) {
      correctionInFlight = false;
    }
    lastPhysicalPosition = { x: e.x, y: e.y };
    return;
  }

  if (!lastPhysicalPosition) {
    lastPhysicalPosition = { x: e.x, y: e.y };
    frozenPosition = { x: e.x, y: e.y };
    return;
  }

  const dx = e.x - lastPhysicalPosition.x;
  const dy = e.y - lastPhysicalPosition.y;
  lastPhysicalPosition = { x: e.x, y: e.y };

  if (dx === 0 && dy === 0) return;

  // 1. Immobilized during speeding penalty or RED
  if (speedingPenalized || trafficState === 'RED') {
    setCursorProgrammatically(frozenPosition.x, frozenPosition.y);
    return;
  }

  // 2. Reverse gear
  if (reverseControls) {
    frozenPosition = {
      x: frozenPosition.x - dx,
      y: frozenPosition.y - dy,
    };
    setCursorProgrammatically(frozenPosition.x, frozenPosition.y);
    return;
  }

  // 3. YELLOW light
  if (trafficState === 'YELLOW') {
    const mult = chaosMode ? currentChaosYellowMultiplier : YELLOW_MULTIPLIER;
    let stepX = dx * mult;
    let stepY = dy * mult;

    if (chaosMode && Math.random() < 0.16) {
      stepX += (Math.floor(Math.random() * 5) - 2);
      stepY += (Math.floor(Math.random() * 5) - 2);
    }

    frozenPosition = {
      x: frozenPosition.x + stepX,
      y: frozenPosition.y + stepY,
    };
    setCursorProgrammatically(frozenPosition.x, frozenPosition.y);
    return;
  }

  // 4. GREEN light
  if (chaosMode && Math.random() < 0.12) {
    const jx = Math.floor(Math.random() * 9) - 4;
    const jy = Math.floor(Math.random() * 9) - 4;
    frozenPosition = { x: e.x + jx, y: e.y + jy };
    setCursorProgrammatically(frozenPosition.x, frozenPosition.y);
    return;
  }

  frozenPosition = { x: e.x, y: e.y };
}

// ---------------------------------------------------------------------------
// BROADCAST & STATE UPDATES
// ---------------------------------------------------------------------------
function broadcastState(extra = {}) {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  mainWindow.webContents.send('state-update', {
    state: trafficState,
    cursorControlEnabled,
    chaosMode,
    reverseControls,
    speedingPenalized,
    statusLine1,
    statusLine2,
    colorClass: statusColor,
    ...extra,
  });
}

function setState(newState, line1, line2) {
  const prevState = trafficState;
  trafficState = newState;

  if (line1) statusLine1 = line1;
  else statusLine1 = newState;

  if (line2) statusLine2 = line2;
  else {
    if (newState === 'RED') statusLine2 = 'CURSOR: STOPPED';
    else if (newState === 'YELLOW') statusLine2 = 'CURSOR: SLOW';
    else if (newState === 'GREEN') statusLine2 = 'CURSOR: GO';
  }

  statusColor = newState;

  if (nativeWarp) {
    nativeWarp.setState(newState);
  }

  if (prevState !== newState) {
    const pos = nativeWarp ? nativeWarp.getCursorPosition() : screen.getCursorScreenPoint();
    frozenPosition = { x: pos.x, y: pos.y };
    lastPhysicalPosition = { x: pos.x, y: pos.y };
    pendingCorrections = 0;
    correctionInFlight = false;
    lastCommandedPos = null;
  }

  broadcastState();
}

// ---------------------------------------------------------------------------
// TRAFFIC CYCLES
// ---------------------------------------------------------------------------
function scheduleNormalCycle() {
  clearTimeout(cycleTimer);
  if (!cursorControlEnabled || chaosMode || reverseControls || speedingPenalized) return;

  if (trafficState === 'RED') {
    cycleTimer = setTimeout(() => {
      setState('GREEN');
      scheduleNormalCycle();
    }, NORMAL_TIMING.RED);
  } else if (trafficState === 'GREEN') {
    cycleTimer = setTimeout(() => {
      setState('YELLOW');
      scheduleNormalCycle();
    }, NORMAL_TIMING.GREEN);
  } else if (trafficState === 'YELLOW') {
    cycleTimer = setTimeout(() => {
      cycleCount++;
      // Every other cycle: Reverse Gear for 6 seconds!
      // Red light NEVER occurs during reverse gear!
      if (cycleCount % 2 === 0) {
        startReverseGear('REVERSE GEAR!', 'STEERING INVERTED (DRIVE IN REVERSE)');
        cycleTimer = setTimeout(() => {
          stopReverseGear();
          setState('RED');
          scheduleNormalCycle();
        }, 6000);
      } else {
        setState('RED');
        scheduleNormalCycle();
      }
    }, NORMAL_TIMING.YELLOW);
  }
}

function scheduleChaosCycle() {
  clearTimeout(cycleTimer);
  if (!chaosMode || !cursorControlEnabled || speedingPenalized) return;

  const states = ['RED', 'YELLOW', 'GREEN'];
  const next = states[Math.floor(Math.random() * states.length)];
  const chaosMsg = CHAOS_MESSAGES[Math.floor(Math.random() * CHAOS_MESSAGES.length)];

  // 40% chance to toggle reverse controls in chaos
  if (Math.random() < 0.40) {
    if (reverseControls) {
      stopReverseGear();
    } else {
      startReverseGear('REVERSED', chaosMsg);
    }
  }

  if (!reverseControls) {
    setState(next, next, chaosMsg);
  } else {
    broadcastState();
  }

  if (next === 'YELLOW') {
    currentChaosYellowMultiplier = 0.02 + Math.random() * 0.09;
    if (nativeWarp) nativeWarp.setMultiplier(currentChaosYellowMultiplier);
  }

  // Rapid cycle duration: 0.15s - 0.85s (150ms to 850ms)
  const duration = 150 + Math.floor(Math.random() * 700);

  cycleTimer = setTimeout(() => {
    scheduleChaosCycle();
  }, duration);
}

function enableChaosMode() {
  chaosMode = true;
  if (nativeWarp) nativeWarp.setChaos(true);

  scheduleChaosCycle();
  broadcastState({ message: 'CHAOS MODE ENGAGED' });

  clearTimeout(chaosTimer);
  chaosTimer = setTimeout(() => {
    disableChaosMode();
  }, 15000); // 15 seconds auto-end
}

function disableChaosMode() {
  chaosMode = false;
  if (reverseControls) stopReverseGear();
  if (nativeWarp) {
    nativeWarp.setChaos(false);
    nativeWarp.setMultiplier(YELLOW_MULTIPLIER);
  }
  clearTimeout(chaosTimer);
  currentChaosYellowMultiplier = YELLOW_MULTIPLIER;
  broadcastState({ message: 'Order restored.' });
  setState('RED');
  scheduleNormalCycle();
}

// ---------------------------------------------------------------------------
// WINDOW
// ---------------------------------------------------------------------------
function createWindow() {
  const { workArea } = screen.getPrimaryDisplay();

  mainWindow = new BrowserWindow({
    width: 320,
    height: 560,
    x: workArea.x + workArea.width - 340,
    y: workArea.y + 20,
    frame: false,
    resizable: false,
    alwaysOnTop: true,
    skipTaskbar: false,
    hasShadow: true,
    backgroundColor: '#00000000',
    transparent: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  mainWindow.setAlwaysOnTop(true, 'screen-saver');
  mainWindow.loadFile('index.html');

  mainWindow.webContents.once('did-finish-load', () => {
    broadcastState();
  });
}

// ---------------------------------------------------------------------------
// EMERGENCY RESTORE & EXIT
// ---------------------------------------------------------------------------
function emergencyRestoreCursor() {
  cursorControlEnabled = false;
  if (nativeWarp) {
    try {
      nativeWarp.setEnabled(false);
      nativeWarp.stopHook();
    } catch (_) {}
  }
  try {
    uIOhook.stop();
  } catch (_) {}
  if (penaltyWindow && !penaltyWindow.isDestroyed()) {
    penaltyWindow.destroy();
    penaltyWindow = null;
  }
}

// ---------------------------------------------------------------------------
// IPC HANDLERS
// ---------------------------------------------------------------------------
ipcMain.on('toggle-chaos', () => {
  if (chaosMode) disableChaosMode();
  else enableChaosMode();
});

// FAKE DISABLE BUTTON: Triggers random troll popups
ipcMain.on('fake-disable', () => {
  const idx = Math.floor(Math.random() * TROLL_POPUPS.length);
  const trollMsg = TROLL_POPUPS[idx];
  dialog.showMessageBoxSync(mainWindow, {
    type: 'warning',
    title: 'Traffic Authority Alert',
    message: trollMsg,
    buttons: ['OK']
  });
});

// FAKE CLOSE BUTTON: Triggers random troll popups
ipcMain.on('fake-close', () => {
  const idx = Math.floor(Math.random() * TROLL_POPUPS.length);
  const trollMsg = TROLL_POPUPS[idx];
  dialog.showMessageBoxSync(mainWindow, {
    type: 'error',
    title: 'Error 418: I am a coconut',
    message: trollMsg,
    buttons: ['Dismiss']
  });
});

ipcMain.on('penalty-ended', () => {
  endSpeedingPenalty();
});

ipcMain.on('emergency-exit', () => {
  emergencyRestoreCursor();
  app.quit();
});

// ---------------------------------------------------------------------------
// APP LIFECYCLE & HOTKEYS
// ---------------------------------------------------------------------------
app.whenReady().then(() => {
  createWindow();

  if (nativeWarp) {
    nativeWarp.setMultiplier(YELLOW_MULTIPLIER);
    nativeWarp.setState(trafficState);
    nativeWarp.setEnabled(cursorControlEnabled);
    nativeWarp.startHook();
    console.log('[traffic-light-cursor] Native hardware-delta event tap active.');
  }

  // Global mouse & keyboard tracking via uIOhook
  uIOhook.on('mousemove', (e) => {
    checkSpeedViolation(e.x, e.y);
    if (!nativeWarp) {
      onGlobalMouseMove(e);
    }
  });

  uIOhook.on('keydown', (e) => {
    if (e.keycode === UiohookKey.Escape) {
      // ESC: Emergency Exit
      emergencyRestoreCursor();
      app.quit();
    } else if (e.keycode === UiohookKey.S) {
      // S: Force speeding penalty
      triggerSpeedingPenalty();
    } else if (e.keycode === UiohookKey.R) {
      // R: Toggle reverse gear
      if (reverseControls) {
        stopReverseGear();
      } else {
        startReverseGear('REVERSE GEAR!', 'STEERING INVERTED (KEY R)');
      }
    }
  });

  try {
    uIOhook.start();
  } catch (err) {
    console.warn('[traffic-light-cursor] uIOhook start warning:', err);
  }

  // Global ESC shortcut registered via Electron as a failsafe
  globalShortcut.register('Escape', () => {
    emergencyRestoreCursor();
    app.quit();
  });

  scheduleNormalCycle();
});

app.on('window-all-closed', () => {
  emergencyRestoreCursor();
  app.quit();
});

app.on('before-quit', emergencyRestoreCursor);
app.on('will-quit', () => {
  globalShortcut.unregisterAll();
});

process.on('uncaughtException', (err) => {
  console.error('[traffic-light-cursor] Uncaught exception, restoring cursor:', err);
  emergencyRestoreCursor();
});
