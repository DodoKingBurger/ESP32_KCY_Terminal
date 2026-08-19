/* ============================================================
   ESP32 Terminal + Archive Download
   Терминал — как в оригинале:
     windows-1251, фильтр байтов, parseLampState / parseTerminalSize
   Скачивание — WS закрывается, статус Downloading..., partial-файл
   ============================================================ */

// -------------------- Терминал (размер как в оригинале) --------------------
let termCols = 80;
let termRows = 24;

const term = new Terminal({
    cursorBlink: true,
    cols: termCols,
    rows: termRows,
    convertEol: false,
    scrollback: 0,
    disableStdin: false,
    fontFamily: 'Consolas, "Courier New", monospace',
    theme: {
        background: '#000000',
        foreground: '#00ff88',
        cursor: '#00ff88'
    }
});

const container = document.getElementById('terminal-container');

let ws = null;
let reconnectTimer = null;
let intentionalClose = false;
let terminalActive = true;
let currentTab = 'terminal';

let archiveSize = 0;
let fileName = '';
let downloadController = null;
let downloadStartTime = 0;
let receivedBytes = 0;
let isDownloading = false;
let downloadChunks = [];
let downloadAbortReason = null;
let lastProgressUpdate = 0;
const PROGRESS_UPDATE_INTERVAL = 100;
let downloadInterval = null;
let archiveSizeTimer = null;
let startDownloadRequested = false;

// -------------------- Утилиты --------------------
function formatBytes(bytes) {
    if (bytes < 1024) return bytes + ' байт';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' КБ';
    return (bytes / (1024 * 1024)).toFixed(2) + ' МБ';
}

function formatTimeMs(ms) {
    if (ms < 0) ms = 0;
    const totalSec = Math.floor(ms / 1000);
    const min = String(Math.floor(totalSec / 60)).padStart(2, '0');
    const sec = String(totalSec % 60).padStart(2, '0');
    return min + ':' + sec;
}

function setStatus(mode) {
    const dot = document.getElementById('dot');
    const status = document.getElementById('status');
    if (!dot || !status) return;
    dot.classList.remove('connected', 'downloading');
    if (mode === 'download') {
        dot.classList.add('downloading');
        status.textContent = 'Downloading...';
    } else if (mode === true) {
        dot.classList.add('connected');
        status.textContent = 'Connected';
    } else {
        status.textContent = 'Disconnected';
    }
}

// -------------------- Размер шрифта под контейнер (оригинал) --------------------
function resizeTerminal() {
    if (!container) return;
    const rect = container.getBoundingClientRect();
    if (rect.width < 10 || rect.height < 10) return;
    const fontByWidth = rect.width / (termCols * 0.62);
    const fontByHeight = rect.height / (termRows * 1.25);
    const fontSize = Math.floor(Math.min(fontByWidth, fontByHeight));
    term.options.fontSize = Math.max(8, Math.min(fontSize, 40));
    term.refresh(0, termRows - 1);
}

// -------------------- Парсинг CSI из кадра (оригинал) --------------------
function parseTerminalSize(text) {
    const match = text.match(/\x1B\[8;(\d+);(\d+)t/);
    if (!match) return;
    const newRows = parseInt(match[2], 10);
    const newCols = parseInt(match[1], 10);
    if (newRows !== termRows || newCols !== termCols) {
        termRows = newRows;
        termCols = newCols;
        term.resize(termCols, termRows);
        setTimeout(resizeTerminal, 0);
    }
}

/** Лампы: ESC [ <state> q  — биты 0x01=СТОП, 0x02=ОЖИДАНИЕ, 0x04=РАБОТА */
function parseLampState(text) {
    const match = text.match(/\x1B\[(\d+)q/);
    if (!match) return;
    const state = parseInt(match[1], 10);
    const ledStop = document.getElementById('led-stop');
    const ledWait = document.getElementById('led-wait');
    const ledRun  = document.getElementById('led-run');
    if (ledStop) ledStop.classList.toggle('active-stop', (state & 0x01) !== 0);
    if (ledWait) ledWait.classList.toggle('active-wait', (state & 0x02) !== 0);
    if (ledRun)  ledRun.classList.toggle('active-run',  (state & 0x04) !== 0);
}

/**
 * Оригинальная обработка бинарного кадра экрана:
 *  - подмена проблемных байтов
 *  - windows-1251
 *  - parse size / lamps
 *  - убрать null и CSI CUP
 *  - clear + home + write
 */
function writeTerminalScreen(arrayBuffer) {
    const bytes = new Uint8Array(arrayBuffer);
    const filtered = bytes.map(b => {
        if (b === 0x81) return 0x76;
        if (b === 0x7F) return 0x30;
        if (b === 0x80) return 0x5E;
        return b;
    }).filter(b => b !== 0x00);

    const decoder = new TextDecoder('windows-1251');
    let text = decoder.decode(filtered);

    parseTerminalSize(text);
    parseLampState(text);

    text = text.replace(/\x00/g, '');
    text = text.replace(/\x1B\[\d+;\d+H/g, '');

    term.write('\x1b[2J\x1b[H');
    term.write(text);
}

// -------------------- WebSocket --------------------
function getWsUrl() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return proto + '//' + location.host + '/ws';
}

function connectWebSocket() {
    if (isDownloading) return;
    // Уже есть живое или устанавливающееся соединение
    if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) {
        return;
    }

    intentionalClose = false;
    // Не вызываем close() на старом сокете здесь — это провоцирует гонку
    // с сервером (старый fd ещё owner → 503 → Invalid frame header).
    ws = null;

    console.log('Connecting WebSocket to', getWsUrl());
    try {
        ws = new WebSocket(getWsUrl());
    } catch (e) {
        console.error('WS create error', e);
        scheduleReconnect();
        return;
    }

    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        console.log('WebSocket connected');
        setStatus(true);

        setTimeout(() => {
            const now = new Date();
            if (ws && ws.readyState === WebSocket.OPEN) {
                try {
                    ws.send(JSON.stringify({
                        action: 'setTime',
                        year: now.getFullYear(),
                        month: now.getMonth() + 1,
                        day: now.getDate(),
                        hour: now.getHours(),
                        minute: now.getMinutes(),
                        second: now.getSeconds()
                    }));
                } catch (e) {}
            }
        }, 500);

        const isDownload = (currentTab === 'downloads');
        try {
            ws.send(JSON.stringify({ action: 'setTerminalActive', active: isDownload }));
            ws.send(JSON.stringify({ action: 'getArchiveSize' }));
        } catch (e) {}
    };

    ws.onclose = () => {
        console.log('WebSocket closed');
        const wasIntentional = intentionalClose;
        ws = null;
        if (!wasIntentional && !isDownloading) {
            setStatus(false);
            scheduleReconnect();
        }
    };

    ws.onerror = (err) => {
        console.log('WS error:', err);
        // onclose придёт следом — реконнект там
    };

    ws.onmessage = (event) => {
        // ===== БИНАРНЫЙ КАДР ЭКРАНА (оригинал) =====
        if (event.data instanceof ArrayBuffer) {
            writeTerminalScreen(event.data);
            return;
        }

        if (typeof event.data !== 'string') return;

        if (event.data === 'ping') {
            try { ws.send('pong'); } catch (e) {}
            return;
        }
        if (event.data === 'pong') return;

        let msg;
        try {
            msg = JSON.parse(event.data);
        } catch (e) {
            console.log('Non-JSON string:', event.data);
            return;
        }

        console.log('📨 WS message:', msg.type, msg);

        // Подтверждение старта download → HTTP fetch
        if (msg.type === 'log' && msg.msg === 'Download start requested') {
            if (startDownloadRequested && !isDownloading) {
                console.log('Server confirmed, starting HTTP fetch...');
                startHttpFetch();
            }
            return;
        }

        switch (msg.type) {
            case 'log':
                console.log('[LOG]', msg.msg);
                break;

            case 'archiveSize':
                archiveSize = msg.size || 0;
                {
                    const el = document.getElementById('archive-size');
                    const tot = document.getElementById('total-bytes');
                    if (el) el.textContent = archiveSize + ' байт';
                    if (tot) tot.textContent = archiveSize;
                }
                break;

            case 'fileName':
                if (msg.name) {
                    const parts = String(msg.name).replace(/\\/g, '/').split('/');
                    fileName = parts[parts.length - 1] || 'archive.irz';
                }
                break;

            case 'progress':
                // прогресс считаем на клиенте из HTTP
                break;

            case 'downloadComplete':
            case 'downloadStopped':
                // сервер закончил — UI уже сбросится в finally fetch
                break;

            case 'error':
                console.error('Server error:', msg.msg);
                break;

            default:
                // Телеметрия без type — не трогаем лампы (они из CSI в кадре)
                break;
        }
    };
}

function scheduleReconnect() {
    if (reconnectTimer) clearTimeout(reconnectTimer);
    if (isDownloading) return;
    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectWebSocket();
    }, 2500);
}

function closeWebSocketForDownload() {
    intentionalClose = true;
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    if (ws) {
        try { ws.close(1000, 'download'); } catch (e) {}
        ws = null;
    }
    setStatus('download');
}

// -------------------- Клавиши (оригинал: простые строки) --------------------
function sendKey(key) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        if (terminalActive) term.writeln('\r\n[Не подключено]');
        return;
    }
    try {
        ws.send(String(key));
    } catch (e) {
        console.warn('sendKey failed', e);
    }
}

// -------------------- Вкладки --------------------
function showTab(name) {
    if (currentTab === 'downloads' && name !== 'downloads' && isDownloading) {
        downloadAbortReason = 'tab';
        stopDownload();
    }

    currentTab = name;
    document.querySelectorAll('.page').forEach(x => x.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(x => x.classList.remove('active'));
    document.getElementById(name + '-page').classList.add('active');
    const tabs = document.querySelectorAll('.tab');
    if (name === 'terminal') tabs[0].classList.add('active');
    else tabs[1].classList.add('active');

    if (ws && ws.readyState === WebSocket.OPEN) {
        const isDownload = (name === 'downloads');
        try {
            ws.send(JSON.stringify({ action: 'setTerminalActive', active: isDownload }));
        } catch (e) {}
    }

    if (name === 'terminal') {
        terminalActive = true;
        stopArchiveSizePolling();
        if (!isDownloading) {
            if (!ws || ws.readyState !== WebSocket.OPEN) connectWebSocket();
        }
        setTimeout(resizeTerminal, 50);
    } else {
        terminalActive = false;
        startArchiveSizePolling();
    }
}

// -------------------- Опрос размера архива --------------------
function requestArchiveSize() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    try {
        ws.send(JSON.stringify({ action: 'getArchiveSize' }));
    } catch (e) {}
}

function startArchiveSizePolling() {
    if (archiveSizeTimer) return;
    const page = document.getElementById('downloads-page');
    if (!page || !page.classList.contains('active') || isDownloading) return;
    requestArchiveSize();
    archiveSizeTimer = setInterval(requestArchiveSize, 5000);
}

function stopArchiveSizePolling() {
    if (archiveSizeTimer) {
        clearInterval(archiveSizeTimer);
        archiveSizeTimer = null;
    }
}

// -------------------- Скачивание --------------------
function updateProgressUI(received, total) {
    const bar = document.getElementById('progress-bar');
    const txt = document.getElementById('progress-text');
    const recv = document.getElementById('received-bytes');
    const percent = total > 0 ? Math.min(100, (received / total) * 100) : 0;
    if (bar) bar.style.width = percent + '%';
    if (txt) txt.textContent = Math.round(percent) + '%';
    if (recv) recv.textContent = received;

    const elapsed = Date.now() - downloadStartTime;
    const elElapsed = document.getElementById('elapsed');
    if (elElapsed) elElapsed.textContent = formatTimeMs(elapsed);

    const speed = elapsed > 300 ? received / (elapsed / 1000) : 0;
    const elSpeed = document.getElementById('speed');
    if (elSpeed) elSpeed.textContent = Math.round(speed) + ' байт/с';

    const elEta = document.getElementById('eta');
    if (elEta) {
        if (speed > 100 && total > received) {
            elEta.textContent = formatTimeMs(((total - received) / speed) * 1000);
        } else {
            elEta.textContent = '—';
        }
    }
}

/**
 * Имя файла из HTTP-заголовков ответа /download.
 * Сервер кладёт URL-encoded UTF-8 в X-File-Name и filename*=UTF-8''...
 * (WS на время скачивания закрыт, поэтому имя из WS не приходит.)
 */
function parseFileNameFromHeaders(res) {
    const x = res.headers.get('X-File-Name');
    if (x) {
        try {
            const decoded = decodeURIComponent(x.trim());
            if (decoded) return decoded.replace(/[\\/]/g, '_');
        } catch (e) {}
    }
    const cd = res.headers.get('Content-Disposition');
    if (cd) {
        // filename*=UTF-8''encoded
        let m = cd.match(/filename\*\s*=\s*(?:UTF-8''|utf-8'')([^;]+)/i);
        if (m) {
            try {
                const decoded = decodeURIComponent(m[1].trim().replace(/^["']|["']$/g, ''));
                if (decoded) return decoded.replace(/[\\/]/g, '_');
            } catch (e) {}
        }
        // filename="..."
        m = cd.match(/filename\s*=\s*("?)([^";]+)\1/i);
        if (m && m[2]) return m[2].trim().replace(/[\\/]/g, '_');
    }
    return '';
}

function makePartialName(original) {
    if (!original) return 'archive_partial.irz';
    const m = original.match(/^(.*)(\.[^.]+)$/);
    if (m) return m[1] + '_partial' + m[2];
    return original + '_partial';
}

function saveBlob(chunks, filename) {
    if (!chunks || chunks.length === 0) return false;
    const blob = new Blob(chunks);
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename || 'archive.bin';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
    return true;
}

function startDownload() {
    if (isDownloading) return;

    if (!ws || ws.readyState !== WebSocket.OPEN) {
        if (terminalActive) term.writeln('\x1b[31mНет соединения с сервером\x1b[0m');
        return;
    }

    if (archiveSize <= 0) {
        if (terminalActive) term.writeln('\x1b[33mРазмер архива неизвестен. Подождите обновления.\x1b[0m');
        requestArchiveSize();
        return;
    }

    console.log('Starting download, archiveSize:', archiveSize);
    stopArchiveSizePolling();
    startDownloadRequested = true;
    receivedBytes = 0;
    downloadChunks = [];
    downloadAbortReason = null;
    fileName = ''; /* имя придёт из HTTP-заголовков после первого пакета */

    const btnDownload = document.getElementById('btn-download');
    const btnStop = document.getElementById('btn-stop');
    if (btnDownload) btnDownload.disabled = true;
    if (btnStop) btnStop.disabled = false;
    updateProgressUI(0, archiveSize);

    try {
        ws.send(JSON.stringify({ action: 'startDownload' }));
        console.log('Sent startDownload command to server');
    } catch (e) {}
}

async function startHttpFetch() {
    if (isDownloading) return;
    isDownloading = true;
    startDownloadRequested = false;
    downloadStartTime = Date.now();
    receivedBytes = 0;
    downloadChunks = [];

    // Закрываем WS на время скачивания (SoftAP)
    closeWebSocketForDownload();

    downloadController = new AbortController();

    if (downloadInterval) clearInterval(downloadInterval);
    downloadInterval = setInterval(() => {
        if (isDownloading && downloadStartTime) {
            const el = document.getElementById('elapsed');
            if (el) el.textContent = formatTimeMs(Date.now() - downloadStartTime);
        }
    }, 1000);

    try {
        console.log('Starting HTTP fetch: /download');
        const res = await fetch('/download', {
            cache: 'no-store',
            signal: downloadController.signal
        });

        console.log('HTTP response:', res.status, res.statusText);
        if (!res.ok) throw new Error('HTTP ' + res.status);
        if (!res.body) throw new Error('No response body');

        /* Имя из первого пакета (сервер уже извлёк и положил в заголовки) */
        const fromHdr = parseFileNameFromHeaders(res);
        if (fromHdr) {
            fileName = fromHdr;
            console.log('File name from HTTP headers:', fileName);
        }

        const cl = res.headers.get('Content-Length');
        if (cl) {
            archiveSize = parseInt(cl, 10);
            const tot = document.getElementById('total-bytes');
            if (tot) tot.textContent = archiveSize;
        }

        const reader = res.body.getReader();
        let chunkCount = 0;

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;

            if (chunkCount === 0) console.log('First chunk received!');
            chunkCount++;
            downloadChunks.push(value);
            receivedBytes += value.byteLength;

            const now = Date.now();
            if (now - lastProgressUpdate > PROGRESS_UPDATE_INTERVAL) {
                lastProgressUpdate = now;
                updateProgressUI(receivedBytes, archiveSize);
            }
        }

        console.log('Download complete, received:', receivedBytes, 'name:', fileName);
        updateProgressUI(receivedBytes, archiveSize || receivedBytes);
        saveBlob(downloadChunks, fileName || 'archive.bin');

    } catch (e) {
        if (e.name === 'AbortError') {
            console.log('Download aborted, reason =', downloadAbortReason || 'user');
            if (receivedBytes > 0) {
                /* partial — то же имя из заголовков (уже в fileName), суффикс _partial */
                saveBlob(downloadChunks, makePartialName(fileName || 'archive.bin'));
                if (downloadAbortReason !== 'tab') {
                    alert('Скачивание остановлено.\nСохранён частичный файл: ' + formatBytes(receivedBytes));
                }
            }
        } else {
            console.error('Download error:', e);
            if (receivedBytes > 0) {
                saveBlob(downloadChunks, makePartialName(fileName || 'archive.bin'));
                alert('Ошибка скачивания, но сохранён частичный файл (' +
                      formatBytes(receivedBytes) + ').\n' + e.message);
            } else {
                alert('Ошибка скачивания: ' + e.message);
            }
        }
    } finally {
        isDownloading = false;
        downloadController = null;
        downloadChunks = [];
        downloadAbortReason = null;
        startDownloadRequested = false;
        if (downloadInterval) {
            clearInterval(downloadInterval);
            downloadInterval = null;
        }
        const btnDownload = document.getElementById('btn-download');
        const btnStop = document.getElementById('btn-stop');
        if (btnDownload) btnDownload.disabled = false;
        if (btnStop) btnStop.disabled = true;

        if (currentTab === 'terminal') {
            connectWebSocket();
        } else {
            setStatus(false);
            startArchiveSizePolling();
        }
    }
}

function stopDownload() {
    console.log('Stop download requested');
    if (!downloadAbortReason) downloadAbortReason = 'user';

    if (downloadController) {
        downloadController.abort();
    }

    if (ws && ws.readyState === WebSocket.OPEN) {
        try {
            ws.send(JSON.stringify({ action: 'stopDownload' }));
        } catch (e) {}
        if (terminalActive) term.writeln('\x1b[33mОстановка запрошена\x1b[0m');
    }

    startDownloadRequested = false;
}

// -------------------- Клавиатура (оригинал) --------------------
document.addEventListener('keydown', e => {
    if (!terminalActive) return;
    switch (e.key) {
        case '1': sendKey('f1'); break;
        case '2': sendKey('f2'); break;
        case '3': sendKey('f3'); break;
        case 'Escape': sendKey('esc'); break;
        case 'Enter': sendKey('enter'); break;
        case 'q': case 'Q': sendKey('esc'); break;
        case 'e': case 'E': sendKey('enter'); break;
        case 'w': case 'W': case 'ArrowUp': sendKey('up'); break;
        case 's': case 'S': case 'ArrowDown': sendKey('down'); break;
        case 'a': case 'A': case 'ArrowLeft': sendKey('left'); break;
        case 'd': case 'D': case 'ArrowRight': sendKey('right'); break;
        case '5': sendKey('start'); break;
        case '6': sendKey('stop'); break;
        default: return;
    }
    e.preventDefault();
});

// -------------------- Init --------------------
document.addEventListener('DOMContentLoaded', () => {
    term.open(container);
    resizeTerminal();
    term.writeln('ESP32 Terminal');
    term.writeln('');
    connectWebSocket();
    window.addEventListener('resize', resizeTerminal);
    if (window.ResizeObserver && container) {
        const ro = new ResizeObserver(resizeTerminal);
        ro.observe(container);
    }
    setTimeout(resizeTerminal, 100);
});

window.addEventListener('beforeunload', () => {
    if (ws && ws.readyState === WebSocket.OPEN && isDownloading) {
        try { ws.send(JSON.stringify({ action: 'stopDownload' })); } catch (e) {}
    }
});

window.showTab = showTab;
window.sendKey = sendKey;
window.startDownload = startDownload;
window.stopDownload = stopDownload;
