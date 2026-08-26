// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const client = path.join(__dirname, '..', 'cep', 'client');
const html = fs.readFileSync(path.join(client, 'index.html'), 'utf8');
const main = fs.readFileSync(path.join(client, 'main.js'), 'utf8');
const css = fs.readFileSync(path.join(client, 'style.css'), 'utf8');

assert.match(html, /Content-Security-Policy/);
assert.match(html, /script-src 'self'/);
assert.match(html, /role="tablist"/);
assert.match(html, /role="tabpanel"/);
assert.match(html, /id="tab-cache"/);
assert.match(html, /id="memory-cache"/);
assert.match(html, /id="preview-cache"/);
assert.match(html, /id="clear-cache"/);
assert.match(html, /aria-live="polite"/);
assert.match(html, /id="file-picker"/);
assert.match(main, /ArrowRight/);
assert.doesNotMatch(main, /execFileSync/);
assert.match(main, /function system32/);
assert.match(main, /system32\('reg\.exe'\)/);
assert.match(main, /WindowsPowerShell/);
assert.match(main, /--settings-json/);
assert.match(main, /--cache-json/);
assert.match(main, /--clear-preview-cache/);
assert.match(main, /--set/);
assert.doesNotMatch(main, /readFileSync\(\s*SETTINGS_INI/);
assert.doesNotMatch(main, /writeFileAtomic/);
assert.match(css, /:focus-visible/);
assert.match(css, /\.results[\s\S]*user-select:\s*text/);

console.log('panel settings checks: OK');
