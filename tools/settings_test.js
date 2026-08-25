// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

'use strict';

const assert = require('assert');
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

console.log('panel settings checks: OK');
