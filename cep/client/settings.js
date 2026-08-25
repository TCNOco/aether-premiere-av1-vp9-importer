// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

'use strict';

function updateDecodeMode(text, mode) {
    if (!['auto', 'software', 'hardware'].includes(mode)) {
        throw new Error('invalid decode mode');
    }

    const newline = text.includes('\r\n') ? '\r\n' : '\n';
    const hadFinalNewline = /\r?\n$/.test(text);
    const lines = text ? text.split(/\r?\n/) : [];
    if (hadFinalNewline) lines.pop();

    const updated = [];
    let replaced = false;
    for (const line of lines) {
        if (/^\s*decode\s*=/i.test(line)) {
            if (!replaced) {
                updated.push('decode=' + mode);
                replaced = true;
            }
            continue;
        }
        updated.push(line);
    }

    if (!replaced) updated.push('decode=' + mode);
    return updated.join(newline) + newline;
}

module.exports = { updateDecodeMode };
