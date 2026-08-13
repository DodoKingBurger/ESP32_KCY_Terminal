// ======================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ======================================================================
// Добавить переменные в начало
let lastProgressUpdate = 0;
const PROGRESS_UPDATE_INTERVAL = 100; // мс
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
/** @type {number|null} Идентификатор requestAnimationFrame для прогресса */
let progressRaf = null;

// ===== ФЛАГИ ДЛЯ ЗАЩИТЫ ОТ ПОВТОРОВ =====
let downloadController = null;
let httpDownloadInProgress = false;
let downloadStarted = false;
let isFetching = false;
let pendingStartDownload = false;
let isProcessing = false;
let startDownloadRequested = false; // Флаг: запрос на старт отправлен, ждем подтверждения

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

    if (downloadInProgress && received > 0 && startTime) {
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

// В script.js, добавить проверку соединения перед каждой отправкой
function isConnectionHealthy() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        return false;
    }
    return true;
}

// ======================================================================
// СКАЧИВАНИЕ ФАЙЛА
// ======================================================================

/**
 * @brief Скачивает файл через fetch и отображает прогресс
 * @param {string} url - URL для скачивания
 */
async function triggerBrowserDownload(url) {
    if (isFetching) {
        console.warn("⚠️ Fetch already in progress, ignoring duplicate call");
        return;
    }
    
    if (downloadController) {
        console.warn("⚠️ Aborting previous download");
        downloadController.abort();
        downloadController = null;
    }
    
    isFetching = true;
    httpDownloadInProgress = true;
    
    let chunks = [];
    let received = 0;
    let chunkCount = 0;
    let firstChunkReceived = false;
    try {
        console.log("📥 Starting HTTP fetch:", url);
        downloadController = new AbortController();

        
        const res = await fetch(url, { 
            cache: "no-store",
            signal: downloadController.signal,
            headers: {
                'Connection': 'keep-alive'
            }
        });
        
        console.log("📥 HTTP response:", res.status, res.statusText);
        console.log("📥 Headers:", [...res.headers.entries()]);
        
        if (!res.ok) {
            throw new Error("HTTP " + res.status);
        }
        
        if (!res.body) {
            console.warn("⚠️ No response body");
            throw new Error("No response body");
        }
        
        const reader = res.body.getReader();
        
        const contentLength = res.headers.get("Content-Length");
        if (contentLength) {
            archiveSize = parseInt(contentLength, 10);
            totalBytesSpan.innerText = archiveSize;
            console.log("📥 Content-Length:", archiveSize);
        }
        
        console.log("📥 Starting to read chunks...");
        
        while (true) {
            const { done, value } = await reader.read();
            
            if (done) {
                console.log("📥 Reader done, total chunks:", chunkCount);
                break;
            }
            
            if (!firstChunkReceived) {
                firstChunkReceived = true;
                console.log("📥 First chunk received!");
            }
            
            chunkCount++;
            chunks.push(value);
            received += value.byteLength;
            
            console.log(`📥 Chunk ${chunkCount}: ${value.byteLength} bytes, total: ${received}`);
            const now = Date.now();
            if (now - lastProgressUpdate > PROGRESS_UPDATE_INTERVAL) {
                lastProgressUpdate = now;           
                if (archiveSize > 0) {
                    const percent = (received / archiveSize * 100);
                    progressBar.style.width = Math.min(percent, 100) + '%';
                    progressText.innerText = Math.round(Math.min(percent, 100)) + '%';
                    receivedBytesSpan.innerText = received;
                    
                    if (startTime) {
                        const elapsed = Date.now() - startTime;
                        const elapsedSec = elapsed / 1000;
                        if (elapsedSec > 0) {
                            const speed = received / elapsedSec;
                            speedSpan.innerText = Math.round(speed) + " байт/с";
                            elapsedSpan.innerText = formatTime(elapsed);
                            
                            if (archiveSize > received && speed > 0) {
                                const remaining = (archiveSize - received) / speed;
                                etaSpan.innerText = formatTime(remaining * 1000);
                            }
                        }
                    }
                }
            }
            //if (chunkCount % 10 === 0 && !isConnectionHealthy()) {
                //console.warn("⚠️ WebSocket disconnected during download");
            // Сохраняем прогресс и пробуем переподключиться
            //}
        }
        
        console.log("✅ Download complete, received:", received, "bytes");
        
        if (archiveSize > 0 && received < archiveSize) {
            console.warn("⚠️ WARNING: Incomplete download! Received " + received + "/" + archiveSize + " bytes");
        }
        
        if (chunks.length > 0 && received > 0) {
            console.log("📦 Creating blob from", chunks.length, "chunks");
            const blob = new Blob(chunks);
            const objUrl = URL.createObjectURL(blob);
            const a = document.createElement("a");
            a.href = objUrl;
            a.download = fileName || "archive.bin";
            document.body.appendChild(a);
            a.click();
            a.remove();
            setTimeout(() => URL.revokeObjectURL(objUrl), 2000);
        } else {
            console.warn("⚠️ No data received, skipping file creation");
        }
        
    } catch (e) {
        if (e.name === 'AbortError') {
            console.log("⏹️ Download aborted by user or timeout");
            if (chunks.length > 0 && received > 0) {
                console.log("📦 Saving partial download, received:", received, "bytes");
                const blob = new Blob(chunks);
                const objUrl = URL.createObjectURL(blob);
                const a = document.createElement("a");
                a.href = objUrl;
                a.download = fileName || "partial_archive.bin";
                document.body.appendChild(a);
                a.click();
                a.remove();
                setTimeout(() => URL.revokeObjectURL(objUrl), 2000);
            }
        } else {
            console.error("❌ Download error:", e);
            console.error("❌ Error stack:", e.stack);
        }
    } finally {
        httpDownloadInProgress = false;
        downloadController = null;
        isFetching = false;
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
    
    if (downloadStarted && downloadInProgress) {
        console.log("⏳ Download already started, ignoring");
        return;
    }
    
    if (isFetching) {
        console.log("⏳ Fetch already in progress, ignoring");
        return;
    }
    
    if (archiveSize <= 0) {
        if (terminalActive) term.writeln("\x1b[33mРазмер архива неизвестен. Подождите обновления.\x1b[0m");
        return;
    }
    
    console.log("🚀 Starting download, archiveSize:", archiveSize);
    
    stopArchiveSizePolling();
    totalReceived = 0;
    downloadInProgress = true;
    downloadStarted = true;
    startTime = Date.now();
    fileName = '';
    startDownloadRequested = true; // Ждем подтверждения
    
    btnDownload.disabled = true;
    btnStop.disabled = false;
    updateProgress(0);
    
    if (downloadInterval) clearInterval(downloadInterval);
    downloadInterval = setInterval(() => {
        if (downloadInProgress && startTime) {
            elapsedSpan.innerText = formatTime(Date.now() - startTime);
        }
    }, 1000);
    
    // Отправляем команду на сервер
    ws.send(JSON.stringify({ action: "startDownload" }));
    console.log("📤 Sent startDownload command to server");
}

/**
 * @brief Отправляет команду остановки загрузки архива
 */
function stopDownload() {
    console.log("⏹️ Stop download requested");
    downloadStarted = false;
    startDownloadRequested = false;
    
    // Останавливаем HTTP-запрос
    if (downloadController) {
        downloadController.abort();
        downloadController = null;
        httpDownloadInProgress = false;
        isFetching = false;
    }
    
    // Отправляем команду остановки на сервер
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ action: "stopDownload" }));
        if (terminalActive) term.writeln("\x1b[33mОстановка запрошена\x1b[0m");
    }
    
    // Сбрасываем состояние
    downloadInProgress = false;
    btnDownload.disabled = false;
    btnStop.disabled = true;
    
    if (downloadInterval) {
        clearInterval(downloadInterval);
        downloadInterval = null;
    }
    
    // Сбрасываем прогресс
    progressBar.style.width = '0%';
    progressText.innerText = '0%';
    receivedBytesSpan.innerText = '0';
    speedSpan.innerText = '0 байт/с';
    elapsedSpan.innerText = '00:00';
    etaSpan.innerText = '—';
    
    startArchiveSizePolling();
}

// ======================================================================
// ПАРСИНГ СПЕЦИАЛЬНЫХ ПОСЛЕДОВАТЕЛЬНОСТЕЙ
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
// ОТПРАВКА КОМАНД
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
// WEBSOCKET СОЕДИНЕНИЕ
// ======================================================================

function connectWebSocket() {
    if (ws) ws.close();
    ws = new WebSocket(`ws://${location.host}/ws`);
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
        console.log("🔗 WebSocket connected");
        pingTimer = setInterval(() => {
            if (ws.readyState === WebSocket.OPEN) ws.send("ping");
        }, 15000);
        document.getElementById("dot").classList.add("connected");
        document.getElementById("status").innerText = "Connected";

        setTimeout(() => {
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
        }, 500);
    };

    ws.onclose = () => {
        clearInterval(pingTimer);
        clearInterval(downloadInterval);
        clearInterval(archiveSizeTimer);
        document.getElementById("dot").classList.remove("connected");
        document.getElementById("status").innerText = "Disconnected";
        downloadInProgress = false;
        downloadStarted = false;
        isFetching = false;
        startDownloadRequested = false;
        btnDownload.disabled = false;
        btnStop.disabled = true;
        setTimeout(connectWebSocket, 2500);
    };

    ws.onerror = (err) => {
        console.error('❌ WS error:', err);
    };

    ws.onmessage = (event) => {
        // Обработка бинарных данных
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

        // Обработка текстовых сообщений
        if (typeof event.data === 'string') {
            let msg;
            try {
                msg = JSON.parse(event.data);
            } catch (e) {
                console.log('Non-JSON string:', event.data);
                return;
            }

            console.log("📨 WS message:", msg.type, msg);

            switch (true){
                // ===== ОБРАБОТКА ЛОГА "Download start requested" =====
                case (msg.type === "log" && msg.msg === "Download start requested"): {
                    // Сервер подтвердил запрос на старт - теперь запускаем HTTP-запрос
                    if (startDownloadRequested && !isFetching && !httpDownloadInProgress) {
                        console.log("🚀 Server confirmed, starting HTTP fetch...");
                        // Запускаем HTTP-загрузку
                        triggerBrowserDownload('/download');
                    } else {
                        console.log("ℹ️ Download start confirmed, but already in progress");
                    }
                    break;
                }

                // ===== ОБРАБОТКА startDownload (если сервер все-таки присылает) =====
                case (msg.type === "startDownload" || msg.type === "startHttpDownload"): {
                    if (isFetching || httpDownloadInProgress) {
                        console.warn("⚠️ Ignoring startDownload - already in progress");
                        break;
                    }
                    
                    if (downloadInProgress) {
                        console.warn("⚠️ Ignoring startDownload - download already in progress");
                        break;
                    }
                    
                    const url = msg.url || '/download';
                    
                    downloadInProgress = true;
                    startTime = Date.now();
                    btnDownload.disabled = true;
                    btnStop.disabled = false;
                    totalReceived = 0;
                    updateProgress(0);
                    
                    triggerBrowserDownload(url);
                    break;
                }
                
                // ===== ОСТАЛЬНЫЕ ОБРАБОТЧИКИ =====
                case (msg.type === "log"): {
                    console.log("[LOG]", msg.msg);
                    break;
                }
                
                case (msg.type === "archiveSize"): {
                    archiveSize = msg.size;
                    archiveSizeSpan.innerText = archiveSize + " байт";
                    totalBytesSpan.innerText  = archiveSize;
                    if (!downloadInProgress) updateProgress(0);
                    break;
                }
                
                case (msg.type === "fileName"): {
                    fileName = msg.name || '';
                    break;
                }
                
                case (msg.type === "progress"): {
                    scheduleProgressUpdate(msg.received, msg.total);
                    break;
                }
                
                case (msg.type === "downloadComplete" || msg.type === "downloadStopped"): {
                    downloadStarted = false;
                    downloadInProgress = false;
                    httpDownloadInProgress = false;
                    downloadController = null;
                    isFetching = false;
                    startDownloadRequested = false;
                    
                    clearInterval(downloadInterval);
                    btnDownload.disabled = false;
                    btnStop.disabled = true;
                    updateProgress(0);
                    startArchiveSizePolling();
                    break;
                }
                
                case (msg.type === "error"): {
                    downloadInProgress = false;
                    isFetching = false;
                    startDownloadRequested = false;
                    clearInterval(downloadInterval);
                    btnDownload.disabled = false;
                    btnStop.disabled = true;
                    const errMsg = "Ошибка: " + (msg.msg || '');
                    if (terminalActive) term.writeln("\x1b[31m" + errMsg + "\x1b[0m");
                    console.error(errMsg);
                    break;
                }

                default:{
                    console.error('❌ Unknown JSON:', msg);
                    break;
                }
            }
        }
    };
}

// ======================================================================
// ОБРАБОТЧИК КЛАВИАТУРЫ
// ======================================================================

document.addEventListener("keydown", e => {
    console.log("keydown event:", e.key);
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
    e.preventDefault();
});

// ======================================================================
// ЗАПУСК ПРИ ЗАГРУЗКЕ СТРАНИЦЫ
// ======================================================================

window.addEventListener('resize', resizeTerminal);
window.addEventListener('beforeunload', function() {
    if (ws && ws.readyState === WebSocket.OPEN && downloadInProgress) {
        ws.send(JSON.stringify({ action: "stopDownload" }));
    }
});
const ro = new ResizeObserver(resizeTerminal);
ro.observe(container);
setTimeout(resizeTerminal, 100);