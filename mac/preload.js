'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('trafficAPI', {
  onStateUpdate: (callback) => {
    ipcRenderer.on('state-update', (_event, data) => callback(data));
  },
  toggleChaos: () => ipcRenderer.send('toggle-chaos'),
  toggleDisable: () => ipcRenderer.send('fake-disable'),
  closeApp: () => ipcRenderer.send('fake-close'),
  fakeDisable: () => ipcRenderer.send('fake-disable'),
  fakeClose: () => ipcRenderer.send('fake-close'),
  penaltyEnded: () => ipcRenderer.send('penalty-ended'),
  emergencyExit: () => ipcRenderer.send('emergency-exit'),
});
