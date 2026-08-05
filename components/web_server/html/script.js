// ======================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ======================================================================

/** @type {number} Количество столбцов терминала */
let termCols = 80;
/** @type {number} Количество строк терминала */
let termRows = 24;

/** @type {Terminal} Экземпляр xterm.js */
const term = new Terminal({
    cursorBlink: true,
    cols: termCols,
    rows: termRows,
    convertEol: false,
    scrollback: 0,
    disableStdin: false
});

/** @type {HTMLElement} Контейнер для терминала */
const container = document.getElementById('terminal-container');

// ======================================================================
// ЗАПУСК ПОСЛЕ ЗАГРУЗКИ DOM
// ======================================================================

document.addEventListener('DOMContentLoaded', function() {
    term.open(container);
    resizeTerminal();
    term.writeln('ESP32 Terminal');
    term.writeln('');
    // Остальной код инициализации (WebSocket, resize, etc.)
    connectWebSocket();
    window.addEventListener('resize', resizeTerminal);
    const ro = new ResizeObserver(resizeTerminal);
    ro.observe(container);
    setTimeout(resizeTerminal, 100);
});

/** @type {number|null} Идентификатор таймера ping */
let pingTimer = null;
/** @type {WebSocket|null} Соединение WebSocket */
let ws = null;
/** @type {boolean} Активна ли вкладка терминала */
let terminalActive = true;

// ---------- ПЕРЕМЕННЫЕ ДЛЯ ЗАГРУЗОК ----------
/** @type {number} Размер архива в байтах */
let archiveSize = 0;
/** @type {number} Получено байт при текущей загрузке */
let totalReceived = 0;
/** @type {boolean} Флаг активной загрузки */
let downloadInProgress = false;
/** @type {number|null} Время старта загрузки (Date.now()) */
let startTime = null;
/** @type {number|null} Интервал обновления времени */
let downloadInterval = null;
/** @type {number|null} Интервал опроса размера архива */
let archiveSizeTimer = null;
/** @type {string} Имя файла архива */
let fileName = '';
/** @type {string|null} Последний URL для скачивания */
let lastDownloadUrl = null;
/** @type {number|null} Идентификатор requestAnimationFrame для прогресса */
let progressRaf = null;

// DOM-элементы для страницы загрузок
const archiveSizeSpan   = document.getElementById("archive-size");
const btnDownload       = document.getElementById("btn-download");
const btnStop           = document.getElementById("btn-stop");
const progressBar       = document.getElementById("progress-bar");
const progressText      = document.getElementById("progress-text");
const receivedBytesSpan = document.getElementById("received-bytes");
const totalBytesSpan    = document.getElementById("total-bytes");
const speedSpan         = document.getElementById("speed");
const elapsedSpan       = document.getElementById("elapsed");
const etaSpan           = document.getElementById("eta");

// ======================================================================
// УПРАВЛЕНИЕ РАЗМЕРОМ ТЕРМИНАЛА
// ======================================================================

/**
 * Подбирает оптимальный размер шрифта терминала,
 *        чтобы он занимал всю доступную область контейнера.
 */
function resizeTerminal() {
    const rect = container.getBoundingClientRect();
    const availWidth  = rect.width;
    const availHeight = rect.height;
    const fontByWidth = availWidth / (termCols * 0.62);
    const fontByHeight = availHeight / (termRows * 1.25);
    const fontSize = Math.floor(Math.min(fontByWidth, fontByHeight));
    
    term.options.fontSize = Math.max(8, Math.min(fontSize, 40));
    term.refresh(0, termRows - 1);
}

// ======================================================================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ДЛЯ ЗАГРУЗОК
// ======================================================================

/**
 * @brief Форматирует миллисекунды в строку MM:SS
 * @param {number} ms - время в миллисекундах
 * @returns {string} строка формата MM:SS
 */
function formatTime(ms) {
    if (ms < 0) ms = 0;
    const totalSec = Math.floor(ms / 1000);
    const min = String(Math.floor(totalSec / 60)).padStart(2, "0");
    const sec = String(totalSec % 60).padStart(2, "0");
    return min + ":" + sec;
}

/**
 * @brief Обновляет индикаторы прогресса скачивания
 * @param {number} received - количество полученных байт
 */
function updateProgress(received) {
    const percent = archiveSize > 0 ? (received / archiveSize * 100) : 0;
    progressBar.style.width = Math.min(percent, 100) + '%';
    progressText.innerText  = Math.round(Math.min(percent, 100)) + '%';
    receivedBytesSpan.innerText = received;

    if (downloadInProgress && received > 0) {
        const elapsed    = Date.now() - startTime;
        const elapsedSec = elapsed / 1000;
        const speed      = received / elapsedSec;
        speedSpan.innerText   = Math.round(speed) + " байт/с";
        elapsedSpan.innerText = formatTime(elapsed);
        if (speed > 0 && archiveSize > received) {
            const remaining = (archiveSize - received) / speed;
            etaSpan.innerText = formatTime(remaining * 1000);
        } else {
            etaSpan.innerText = '—';
        }
    } else {
        const remaining = 0;
        speedSpan.innerText = "0 байт/с";
        etaSpan.innerText   = "—";
    }
}

/**
 * @brief Планирует обновление прогресса через requestAnimationFrame
 * @param {number} received - получено байт
 * @param {number} [total] - общий размер (если известен)
 */
function scheduleProgressUpdate(received, total) {
    if (total) archiveSize = total;
    if (progressRaf) return;
    progressRaf = requestAnimationFrame(() => {
        progressRaf = null;
        totalReceived = received;
        updateProgress(totalReceived);
    });
}

/**
 * @brief Скачивает файл через fetch и отображает прогресс
 * @param {string} url - URL для скачивания
 * @param {string} filename - имя сохраняемого файла
 */
async function triggerBrowserDownload(url, filename) {
    try {
        const res = await fetch(url, { cache: "no-store" });
        if (!res.ok) throw new Error("HTTP " + res.status);
        const total   = parseInt(res.headers.get("Content-Length") || "0", 10);
        const reader  = res.body.getReader();
        const chunks  = [];
        let received  = 0;
        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            chunks.push(value);
            received += value.byteLength;
            scheduleProgressUpdate(received, total || archiveSize);
        }
        const blob  = new Blob(chunks);
        const objUrl= URL.createObjectURL(blob);
        const a     = document.createElement("a");
        a.href = objUrl;
        a.download = filename || "archive.bin";
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(() => URL.revokeObjectURL(objUrl), 2000);
        lastDownloadUrl = url;
    } catch (e) {
        if (terminalActive) term.writeln("\x1b[31mНе удалось скачать файл: " + e.message + "\x1b[0m");
        console.error("Download error:", e);
    }
}
// ======================================================================
// ОПРОС РАЗМЕРА АРХИВА
// ======================================================================

/**
 * @brief Запрашивает у сервера текущий размер архива через WebSocket
 */
function requestArchiveSize() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        archiveSizeSpan.innerText = "Нет соединения";
        return;
    }
    ws.send(JSON.stringify({ action: "getArchiveSize" }));
}

/**
 * @brief Запускает периодический опрос размера архива (каждые 5 сек)
 */
function startArchiveSizePolling() {
    if (!archiveSizeTimer && document.getElementById("downloads-page").classList.contains("active") && !downloadInProgress) {
        requestArchiveSize();
        archiveSizeTimer = setInterval(requestArchiveSize, 5000);
    }
}

/**
 * @brief Останавливает периодический опрос размера архива
 */
function stopArchiveSizePolling() {
    if (archiveSizeTimer) { clearInterval(archiveSizeTimer); archiveSizeTimer = null; }
}

// ======================================================================
// УПРАВЛЕНИЕ ЗАГРУЗКОЙ АРХИВА
// ======================================================================

/**
 * @brief Запускает процесс скачивания архива
 *        Проверяет соединение, размер архива, отправляет команду старта
 */
function startDownload() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        if (terminalActive) term.writeln("\x1b[31mНет соединения с сервером\x1b[0m");
        return;
    }
    if (downloadInProgress) return;
    if (archiveSize <= 0) {
        if (terminalActive) term.writeln("\x1b[33mРазмер архива неизвестен. Подождите обновления.\x1b[0m");
        return;
    }
    stopArchiveSizePolling();
    totalReceived     = 0;
    downloadInProgress= true;
    startTime         = Date.now();
    fileName          = '';
    btnDownload.disabled = true;
    btnStop.disabled     = false;
    updateProgress(0);

    ws.send(JSON.stringify({ action: "startDownload" }));

    if (downloadInterval) clearInterval(downloadInterval);
    downloadInterval = setInterval(() => {
        if (downloadInProgress) {
            elapsedSpan.innerText = formatTime(Date.now() - startTime);
        } else {
            clearInterval(downloadInterval);
        }
    }, 1000);
}

/**
 * @brief Отправляет команду остановки загрузки архива
 */
function stopDownload() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    if (!downloadInProgress) return;
    ws.send(JSON.stringify({ action: "stopDownload" }));
    if (terminalActive) term.writeln("\x1b[33mОстановка запрошена\x1b[0m");
}

// ======================================================================
// ПАРСИНГ СПЕЦИАЛЬНЫХ ПОСЛЕДОВАТЕЛЬНОСТЕЙ ТЕРМИНАЛА
// ======================================================================

/**
 * @brief Извлекает размер терминала из escape-последовательности \x1B[8;rows;colst
 * @param {string} text - текст, полученный от сервера
 */
function parseTerminalSize(text) {
    const match = text.match(/\x1B\[8;(\d+);(\d+)t/);
    if (!match) return;
    const newRows = parseInt(match[2]);
    const newCols = parseInt(match[1]);
    if (newRows !== termRows || newCols !== termCols) {
        termRows = newRows;
        termCols = newCols;
        term.resize(termCols, termRows);
        setTimeout(resizeTerminal, 0);
    }
}

/**
 * @brief Извлекает состояние ламп из escape-последовательности \x1B[<state>q
 * @param {string} text - текст, полученный от сервера
 */
function parseLampState(text) {
    const match = text.match(/\x1B\[(\d+)q/);
    if (!match) return;
    const state = parseInt(match[1]);
    document.getElementById("led-stop").classList.toggle("active-stop", (state & 0x01) !== 0);
    document.getElementById("led-wait").classList.toggle("active-wait", (state & 0x02) !== 0);
    document.getElementById("led-run").classList.toggle("active-run", (state & 0x04) !== 0);
}

// ======================================================================
// ОТПРАВКА КОМАНД НА СЕРВЕР
// ======================================================================

/**
 * @brief Отправляет команду клавиши или действия через WebSocket
 * @param {string} key - идентификатор команды (f1, up, start, и т.д.)
 */
function sendKey(key) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        term.writeln('\r\n[Не подключено]');
        return;
    }
    ws.send(key);
}

// ======================================================================
// ПЕРЕКЛЮЧЕНИЕ ВКЛАДОК
// ======================================================================

/**
 * @brief Переключает активную вкладку (терминал/загрузки)
 * @param {string} name - имя вкладки ('terminal' или 'downloads')
 */
function showTab(name) {
    document.querySelectorAll(".page").forEach(x => x.classList.remove("active"));
    document.querySelectorAll(".tab").forEach(x => x.classList.remove("active"));

    if (name === "terminal") {
        document.getElementById("terminal-page").classList.add("active");
        document.querySelectorAll(".tab")[0].classList.add("active");
        terminalActive = true;
        resizeTerminal();
        stopArchiveSizePolling();
        if (ws && ws.readyState === WebSocket.OPEN) 
            ws.send(JSON.stringify({ action: "setTerminalActive", active: false }));
    } else {
        document.getElementById("downloads-page").classList.add("active");
        document.querySelectorAll(".tab")[1].classList.add("active");
        terminalActive = false;
        startArchiveSizePolling();
        if (ws && ws.readyState === WebSocket.OPEN) 
            ws.send(JSON.stringify({ action: "setTerminalActive", active: true }));
    }
}

// ======================================================================
// УСТАНОВКА WEBSOCKET-СОЕДИНЕНИЯ
// ======================================================================

/**
 * @brief Устанавливает WebSocket-соединение с сервером и настраивает обработчики
 */
function connectWebSocket() {
    if (ws) ws.close(); // закрываем старое, если есть
    ws = new WebSocket(`ws://${location.host}/ws`);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
        console.log("WebSocket connected");
        pingTimer = setInterval(() => {
            if (ws.readyState === WebSocket.OPEN) ws.send("ping");
        }, 5000);
        document.getElementById("dot").classList.add("connected");
        document.getElementById("status").innerText = "Connected";

        // Отправка времени — с небольшой задержкой
        setTimeout(() => {
            //requestArchiveSize();
            const now = new Date();
            const timeMsg = JSON.stringify({
                action: "setTime",
                year: now.getFullYear(),
                month: now.getMonth() + 1,
                day: now.getDate(),
                hour: now.getHours(),
                minute: now.getMinutes(),
                second: now.getSeconds()
            });
            if (ws.readyState === WebSocket.OPEN) {
                ws.send(timeMsg);
            }
        }, 500); // небольшая задержка
    };

    ws.onclose = () => {
        clearInterval(pingTimer);
        clearInterval(downloadInterval);
        clearInterval(archiveSizeTimer);
        document.getElementById("dot").classList.remove("connected");
        document.getElementById("status").innerText = "Disconnected";
        downloadInProgress = false;
        btnDownload.disabled = false;
        btnStop.disabled = true;
        setTimeout(connectWebSocket, 2500);
    };

    ws.onerror = (err) => {
        console.error('WS error:', err);
    };

    ws.onmessage = (event) => {
        console.log("WS message:", event.data); 
        // Обработка бинарных данных (ANSI-экран)
        if (event.data instanceof ArrayBuffer) {
            const bytes = new Uint8Array(event.data);
            const filtered = bytes.map(b => {
                if (b === 0x81) return 0x76;
                if (b === 0x7F) return 0x30;
                if (b === 0x80) return 0x5E;
                return b;
            }).filter(b => b !== 0x00);

            const decoder = new TextDecoder("windows-1251");
            let text = decoder.decode(filtered);

            parseTerminalSize(text);
            parseLampState(text);

            text = text.replace(/\x00/g, '');
            text = text.replace(/\x1B\[\d+;\d+H/g, '');

            term.write('\x1b[2J\x1b[H');
            term.write(text);
            return;
        }

        // Обработка текстовых сообщений (JSON)
        if (typeof event.data === 'string') {
            let msg;
            try {
                msg = JSON.parse(event.data);
            } catch (e) {
                console.log('Non-JSON string:', event.data);
                return;
            }

            if (msg.type === "log") {
                console.log("[LOG]", msg.msg);
                return;
            }
            if (msg.type === "archiveSize") {
                archiveSize = msg.size;
                archiveSizeSpan.innerText = archiveSize + " байт";
                totalBytesSpan.innerText  = archiveSize;
                if (!downloadInProgress) updateProgress(0);
                return;
            }
            if (msg.type === "fileName") {
                fileName = msg.name || '';
                return;
            }
            if (msg.type === "progress") {
                scheduleProgressUpdate(msg.received, msg.total);
                return;
            }
            if (msg.type === "downloadComplete") {
                downloadInProgress = false;
                clearInterval(downloadInterval);
                btnDownload.disabled = false;
                btnStop.disabled     = true;
                updateProgress(0);
                triggerBrowserDownload(msg.url || '/download', msg.fileName || fileName || 'archive.bin');
                startArchiveSizePolling();
                return;
            }
            if (msg.type === "downloadStopped") {
                downloadInProgress = false;
                clearInterval(downloadInterval);
                btnDownload.disabled = false;
                btnStop.disabled     = true;
                updateProgress(msg.received || totalReceived);
                if (msg.url) {
                    const a = document.createElement("a");
                    a.href = msg.url;
                    a.download = msg.fileName || "archive_part.bin";
                    document.body.appendChild(a);
                    a.click();
                    a.remove();
                    console.log("Скачана неполная версия архива.");
                }
                startArchiveSizePolling();
                return;
            }
            if (msg.type === "error") {
                downloadInProgress = false;
                clearInterval(downloadInterval);
                btnDownload.disabled = false;
                btnStop.disabled     = true;
                const errMsg = "Ошибка: " + (msg.msg || '');
                if (terminalActive) term.writeln("\x1b[31m" + errMsg + "\x1b[0m");
                console.error(errMsg);
                return;
            }
            console.error('Unknown JSON:', msg);
        }
    };
}

// ======================================================================
// ОБРАБОТЧИК КЛАВИАТУРЫ
// ======================================================================

document.addEventListener("keydown", e => {
    console.log("keydown event:", e.key);  // <-- ДОБАВЬТЕ
    if (!terminalActive) return;
    switch (e.key) {
        case "1": sendKey("f1"); break;
        case "2": sendKey("f2"); break;
        case "3": sendKey("f3"); break;
        case "Escape": sendKey("esc"); break;
        case "Enter": sendKey("enter"); break;
        case "q": case "Q": sendKey("esc"); break;
        case "e": case "E": sendKey("enter"); break;
        case "w": case "W": case "ArrowUp": sendKey("up"); break;
        case "s": case "S": case "ArrowDown": sendKey("down"); break;
        case "a": case "A": case "ArrowLeft": sendKey("left"); break;
        case "d": case "D": case "ArrowRight": sendKey("right"); break;
        case "5": sendKey("start"); break;
        case "6": sendKey("stop"); break;
        default: return;
    }
    e.preventDefault(); // предотвращаем стандартное поведение
});

// ======================================================================
// ЗАПУСК ПРИ ЗАГРУЗКЕ СТРАНИЦЫ
// ======================================================================

//connectWebSocket();
window.addEventListener('resize', resizeTerminal);
window.addEventListener('beforeunload', function() {
    if (ws && ws.readyState === WebSocket.OPEN && downloadInProgress) {
        ws.send(JSON.stringify({ action: "stopDownload" }));
    }
});
const ro = new ResizeObserver(resizeTerminal);
ro.observe(container);
setTimeout(resizeTerminal, 100);