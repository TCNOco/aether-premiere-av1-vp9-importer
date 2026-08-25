// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { updateDecodeMode } = require('../cep/client/settings');

assert.strictEqual(updateDecodeMode('', 'auto'), 'decode=auto\n');

assert.strictEqual(
    updateDecodeMode(
        '; settings\r\ndecode=auto\r\nasync=off\r\nyuv=off\r\n',
        'hardware'),
    '; settings\r\ndecode=hardware\r\nasync=off\r\nyuv=off\r\n');

assert.strictEqual(
    updateDecodeMode('async=off\nyuv=off', 'software'),
    'async=off\nyuv=off\ndecode=software\n');

assert.strictEqual(
    updateDecodeMode('decode=auto\nasync=off\ndecode=software\nyuv=off\n',
                     'hardware'),
    'decode=hardware\nasync=off\nyuv=off\n');

assert.strictEqual(
    updateDecodeMode('decoder=custom\ndecode_mode=manual\n', 'auto'),
    'decoder=custom\ndecode_mode=manual\ndecode=auto\n');

assert.throws(() => updateDecodeMode('', 'surprise'), /invalid decode mode/);

const client = path.join(__dirname, '..', 'cep', 'client');
const html = fs.readFileSync(path.join(client, 'index.html'), 'utf8');
const main = fs.readFileSync(path.join(client, 'main.js'), 'utf8');
const css = fs.readFileSync(path.join(client, 'style.css'), 'utf8');

assert.match(html, /role="tablist"/);
assert.match(html, /role="tabpanel"/);
assert.match(html, /aria-live="polite"/);
assert.match(html, /id="file-picker"/);
assert.match(main, /ArrowRight/);
assert.doesNotMatch(main, /execFileSync/);
assert.match(css, /:focus-visible/);
assert.match(css, /\.results[\s\S]*user-select:\s*text/);

console.log('panel settings checks: OK');
