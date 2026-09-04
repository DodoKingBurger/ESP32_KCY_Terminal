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
    cursorBlink: false,
    cursorStyle: 'underline',
    cols: termCols,
    rows: termRows,
    convertEol: false,
    scrollback: 0,
    disableStdin: false,
    fontFamily: 'Consolas, "Courier New", monospace',
    theme: {
        background: '#000000',
        foreground: '#00ff88',
        /* Курсор не рисуем: экран КСУ сам показывает позицию */
        cursor: '#000000',
        cursorAccent: '#000000'
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
let needsWsRefresh = false;
let terminalFrameWatchdog = null;
let terminalReconnectAttempts = 0;

// -------------------- Firmware --------------------
let fwSelectedFile = null;
let fwUploadInProgress = false;
let fwFileReady = false;


/** Версия прошивки ESP (PROJECT_VER) — topbar рядом с «ESP32 TERMINAL» */
function setEspAppVersion(ver) {
    const el = document.getElementById('esp-app-ver');
    if (!el) return;
    const v = (ver && String(ver).trim()) ? String(ver).trim() : '—';
    el.textContent = v.indexOf('v') === 0 ? v : ('v' + v);
    el.title = 'Версия прошивки ESP: ' + v;
}

function fetchEspAppVersion() {
    fetch('/version', { cache: 'no-store' })
        .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
        .then(function (j) {
            if (j && j.version) setEspAppVersion(j.version);
        })
        .catch(function () { /* softAP ещё поднимается */ });
}

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
 *  - подмена байтов скроллбара на unicode-стрелки/ползунок
 *  - windows-1251
 *  - parse size / lamps
 *  - убрать null и CSI CUP
 *  - clear + home + write, курсор xterm скрыт
 *
 * КСУ: 0x80 = вверх, 0x81 = вниз, 0x7F = ползунок скроллбара
 */
function writeTerminalScreen(arrayBuffer) {
    const bytes = new Uint8Array(arrayBuffer);
    const decoder = new TextDecoder('windows-1251');
    let text = '';
    let chunk = [];

    const flushChunk = () => {
        if (chunk.length === 0) return;
        text += decoder.decode(Uint8Array.from(chunk));
        chunk = [];
    };

    for (let i = 0; i < bytes.length; i++) {
        const b = bytes[i];
        if (b === 0x00) continue;
        /* Символы ближе к реальному скроллбару (одна ячейка) */
        if (b === 0x80) {
            flushChunk();
            text += '▲'; /* вверх */
            continue;
        }
        if (b === 0x81) {
            flushChunk();
            text += '▼'; /* вниз */
            continue;
        }
        if (b === 0x7F) {
            flushChunk();
            text += '█'; /* ползунок */
            continue;
        }
        chunk.push(b);
    }
    flushChunk();

    parseTerminalSize(text);
    parseLampState(text);

    text = text.replace(/\x00/g, '');
    text = text.replace(/\x1B\[\d+;\d+H/g, '');

    term.write('\x1b[2J\x1b[H');
    term.write(text);
    term.write('\x1b[?25l'); /* hide cursor — без рамки на последнем символе */
}

// -------------------- WebSocket --------------------
function getWsUrl() {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    return proto + '//' + location.host + '/ws';
}

function connectWebSocket() {
    if (isDownloading) return;
    if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) {
        return;
    }

    intentionalClose = false;
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
        updateFirmwareButtons();
        fetchEspAppVersion();

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

        const pauseTerminal = (currentTab === 'downloads' || currentTab === 'firmware');
        try {
            ws.send(JSON.stringify({ action: 'setTerminalActive', active: pauseTerminal }));
            if (currentTab === 'downloads') {
                ws.send(JSON.stringify({ action: 'getArchiveSize' }));
            }
            if (currentTab === 'firmware') {
                setTimeout(() => {
                    if (currentTab === 'firmware' && ws && ws.readyState === WebSocket.OPEN) {
                        requestFirmwareVersion();
                    }
                }, 100);
            }
            if (currentTab === 'terminal') {
                startTerminalFrameWatchdog();
            }
        } catch (e) {}
    };

    ws.onclose = () => {
        console.log('WebSocket closed');
        const wasIntentional = intentionalClose;
        ws = null;
        if (isDownloading) {
            needsWsRefresh = true;
            return;
        }
        if (!wasIntentional) {
            setStatus(false);
            scheduleReconnect();
        }
    };

    ws.onerror = (err) => {
        console.log('WS error:', err);
    };

    ws.onmessage = (event) => {
        if (event.data instanceof ArrayBuffer) {
            if (needsWsRefresh) needsWsRefresh = false;
            terminalReconnectAttempts = 0;
            if (terminalFrameWatchdog) {
                clearTimeout(terminalFrameWatchdog);
                terminalFrameWatchdog = null;
            }
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
                break;

            case 'downloadComplete':
            case 'downloadStopped':
                break;

            case 'firmwareVersion':
                {
                    const el = document.getElementById('fw-version');
                    if (el) el.textContent = msg.version || '—';
                    /* статус не трогаем — там может быть результат 0x008A */
                }
                break;

            case 'firmwareUploadStart':
                fwUploadInProgress = true;
                updateFirmwareButtons();
                setFwStatus('идёт отправка…');
                if (typeof msg.size === 'number' && msg.size > 0) {
                    updateFwProgress(0, msg.size);
                }
                break;

            case 'firmwareDebug':
                console.log('[FW]', msg.msg || msg);
                break;

            case 'firmwareProgress':
                fwUploadInProgress = true;
                updateFwProgress(msg.received || 0, msg.total || 0);
                /* статус не спамим процентами */
                break;

            case 'firmwareUploadComplete':
                fwUploadInProgress = false;
                fwFileReady = true;
                if (typeof msg.size === 'number' && msg.size > 0) {
                    updateFwProgress(msg.size, msg.size);
                }
                if (msg.target === 'esp' || msg.reboot) {
                    setFwStatus('OTA OK — ESP перезагружается…');
                } else {
                    setFwStatus('файл отправлен, запуск перепрошивки…');
                }
                updateFirmwareButtons();
                break;

            case 'firmwareUploadError':
                fwUploadInProgress = false;
                fwFileReady = false;
                {
                    let t = 'ошибка 0x65';
                    if (msg.msg) t += ': ' + msg.msg;
                    if (typeof msg.offset === 'number') t += ' @' + msg.offset;
                    setFwStatus(t);
                }
                updateFirmwareButtons();
                break;

            case 'reflashStarted':
                setFwStatus('FC 0x06 отправлена (код 0x20), опрос 0x008A…');
                break;

            case 'reflashStatus':
                {
                    const c = (typeof msg.code === 'number') ? msg.code : -1;
                    const m = msg.msg || '';
                    if (c === 7) {
                        setFwStatus('идёт процесс перепрограммирования…');
                    } else if (c === -2) {
                        setFwStatus(m || 'ожидание ответа КСУ…');
                    } else if (c < 0) {
                        setFwStatus(m || 'ошибка опроса статуса');
                    } else {
                        setFwStatus((m ? m : ('код ' + c)));
                    }
                    console.log('[FW reflash 0x008A]', c, m);
                }
                break;

            case 'deviceCode':
                console.log('Device code set:', msg.code);
                break;

            case 'error':
                console.error('Server error:', msg.msg);
                if (msg.msg && /FC06|reflash|0x06/i.test(msg.msg)) {
                    setFwStatus('ошибка: ' + msg.msg);
                }
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

/**
 * Принудительный reconnect: закрыть текущий fd на ESP и поднять новый.
 * Нужен после длинного /download — иначе setTerminalActive и binary-кадры
 * могут молча теряться при «зомби» WS.
 * needsWsRefresh здесь НЕ сбрасываем — только onopen на terminal / первый кадр.
 */
function forceReconnectWebSocket(reason) {
    if (isDownloading) return;
    console.log('Force WS reconnect:', reason || '', 'needsRefresh=', needsWsRefresh);
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    if (terminalFrameWatchdog) {
        clearTimeout(terminalFrameWatchdog);
        terminalFrameWatchdog = null;
    }
    intentionalClose = true;
    if (ws) {
        try { ws.close(1000, reason || 'reconnect'); } catch (e) {}
        ws = null;
    }
    setStatus(false);
    setTimeout(() => {
        intentionalClose = false;
        connectWebSocket();
    }, 600);
}

/**
 * После setTerminalActive(false) ждём binary-кадр. Если тишина — зомби WS,
 * ещё один force-reconnect (ограничено attempts).
 */
function startTerminalFrameWatchdog() {
    if (terminalFrameWatchdog) {
        clearTimeout(terminalFrameWatchdog);
        terminalFrameWatchdog = null;
    }
    if (currentTab !== 'terminal' || isDownloading) return;
    terminalFrameWatchdog = setTimeout(() => {
        terminalFrameWatchdog = null;
        if (currentTab !== 'terminal' || isDownloading) return;
        if (terminalReconnectAttempts >= 3) {
            console.warn('Terminal frames still missing after reconnects');
            setStatus(false);
            return;
        }
        terminalReconnectAttempts++;
        console.warn('No terminal frames — force reconnect #', terminalReconnectAttempts);
        needsWsRefresh = true;
        forceReconnectWebSocket('no-frames-watchdog');
    }, 3500);
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
    const page = document.getElementById(name + '-page');
    if (page) page.classList.add('active');
    document.querySelectorAll('.tab').forEach(btn => {
        if (btn.getAttribute('onclick') && btn.getAttribute('onclick').indexOf("'" + name + "'") >= 0) {
            btn.classList.add('active');
        }
    });

    stopArchiveSizePolling();
    if (name !== 'terminal' && terminalFrameWatchdog) {
        clearTimeout(terminalFrameWatchdog);
        terminalFrameWatchdog = null;
    }

    if (name === 'terminal') {
        terminalActive = true;
        if (!isDownloading) {
            if (needsWsRefresh || !ws || ws.readyState !== WebSocket.OPEN) {
                /* После полного download (особенно ~10+ мин) WS часто зомби:
                   readyState=OPEN, а setTerminalActive/binary уже не доходят.
                   Флаг needsWsRefresh липкий до первого кадра / onopen terminal. */
                forceReconnectWebSocket(needsWsRefresh ? 'post-download-tab' : 'terminal-tab');
            } else {
                try {
                    ws.send(JSON.stringify({ action: 'setTerminalActive', active: false }));
                } catch (e) {}
                setStatus(true);
                startTerminalFrameWatchdog();
            }
        }
        setTimeout(resizeTerminal, 50);
    } else if (name === 'downloads') {
        terminalActive = false;
        if (needsWsRefresh) {
            forceReconnectWebSocket('post-download-downloads');
        } else if (ws && ws.readyState === WebSocket.OPEN) {
            try {
                ws.send(JSON.stringify({ action: 'setTerminalActive', active: true }));
            } catch (e) {}
        } else if (!isDownloading) {
            connectWebSocket();
        }
        startArchiveSizePolling();
    } else if (name === 'firmware') {
        terminalActive = false;
        if (needsWsRefresh) {
            forceReconnectWebSocket('post-download-firmware');
            setFwStatus('подключение…');
        } else if (ws && ws.readyState === WebSocket.OPEN) {
            try {
                ws.send(JSON.stringify({ action: 'setTerminalActive', active: true }));
            } catch (e) {}
            requestFirmwareVersion();
        } else if (!isDownloading) {
            connectWebSocket();
            setFwStatus('подключение…');
        } else {
            setFwStatus('идёт скачивание — дождитесь');
        }
        updateFirmwareButtons();
    } else {
        terminalActive = false;
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
    archiveSizeTimer = setInterval(requestArchiveSize, 15000);
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
    fileName = ''; 
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
        let res = await fetch('/download', {
            cache: 'no-store',
            signal: downloadController.signal
        });

        console.log('HTTP response:', res.status, res.statusText);
        if (res.status === 503) {
            console.warn('Server busy (503), retry in 2s...');
            await new Promise(r => setTimeout(r, 2000));
            res = await fetch('/download', {
                cache: 'no-store',
                signal: downloadController.signal
            });
            console.log('HTTP retry response:', res.status, res.statusText);
        }
        if (!res.ok) throw new Error('HTTP ' + res.status);
        if (!res.body) throw new Error('No response body');

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
        if (btnStop) btnStop.disabled = true;
        
        if (btnDownload) btnDownload.disabled = true;

        needsWsRefresh = true;
        intentionalClose = false;
        setTimeout(() => {
            if (btnDownload) btnDownload.disabled = false;
            
            forceReconnectWebSocket('post-download');
            setTimeout(() => {
                if (currentTab === 'downloads' && !isDownloading) {
                    startArchiveSizePolling();
                }
            }, 1000);
        }, 1500);
    }
}

function stopDownload() {
    console.log('Stop download requested');
    if (!downloadAbortReason) downloadAbortReason = 'user';

    if (ws && ws.readyState === WebSocket.OPEN) {
        try {
            ws.send(JSON.stringify({ action: 'stopDownload' }));
        } catch (e) {}
    }

    try {
        fetch('/stop-download', { method: 'POST', cache: 'no-store' }).catch(function () {});
    } catch (e) {}

    if (downloadController) {
        try { downloadController.abort(); } catch (e) {}
        downloadController = null;
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
    const fwInput = document.getElementById('fw-file-input');
    if (fwInput) fwInput.addEventListener('change', onFirmwareFileSelected);
    if (typeof onFwTargetChange === 'function') onFwTargetChange();
    fetchEspAppVersion();
    updateFirmwareButtons();
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


// -------------------- Перепрошивка --------------------
function setFwStatus(text) {
    const el = document.getElementById('fw-status');
    if (el) el.textContent = text;
}

function updateFwProgress(received, total) {
    received = Math.max(0, Number(received) || 0);
    total = Math.max(0, Number(total) || 0);
    if (typeof window._fwProgRecv === 'number' && received < window._fwProgRecv &&
        total === window._fwProgTotal) {
        received = window._fwProgRecv;
    }
    window._fwProgRecv = received;
    window._fwProgTotal = total;

    const pct = total > 0 ? Math.min(100, (received / total) * 100) : 0;
    const pctStr = Math.round(pct) + '%';
    const bar = document.getElementById('fw-progress-bar');
    const txt = document.getElementById('fw-progress-text');
    const pctEl = document.getElementById('fw-progress-pct');
    const wrap = document.getElementById('fw-progress-wrap');
    if (wrap) wrap.style.display = '';
    if (bar) bar.style.width = pct + '%';
    if (txt) txt.textContent = pctStr;
    if (pctEl) pctEl.textContent = pctStr;
}

function getFwTarget() {
    const el = document.getElementById('fw-target');
    return (el && el.value === 'esp') ? 'esp' : 'ksu';
}

function onFwTargetChange() {
    const target = getFwTarget();
    const input = document.getElementById('fw-file-input');
    const label = document.getElementById('fw-file-label');
    const btnRf = document.getElementById('btn-fw-reflash');
    const verRow = document.getElementById('fw-version-row');
    const btnUp = document.getElementById('btn-fw-upload');

    fwSelectedFile = null;
    fwFileReady = false;
    if (input) {
        input.value = '';
        if (target === 'esp') {
            input.accept = '.bin,application/octet-stream';
        } else {
            input.accept = '.ubt,application/octet-stream';
        }
    }
    if (label) {
        label.textContent = target === 'esp'
            ? 'Файл прошивки ESP (.bin):'
            : 'Файл прошивки КСУ (.ubt):';
    }
    const nameEl = document.getElementById('fw-file-name');
    const sizeEl = document.getElementById('fw-file-size');
    if (nameEl) nameEl.textContent = '—';
    if (sizeEl) sizeEl.textContent = '—';
    if (btnUp) {
        btnUp.textContent = target === 'esp'
            ? 'Прошить ESP (OTA)'
            : 'Отправить и перепрошить';
    }
    /* FC06 только для КСУ */
    if (btnRf) btnRf.style.display = target === 'esp' ? 'none' : '';
    if (verRow) verRow.style.display = target === 'esp' ? 'none' : '';
    updateFwProgress(0, 0);
    setFwStatus(target === 'esp' ? 'цель: ESP32 (OTA)' : 'цель: КСУ');
    updateFirmwareButtons();
}

function updateFirmwareButtons() {
    const btnUp = document.getElementById('btn-fw-upload');
    const btnRf = document.getElementById('btn-fw-reflash');
    const wsOk = !!(ws && ws.readyState === WebSocket.OPEN);
    const canUpload = !!(fwSelectedFile && fwSelectedFile.size > 0 && !fwUploadInProgress);
    if (btnUp) btnUp.disabled = !canUpload;
    /* Повтор FC06 — только КСУ */
    if (btnRf) {
        const isKsu = getFwTarget() === 'ksu';
        btnRf.disabled = !isKsu || !wsOk || fwUploadInProgress;
        btnRf.style.display = isKsu ? '' : 'none';
    }
}

function requestFirmwareVersion() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        setFwStatus('нет WS');
        return;
    }
    try {
        ws.send(JSON.stringify({ action: 'getFirmwareVersion' }));
        setFwStatus('запрос версии…');
    } catch (e) {}
}

function onFirmwareFileSelected(ev) {
    const input = ev.target;
    let f = input.files && input.files[0] ? input.files[0] : null;
    const target = getFwTarget();
    if (f) {
        if (target === 'esp' && !/\.bin$/i.test(f.name)) {
            alert('Для ESP нужен файл .bin (build/uart_echo.bin)');
            input.value = '';
            f = null;
        } else if (target === 'ksu' && !/\.ubt$/i.test(f.name)) {
            alert('Для КСУ нужен файл с расширением .ubt');
            input.value = '';
            f = null;
        }
    }
    fwSelectedFile = f;
    fwFileReady = false;
    const nameEl = document.getElementById('fw-file-name');
    const sizeEl = document.getElementById('fw-file-size');
    const chooseBtn = document.getElementById('btn-fw-choose');
    if (!f) {
        if (nameEl) nameEl.textContent = '—';
        if (sizeEl) sizeEl.textContent = '—';
        if (chooseBtn) chooseBtn.textContent = 'Выбрать файл';
        updateFwProgress(0, 0);
        setFwStatus('файл не выбран');
    } else {
        if (nameEl) nameEl.textContent = f.name;
        if (sizeEl) sizeEl.textContent = (typeof formatBytes === 'function' ? formatBytes(f.size) : f.size + ' байт') +
            ' (' + f.size + ')';
        if (chooseBtn) chooseBtn.textContent = 'Сменить файл';
        updateFwProgress(0, f.size);
        setFwStatus(target === 'esp' ? 'файл выбран (.bin)' : 'файл выбран (.ubt)');
    }
    updateFirmwareButtons();
}

/**
 * КСУ → POST /firmware (0x65 + FC06)
 * ESP → POST /ota (запись в ota-слот + reboot)
 */
function uploadFirmware() {
    if (!fwSelectedFile || fwSelectedFile.size <= 0 || fwUploadInProgress) return;
    const target = getFwTarget();
    fwUploadInProgress = true;
    fwFileReady = false;
    updateFirmwareButtons();
    setFwStatus(target === 'esp' ? 'идёт OTA ESP…' : 'идёт отправка…');
    window._fwProgRecv = 0;
    window._fwProgTotal = fwSelectedFile.size;
    updateFwProgress(0, fwSelectedFile.size);

    const url = target === 'esp' ? '/ota' : '/firmware';
    const xhr = new XMLHttpRequest();
    xhr.open('POST', url, true);
    xhr.responseType = 'text';
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');

    /* Для ESP прогресс можно брать и с XHR (запись = приём HTTP) */
    if (target === 'esp') {
        xhr.upload.onprogress = function (ev) {
            if (!ev.lengthComputable) return;
            updateFwProgress(ev.loaded, ev.total || fwSelectedFile.size);
        };
    }

    xhr.onload = function () {
        if (xhr.status >= 200 && xhr.status < 300) {
            fwFileReady = true;
            if (fwUploadInProgress) {
                fwUploadInProgress = false;
                updateFwProgress(fwSelectedFile.size, fwSelectedFile.size);
                if (target === 'esp') {
                    setFwStatus('OTA OK — ESP перезагружается…');
                } else {
                    setFwStatus('файл отправлен, запуск перепрошивки…');
                }
            }
        } else {
            fwUploadInProgress = false;
            fwFileReady = false;
            let detail = '';
            try {
                const j = JSON.parse(xhr.responseText || '{}');
                if (j.msg) detail = j.msg;
                if (typeof j.offset === 'number') detail += ' (offset ' + j.offset + ')';
            } catch (e) {
                detail = (xhr.responseText || '').slice(0, 120);
            }
            setFwStatus('ошибка HTTP ' + xhr.status + (detail ? ': ' + detail : ''));
            alert('Не удалось прошить: HTTP ' + xhr.status + (detail ? '\n' + detail : ''));
        }
        updateFirmwareButtons();
    };

    xhr.onerror = function () {
        fwUploadInProgress = false;
        fwFileReady = false;
        /* после OTA reboot соединение рвётся — для esp это может быть норма */
        if (target === 'esp') {
            setFwStatus('связь оборвалась (возможна перезагрузка ESP)');
        } else {
            setFwStatus('ошибка сети');
            alert('Не удалось отправить файл (сеть)');
        }
        updateFirmwareButtons();
    };

    xhr.onabort = function () {
        fwUploadInProgress = false;
        fwFileReady = false;
        setFwStatus('отправка отменена');
        updateFirmwareButtons();
    };

    xhr.send(fwSelectedFile);
}

/** Код устройства FC 0x06: 0x20 (hex) = 32 (dec) — КСУ Linux */
function getFwDeviceCode() {
    return 0x20;
}

function startReflash() {
    if (getFwTarget() !== 'ksu') {
        alert('FC 0x06 только для КСУ');
        return;
    }
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        alert('Нет соединения WebSocket');
        return;
    }
    if (fwUploadInProgress) {
        alert('Дождитесь окончания отправки файла');
        return;
    }
    const code = 0x20;
    if (!confirm('Отправить FC 0x06 (код 0x20 / 32) и прочитать статус 0x008A?')) return;
    try {
        ws.send(JSON.stringify({ action: 'startReflash', code: code }));
        setFwStatus('команда перепрошивки (код 0x20)…');
    } catch (e) {
        alert('Ошибка отправки команды');
    }
}


window.showTab = showTab;
window.sendKey = sendKey;
window.startDownload = startDownload;
window.stopDownload = stopDownload;
window.requestFirmwareVersion = requestFirmwareVersion;
window.uploadFirmware = uploadFirmware;
window.startReflash = startReflash;
window.onFwTargetChange = onFwTargetChange;
