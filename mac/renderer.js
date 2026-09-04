'use strict';

const lightRed = document.getElementById('lightRed');
const lightYellow = document.getElementById('lightYellow');
const lightGreen = document.getElementById('lightGreen');
const statusState = document.getElementById('statusState');
const statusCursor = document.getElementById('statusCursor');
const messageEl = document.getElementById('message');
const chaosBtn = document.getElementById('chaosBtn');
const disableBtn = document.getElementById('disableBtn');
const closeBtn = document.getElementById('closeBtn');

const FAKE_LABELS = ['DENIED', 'NICE TRY', 'IMPOSSIBLE', 'NO WAY', 'HA HA', 'NEVER'];

const CURSOR_LABEL = {
  RED: 'CURSOR: STOPPED',
  YELLOW: 'CURSOR: SLOW',
  GREEN: 'CURSOR: GO',
};

let messageTimer = null;
let trollResetTimer = null;

function render({
  state,
  cursorControlEnabled,
  chaosMode,
  reverseControls,
  speedingPenalized,
  statusLine1,
  statusLine2,
  colorClass,
  message
}) {
  // Clear all lamp states
  [lightRed, lightYellow, lightGreen].forEach((el) => {
    el.classList.remove('active');
    el.classList.remove('magenta');
  });

  if (reverseControls) {
    // Magenta bottom lamp, top and middle dark
    lightGreen.classList.add('active', 'magenta');
    statusState.textContent = `\u25CF ${statusLine1 || 'REVERSE GEAR!'}`;
    statusState.className = 'status-state MAGENTA';
    statusCursor.textContent = statusLine2 || 'STEERING INVERTED';
  } else if (speedingPenalized) {
    lightRed.classList.add('active');
    statusState.textContent = `\u25CF ${statusLine1 || 'SPEEDING VIOLATION!'}`;
    statusState.className = 'status-state SPEEDING';
    statusCursor.textContent = statusLine2 || 'MANDATORY TRAFFIC SCHOOL';
  } else {
    if (state === 'RED') lightRed.classList.add('active');
    if (state === 'YELLOW') lightYellow.classList.add('active');
    if (state === 'GREEN') lightGreen.classList.add('active');

    const cls = colorClass || state;
    statusState.textContent = `\u25CF ${statusLine1 || state}`;
    statusState.className = `status-state ${cls}`;

    statusCursor.textContent = statusLine2 || (CURSOR_LABEL[state] || 'CURSOR: ACTIVE');
  }

  chaosBtn.classList.toggle('active', !!chaosMode);
  chaosBtn.textContent = chaosMode ? 'EXIT CHAOS' : 'CHAOS MODE';

  if (message) {
    messageEl.textContent = message;
    messageEl.classList.add('show');
    clearTimeout(messageTimer);
    messageTimer = setTimeout(() => messageEl.classList.remove('show'), 2200);
  }
}

window.trafficAPI.onStateUpdate(render);

chaosBtn.addEventListener('click', () => window.trafficAPI.toggleChaos());

disableBtn.addEventListener('click', () => {
  // Fake button troll behavior
  const fakeText = FAKE_LABELS[Math.floor(Math.random() * FAKE_LABELS.length)];
  disableBtn.textContent = fakeText;
  disableBtn.classList.add('trolled');

  // Trigger troll popup via main process
  window.trafficAPI.toggleDisable();

  clearTimeout(trollResetTimer);
  trollResetTimer = setTimeout(() => {
    disableBtn.textContent = 'DISABLE';
    disableBtn.classList.remove('trolled');
  }, 1200);
});

closeBtn.addEventListener('click', () => {
  window.trafficAPI.closeApp();
});
