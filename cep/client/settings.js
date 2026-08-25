// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe

'use strict';

const fs = require('fs');

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

// Пишем рядом, потом подменяем. На Windows rename не перезаписывает
// существующий файл, поэтому запасной путь — copy поверх + удалить tmp.
// Так же делает C++ SaveMode: оборванная запись не должна оставить
// наполовину пустой settings.ini.
function writeFileAtomic(filePath, data, encoding) {
    const tmp = filePath + '.tmp';
    fs.writeFileSync(tmp, data, encoding);
    try {
        fs.renameSync(tmp, filePath);
    } catch (error) {
        try {
            fs.copyFileSync(tmp, filePath);
        } finally {
            try { fs.unlinkSync(tmp); } catch (_) { /* leftover tmp is harmless */ }
        }
    }
}

module.exports = { updateDecodeMode, writeFileAtomic };
