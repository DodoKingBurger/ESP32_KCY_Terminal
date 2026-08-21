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
/* После долгого HTTP-download SoftAP часто оставляет WS «полуживым»
   (readyState=OPEN, а кадры/команды уже не доходят). Нужен reconnect.
   Флаг липкий: снимается только после успешного onopen на вкладке «Терминал»
   (или после первого binary-кадра). Auto-reconnect на downloads его НЕ сбрасывает. */
let needsWsRefresh = false;
let terminalFrameWatchdog = null;
let terminalReconnectAttempts = 0;

// -------------------- Firmware --------------------
let fwSelectedFile = null;
let fwUploadInProgress = false;
let fwFileReady = false;


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
        updateFirmwareButtons();

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

        /* active:true = пауза кадров терминала (load_page_active на ESP) */
        const pauseTerminal = (currentTab === 'downloads' || currentTab === 'firmware');
        try {
            ws.send(JSON.stringify({ action: 'setTerminalActive', active: pauseTerminal }));
            if (currentTab === 'downloads') {
                ws.send(JSON.stringify({ action: 'getArchiveSize' }));
            }
            if (currentTab === 'firmware') {
                /* версия после реконнекта — WS уже OPEN */
                setTimeout(() => {
                    if (currentTab === 'firmware' && ws && ws.readyState === WebSocket.OPEN) {
                        requestFirmwareVersion();
                    }
                }, 100);
            }
            if (currentTab === 'terminal') {
                /* Снимаем sticky только когда реально на терминале.
                   Auto-reconnect после download (на downloads) флаг НЕ трогает. */
                startTerminalFrameWatchdog();
            }
        } catch (e) {}
    };

    ws.onclose = () => {
        console.log('WebSocket closed');
        const wasIntentional = intentionalClose;
        ws = null;
        if (isDownloading) {
            /* Во время длинного download SoftAP может уронить WS — reconnect после */
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
        // onclose придёт следом — реконнект там
    };

    ws.onmessage = (event) => {
        // ===== БИНАРНЫЙ КАДР ЭКРАНА (оригинал) =====
        if (event.data instanceof ArrayBuffer) {
            /* Живой поток кадров — SoftAP/WS восстановлены */
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

            case 'firmwareVersion':
                {
                    const el = document.getElementById('fw-version');
                    if (el) el.textContent = msg.version || '—';
                    setFwStatus('версия получена');
                }
                break;

            case 'firmwareUploadStart':
                setFwStatus('отправка…');
                break;

            case 'firmwareProgress':
                updateFwProgress(msg.received || 0, msg.total || 0);
                break;

            case 'firmwareUploadComplete':
                setFwStatus('файл отправлен');
                fwFileReady = true;
                updateFirmwareButtons();
                break;

            case 'firmwareUploadError':
                setFwStatus('ошибка отправки');
                fwFileReady = false;
                updateFirmwareButtons();
                break;

            case 'reflashStarted':
                setFwStatus('команда перепрошивки отправлена');
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
                /* Снять паузу кадров — иначе после «Загрузки» экран молчит */
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
            /* onopen сам запросит версию, когда WS поднимется */
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

    // WS оставляем открытым: stop уходит по WS, нет гонки сокетов SoftAP.
    // (Раньше close WS + abort HTTP ломали httpd → ERR_CONNECTION_RESET.)

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
        if (btnStop) btnStop.disabled = true;
        /* Пауза: handler на ESP должен выйти из цикла и отпустить сокет */
        if (btnDownload) btnDownload.disabled = true;

        /* Длинный /download на SoftAP часто «убивает» WS без onclose в браузере.
           Всегда помечаем и делаем force-reconnect — иначе терминал молчит. */
        needsWsRefresh = true;
        intentionalClose = false;
        setTimeout(() => {
            if (btnDownload) btnDownload.disabled = false;
            /* Дать HTTP-handler'у на ESP выйти из цикла и отпустить сокет */
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

    /* 1) WS stop — handler увидит флаг на следующей итерации */
    if (ws && ws.readyState === WebSocket.OPEN) {
        try {
            ws.send(JSON.stringify({ action: 'stopDownload' }));
        } catch (e) {}
    }
    /* 2) HTTP stop — запасной канал */
    try {
        fetch('/stop-download', { method: 'POST', cache: 'no-store' }).catch(function () {});
    } catch (e) {}

    /* 3) Обрыв тела /download */
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
    const pct = total > 0 ? Math.min(100, (received / total) * 100) : 0;
    const bar = document.getElementById('fw-progress-bar');
    const txt = document.getElementById('fw-progress-text');
    const sent = document.getElementById('fw-sent-bytes');
    const tot = document.getElementById('fw-total-bytes');
    if (bar) bar.style.width = pct + '%';
    if (txt) txt.textContent = Math.round(pct) + '%';
    if (sent) sent.textContent = typeof formatBytes === 'function' ? formatBytes(received) : received;
    if (tot) tot.textContent = typeof formatBytes === 'function' ? formatBytes(total) : total;
}

function updateFirmwareButtons() {
    const btnUp = document.getElementById('btn-fw-upload');
    const btnRf = document.getElementById('btn-fw-reflash');
    const wsOk = !!(ws && ws.readyState === WebSocket.OPEN);
    /* Отправка: файл выбран и size > 0 */
    const canUpload = !!(fwSelectedFile && fwSelectedFile.size > 0 && !fwUploadInProgress);
    if (btnUp) btnUp.disabled = !canUpload;
    /* Перепрошивка: WS есть, не идёт upload (файл на устройстве желателен, но команда — отдельно) */
    if (btnRf) btnRf.disabled = !wsOk || fwUploadInProgress;
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
    const f = input.files && input.files[0] ? input.files[0] : null;
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
        setFwStatus('файл выбран');
    }
    updateFirmwareButtons();
}

/** POST /firmware с прогрессом (XHR) → ESP пишет куски в UART (firmware_manager) */
function uploadFirmware() {
    if (!fwSelectedFile || fwSelectedFile.size <= 0 || fwUploadInProgress) return;
    fwUploadInProgress = true;
    fwFileReady = false;
    updateFirmwareButtons();
    setFwStatus('отправка на устройство…');
    updateFwProgress(0, fwSelectedFile.size);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/firmware', true);
    xhr.responseType = 'text';
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');

    xhr.upload.onprogress = function (ev) {
        if (ev.lengthComputable) {
            updateFwProgress(ev.loaded, ev.total || fwSelectedFile.size);
        }
    };

    xhr.onload = function () {
        fwUploadInProgress = false;
        if (xhr.status >= 200 && xhr.status < 300) {
            fwFileReady = true;
            setFwStatus('файл отправлен');
            updateFwProgress(fwSelectedFile.size, fwSelectedFile.size);
        } else {
            fwFileReady = false;
            setFwStatus('ошибка HTTP ' + xhr.status);
            alert('Не удалось отправить файл: HTTP ' + xhr.status);
        }
        updateFirmwareButtons();
    };

    xhr.onerror = function () {
        fwUploadInProgress = false;
        fwFileReady = false;
        setFwStatus('ошибка сети');
        alert('Не удалось отправить файл (сеть)');
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

function startReflash() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        alert('Нет соединения WebSocket');
        return;
    }
    if (fwUploadInProgress) {
        alert('Дождитесь окончания отправки файла');
        return;
    }
    const hint = fwFileReady
        ? 'Файл уже отправлен. Запустить перепрошивку?'
        : 'Файл ещё не отправляли на устройство в этой сессии.\nВсё равно отправить команду перепрошивки по UART?';
    if (!confirm(hint)) return;
    try {
        ws.send(JSON.stringify({ action: 'startReflash' }));
        setFwStatus('команда перепрошивки…');
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
