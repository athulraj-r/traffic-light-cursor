const { execSync } = require('child_process');
const path = require('path');
const fs = require('fs');

const candidates = [
  path.resolve(process.execPath, '../../include/node'),
  '/opt/homebrew/include/node',
  '/usr/local/include/node',
  path.resolve(process.env.HOME || '', '.cache/node-gyp', process.versions.node, 'include/node')
];

let includeDir = candidates.find(dir => fs.existsSync(path.join(dir, 'node_api.h')));

if (!includeDir) {
  console.warn('[build] Could not find node_api.h automatically in standard locations.');
  includeDir = candidates[0];
}

console.log(`[build] Using Node include directory: ${includeDir}`);
const outDir = path.resolve(__dirname);
const outFile = path.join(outDir, 'warp.node');
const srcFile = path.join(outDir, 'warp.c');

const cmd = `clang -O3 -bundle -flat_namespace -undefined suppress -I"${includeDir}" -framework ApplicationServices -framework CoreGraphics "${srcFile}" -o "${outFile}"`;
console.log(`[build] Compiling: ${cmd}`);
execSync(cmd, { stdio: 'inherit' });
console.log('[build] Native module warp.node successfully built!');
