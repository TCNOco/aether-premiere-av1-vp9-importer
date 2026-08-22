// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 neoHaDe
//
// Панель Aether внутри Premiere.
//
// Логики тут нарочно мало. Всё, что можно решить один раз и в одном месте,
// решено в C++: и настройки, и проверки читает та же программа, которой
// пользуется окно Aether.exe. Панель показывает её ответ и записывает выбор —
// не более. Дублировать проверки на JavaScript значило бы завести вторую
// правду, которая рано или поздно разойдётся с первой.

'use strict';

const fs   = require('fs');
const path = require('path');
const os   = require('os');
const { execFile, execFileSync } = require('child_process');

// ---------------------------------------------------------------------------
// Где живёт плагин
// ---------------------------------------------------------------------------

// Папку плагинов приложения Adobe сами записывают в реестр — оттуда её берёт
// и установщик, и диагностика. Спрашиваем тем же способом, чтобы три места
// не разошлись во мнениях.
function mediaCoreFromRegistry() {
    const keys = [
        'HKLM\\SOFTWARE\\Adobe\\Premiere Pro\\CurrentVersion',
        'HKLM\\SOFTWARE\\Adobe\\After Effects\\CurrentVersion',
    ];
    for (const key of keys) {
        try {
            const out = execFileSync(
                'reg', ['query', key, '/v', 'CommonPluginInstallPath'],
                { encoding: 'utf8', windowsHide: true });
            const m = out.match(/CommonPluginInstallPath\s+REG_\w+\s+(.+)/);
            if (m) return m[1].trim();
        } catch (e) { /* ключа нет — пробуем следующий */ }
    }
    return null;
}

function pluginFolder() {
    const candidates = [];
    const reg = mediaCoreFromRegistry();
    if (reg) candidates.push(path.join(reg, 'Aether'));

    const pf = process.env['ProgramFiles'] || 'C:\\Program Files';
    candidates.push(path.join(pf, 'Adobe', 'Common', 'Plug-ins', '7.0', 'MediaCore', 'Aether'));

    for (const dir of candidates) {
        if (fs.existsSync(path.join(dir, 'AetherDiagnose.exe'))) return dir;
    }
    return null;
}

const PLUGIN_DIR   = pluginFolder();
const SETTINGS_DIR = path.join(process.env['LOCALAPPDATA'] || os.homedir(), 'Aether');
const SETTINGS_INI = path.join(SETTINGS_DIR, 'settings.ini');

// ---------------------------------------------------------------------------
// Цвета из самого Premiere
// ---------------------------------------------------------------------------

// У Premiere четыре ступени яркости интерфейса, и панель, покрашенная в одну
// из них, при любой другой выглядит заплаткой. Спрашиваем, какая стоит сейчас.
function applyHostTheme() {
    let skin;
    try {
        skin = JSON.parse(window.__adobe_cep__.getHostEnvironment()).appSkinInfo;
    } catch (e) {
        return;   // не спросилось — остаёмся при своих, они тёмные
    }

    const rgb = (c) => `rgb(${Math.round(c.red)},${Math.round(c.green)},${Math.round(c.blue)})`;
    const bg  = skin.panelBackgroundColor.color;

    // Светлая тема Premiere или тёмная — решаем по самой подложке, а не по
    // названию: названий у ступеней нет, а число есть.
    const light = (bg.red + bg.green + bg.blue) / 3 > 127;

    const css = document.documentElement.style;
    css.setProperty('--bg', rgb(bg));
    css.setProperty('--panel', light ? 'rgba(0,0,0,.04)' : 'rgba(255,255,255,.04)');
    css.setProperty('--line',  light ? 'rgba(0,0,0,.18)' : 'rgba(255,255,255,.14)');
    css.setProperty('--text',  light ? '#1a1a1a' : '#f0f0f0');
    css.setProperty('--note',  light ? '#5b5b5b' : '#a6a6a6');
    css.setProperty('--accent', light ? '#0067c0' : '#4cc2ff');
    css.setProperty('--onaccent', light ? '#ffffff' : '#001a2b');
    css.setProperty('--good', light ? '#107c10' : '#6ccb5f');
    css.setProperty('--bad',  light ? '#c42b1c' : '#ff99a4');
    css.setProperty('--warn', light ? '#9d5d00' : '#fce100');
}

// ---------------------------------------------------------------------------
// Настройки
// ---------------------------------------------------------------------------

// Файл человекочитаемый и правится руками по совету из issue, поэтому читаем
// его терпимо: неизвестные строки пропускаем, а не считаем поломкой.
function readMode() {
    try {
        const text = fs.readFileSync(SETTINGS_INI, 'utf8');
        const m = text.match(/^\s*decode\s*=\s*(\w+)/mi);
        if (m) {
            const v = m[1].toLowerCase();
            if (v === 'software' || v === 'hardware') return v;
        }
    } catch (e) { /* файла нет — значит «автоматически» */ }
    return 'auto';
}

function writeMode(mode) {
    fs.mkdirSync(SETTINGS_DIR, { recursive: true });
    fs.writeFileSync(SETTINGS_INI,
        '; AV1 / VP9 Importer for Premiere Pro\r\n' +
        '; decode = auto | software | hardware\r\n' +
        'decode=' + mode + '\r\n',
        'utf8');
}

// ---------------------------------------------------------------------------
// Диагностика
// ---------------------------------------------------------------------------

let lastReport = null;
let userFile   = '';

function runDiagnostics() {
    const results = document.getElementById('results');
    const status  = document.getElementById('status');
    const run     = document.getElementById('run');
    const copy    = document.getElementById('copy');

    if (!PLUGIN_DIR) {
        results.innerHTML = '';
        status.textContent = 'Не нашлась папка плагина. Похоже, Aether не установлен.';
        return;
    }

    run.disabled  = true;
    copy.disabled = true;
    status.textContent = 'Проверяем...';

    const exe  = path.join(PLUGIN_DIR, 'AetherDiagnose.exe');
    const args = userFile ? ['--json', userFile] : ['--json'];

    // Буфер побольше умолчания: отчёт с длинными путями в него не влезал бы,
    // а обрезанный JSON разобрать нельзя вовсе.
    execFile(exe, args, { encoding: 'utf8', maxBuffer: 4 * 1024 * 1024, windowsHide: true },
        (err, stdout) => {
            run.disabled = false;

            // Ненулевой код возврата — это «есть сбои», а не «не запустилось».
            // Отличаем по тому, разобрался ли ответ.
            let report = null;
            try { report = JSON.parse(stdout); } catch (e) { /* ниже */ }

            if (!report) {
                results.innerHTML = '';
                status.textContent = 'Проверка не отработала: ' + (err ? err.message : 'пустой ответ');
                return;
            }

            lastReport = report;
            render(report);
            copy.disabled = false;
            status.textContent = report.failed
                ? 'Есть сбои — отмечены красным. Приложите отчёт к issue.'
                : 'Всё в порядке. Это не обещает, что Premiere откроет файл, — он ходит своим путём.';
        });
}

function render(report) {
    const results = document.getElementById('results');
    results.innerHTML = '';

    for (const section of report.sections) {
        const head = document.createElement('div');
        head.className = 'group';
        head.textContent = section.title;
        results.appendChild(head);

        for (const check of section.checks) {
            const row = document.createElement('div');
            row.className = 'row';

            const mark = document.createElement('span');
            mark.className = 'mark ' + check.state;
            mark.textContent = { pass: '\u2713', fail: '\u2715',
                                 warn: '!', skip: '\u2014' }[check.state] || '';

            const body = document.createElement('span');
            const what = document.createElement('div');
            what.className = 'what';
            what.textContent = check.name;
            body.appendChild(what);

            if (check.detail) {
                const detail = document.createElement('div');
                detail.className = 'detail';
                detail.textContent = check.detail;
                body.appendChild(detail);
            }

            row.appendChild(mark);
            row.appendChild(body);
            results.appendChild(row);
        }
    }
}

// Текст для issue собирается здесь, но чистить его уже не нужно: имена
// пользователя и машины движок убрал ещё до того, как отдал нам JSON.
function reportText(report) {
    const mark = { pass: 'OK  ', fail: 'СБОЙ', warn: '!   ', skip: '—   ', info: '    ' };
    let out = 'Отчёт диагностики Aether\nВерсия: ' + report.version + '\n\n';
    for (const section of report.sections) {
        out += section.title + '\n';
        for (const c of section.checks) {
            out += '  ' + (mark[c.state] || '') + '  ' + c.name +
                   (c.detail ? ': ' + c.detail : '') + '\n';
        }
        out += '\n';
    }
    out += 'Проверено ядро плагина на этой машине. Это не доказывает, что\n' +
           'Premiere откроет файл: он ходит к плагину своим путём.\n';
    return out;
}

// ---------------------------------------------------------------------------
// Связывание
// ---------------------------------------------------------------------------

function setFile(file) {
    userFile = file || '';
    const text   = document.getElementById('drop-text');
    const drop   = document.getElementById('drop');
    const forget = document.getElementById('forget');

    if (userFile) {
        text.textContent = path.basename(userFile);
        text.title = userFile;
        drop.classList.add('has');
        forget.hidden = false;
    } else {
        text.textContent = 'Перетащите сюда файл, который не открывается';
        text.title = '';
        drop.classList.remove('has');
        forget.hidden = true;
    }
}

document.addEventListener('DOMContentLoaded', () => {
    applyHostTheme();

    // Версия берётся у самого плагина, а не пишется в панели: разъехаться
    // им тогда негде, а разъехавшиеся версии мы уже проходили.
    const version = document.getElementById('version');
    if (PLUGIN_DIR) {
        try {
            const out = execFileSync('powershell', ['-NoProfile', '-Command',
                `(Get-Item '${path.join(PLUGIN_DIR, 'Aether.prm')}').VersionInfo.ProductVersion`],
                { encoding: 'utf8', windowsHide: true }).trim();
            if (out) version.textContent = 'версия ' + out;
        } catch (e) { /* оставим как есть */ }
    } else {
        version.textContent = 'плагин не найден';
    }

    // Вкладки
    const pages = { settings: 'page-settings', diagnose: 'page-diagnose' };
    for (const name of Object.keys(pages)) {
        document.getElementById('tab-' + name).addEventListener('click', () => {
            for (const other of Object.keys(pages)) {
                document.getElementById('tab-' + other).classList.toggle('active', other === name);
                document.getElementById(pages[other]).hidden = (other !== name);
            }
        });
    }

    // Настройки
    const mode = readMode();
    const radio = document.querySelector(`input[name="decode"][value="${mode}"]`);
    if (radio) radio.checked = true;

    document.getElementById('save').addEventListener('click', () => {
        const chosen = document.querySelector('input[name="decode"]:checked').value;
        const saved  = document.getElementById('saved');
        try {
            writeMode(chosen);
            saved.textContent = 'Сохранено. Перезапустите Premiere Pro.';
        } catch (e) {
            saved.textContent = 'Не удалось записать: ' + e.message;
        }
    });

    // Файл человека
    const drop = document.getElementById('drop');
    drop.addEventListener('dragover', (e) => {
        e.preventDefault();
        drop.classList.add('over');
    });
    drop.addEventListener('dragleave', () => drop.classList.remove('over'));
    drop.addEventListener('drop', (e) => {
        e.preventDefault();
        drop.classList.remove('over');
        if (e.dataTransfer.files.length) setFile(e.dataTransfer.files[0].path);
    });
    document.getElementById('forget').addEventListener('click', () => setFile(''));

    document.getElementById('run').addEventListener('click', runDiagnostics);

    document.getElementById('copy').addEventListener('click', () => {
        if (!lastReport) return;
        const area = document.createElement('textarea');
        area.value = reportText(lastReport);
        document.body.appendChild(area);
        area.select();
        document.execCommand('copy');
        document.body.removeChild(area);
        document.getElementById('status').textContent =
            'Отчёт скопирован — вставьте его в issue на GitHub.';
    });
});
