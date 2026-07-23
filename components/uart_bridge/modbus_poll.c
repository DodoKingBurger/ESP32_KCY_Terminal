#include "modbus_master.h"
#include "web_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "archive_manager.h"  // <-- ДОБАВИТЬ

#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "MODBUS_POLL";

extern bool download_in_progress;
extern bool load_page_active;

static const char *get_start_reason(uint8_t code)
{
    switch (code)
    {
        case 0:  return "Ручной запуск";
        case 1:  return "Автозапуск по таймеру";
        case 2:  return "АПВ после подачи питания";
        case 3:  return "АПВ после защиты";
        case 4:  return "Дистанционный запуск";
        case 5:  return "Запуск от входа 1";
        case 6:  return "Запуск от входа 2";
        case 7:  return "Запуск от входа 3";
        case 8:  return "Запуск от входа 4";
        case 9:  return "Запуск от входа 5";
        case 10: return "Запуск от входа 6";
        case 11: return "АПВ после нормализации";
        default: return "Резерв";
    }
}

/*
====================================================
STOP REASONS
====================================================
*/
static const char *get_stop_reason(uint16_t code)
{
    switch (code)
    {
        case 0: return "НЕТ АВАРИЙ";
        case 1: return "Пониженное сопротивление изоляции";
        case 2: return "Напряжение Uab меньше нормы";
        case 3: return "Напряжение Ubc меньше нормы";
        case 4: return "Напряжение Uca меньше нормы";
        case 5: return "Напряжение Uab больше нормы";
        case 6: return "Напряжение Ubc больше нормы";
        case 7: return "Напряжение Uca больше нормы";
        case 8: return "Дисбаланс Uab-Ubc";
        case 9: return "Дисбаланс Uab-Uca";
        case 10: return "Дисбаланс Ubc-Uca";
        case 11: return "Недогруз ЗСП";
        case 12: return "Перегруз ЗП";
        case 13: return "Дисбаланс Ia-Ib";
        case 14: return "Дисбаланс Ia-Ic";
        case 15: return "Дисбаланс Ib-Ic";
        case 16: return "Пониженная загрузка ПЭД";
        case 17: return "Частота вращения выше нормы";
        case 18: return "Открыта дверь шкафа";
        case 19: return "Неправильное чередование фаз";
        case 20: return "Недогруз ЗСП";
        case 21: return "Недогруз ЗСП";
        case 22: return "Открыта дверь шкафа";
        case 23: return "Сработал ЭКМ";
        case 24: return "Сработал ЭКМ";
        case 25: return "Температура жидкости выше нормы";
        case 26: return "Температура масла выше нормы";
        case 27: return "Вибрация X выше нормы";
        case 28: return "Вибрация Y выше нормы";
        case 29: return "Давление жидкости ниже нормы";
        case 30: return "Давление масла ниже нормы";
        case 31: return "Нет связи с ТМС";
        case 33: return "Перегруз ЗП";
        case 34: return "Перегруз ЗП";
        case 35: return "Вибрация XY";
        case 36: return "Вибрация Z";
        case 37: return "Неисправность контроллера";
        case 38: return "Температура обмотки ПЭД";
        case 40: return "Температура радиатора ПЧ";
        case 41: return "Аппаратный перегрев радиатора";
        case 42: return "Udc выше нормы";
        case 43: return "Udc ниже нормы";
        case 44: return "Высокий ток ПЧ";
        case 45: return "Высокий ток модулей ПЧ";
        case 46: return "Защита ПЧ код 7";
        case 47: return "Защита ПЧ код 8";
        case 48: return "Повреждение цепи заряда";
        case 49: return "Ошибка датчика температуры";
        case 50: return "Обрыв питания ПЧ";
        case 51: return "Температура радиатора ниже нормы";
        case 52: return "Дисбаланс токов ПЧ";
        case 53: return "Недогруз ПЧ";
        case 54: return "Перегрев синус-фильтра";
        case 55: return "Внешняя авария ПЧ";
        case 56: return "Аппаратное превышение Udc";
        case 57: return "Перегруз ПЧ";
        case 58: return "Нет пульта ПЧ";
        case 59: return "Нет связи RS485";
        case 60: return "Нестабильное Udc";
        case 61: return "Частота выше допустимой";
        case 62: return "Высокий ток драйвера";
        case 63: return "Нет связи с платой ПЧ";
        case 64: return "Обрыв питания ПЧ";
        case 65: return "Прогрев СУ";
        case 66: return "ПЧ не готов";
        case 67: return "Обрыв аналогового входа";
        case 68: return "Сбой программы 10кГц";
        case 69: return "Сбой программы 40кГц";
        case 70: return "Сбой программы 1кГц";
        case 71: return "Аппаратная авария ПЧ";
        case 72: return "Сбой ПЧ код 17";
        case 73: return "Сбой ПЧ код 18";
        case 74: return "Сбой ПЧ код 19";
        case 75: return "Обрыв фазы Ia";
        case 76: return "Обрыв фазы Ib";
        case 77: return "Обрыв фазы Ic";
        case 78: return "Нет фазы питания";
        case 79: return "Высокое Udc";
        case 80: return "Авария инвертора";
        case 81: return "Авария сливного ключа";
        case 82: return "Авария пульта";
        case 83: return "Ток фазы A";
        case 84: return "Ток фазы B";
        case 85: return "Ток фазы C";
        case 86: return "Неизвестный тип ПЧ";
        case 87: return "Ошибка ШИМ";
        case 88: return "Авария модуля A";
        case 89: return "Авария модуля B";
        case 90: return "Авария модуля C";
        case 91: return "Перегрев модуля A";
        case 92: return "Перегрев модуля B";
        case 93: return "Внешняя авария";
        case 94: return "Останов DI1";
        case 95: return "Останов DI2";
        case 96: return "Останов DI3";
        case 97: return "Останов DI4";
        case 98: return "Останов DI5";
        case 99: return "Останов DI6";
        case 102:return "Перегрев модуля C";
        case 103:return "Стоп оператор";
        case 104:return "Стоп оператор";
        case 105:return "Нет подтверждения запуска";
        case 106:return "Автостоп по программе";
        case 107:return "Обрыв питания";
        case 112:return "SCADA стоп";
        case 115:return "Стоп оператор";
        case 116:return "Срыв подачи";
        case 117:return "Автостоп";
        case 119:return "Ошибка платы IO";
        case 120:return "Сбой контроллера ПЧ";
        case 121:return "Ошибка платы МКТН2";
        case 122:return "Ошибка платы МКИ";
        case 123:return "Ошибка платы МКТН1";
        case 124:return "Авария тормозного ключа";
        case 126:return "Высокий ток A";
        case 127:return "Высокий ток B";
        case 128:return "AI1 выше допуска";
        case 129:return "AI1 ниже допуска";
        case 130:return "AI2 выше допуска";
        case 131:return "AI2 ниже допуска";
        case 132:return "SCADA стоп";
        case 133:return "Частота выше уставки";
        case 134:return "Частота ниже уставки";
        case 135:return "Высокий ток C";
        case 136:return "Перегрев стойки A";
        case 137:return "Перегрев стойки B";
        case 138:return "Перегрев стойки C";
        case 139:return "Перегрев двигателя";
        case 140:return "Ошибка положения ротора";
        case 141:return "Обрыв 0-ТМПН";
        case 142:return "Нет блока 0-ТМПН";
        case 147:return "Неизвестная авария";
        case 193:return "Минимальное давление";
        case 194:return "Максимальное давление";
        case 195:return "Перегрев отсека СУ";
        case 196:return "Перегрев трансформатора";
        case 197:return "Аварийный стоп";
        case 328:return "AI3 выше допуска";
        case 329:return "AI3 ниже допуска";
        case 330:return "AI4 выше допуска";
        case 331:return "AI4 ниже допуска";
        case 332:return "AI5 выше допуска";
        case 333:return "AI5 ниже допуска";
        case 334:return "AI6 выше допуска";
        case 335:return "AI6 ниже допуска";

        default:
            return "Неизвестная авария";
    }
}

/**
 * @brief Основная задача опроса Modbus для телеметрии
 * @param arg не используется
 * @details Выполняет чтение времени (0xFA-0xFC), состояния (0xFF) и параметров (0x101-0x110),
 *          формирует JSON и отправляет через web_server_send().
 */
void modbus_poll_task(void *arg) {
    uint16_t time_regs[3];   // 0x00FA, 0x00FB, 0x00FC
    uint16_t status;          // 0x00FF
    uint16_t params[16];      // 0x0101 … 0x0110 (16 регистров)

    char json[1024];

    while (1) {
        // 1. Чтение времени (3 регистра)
        if (modbus_read_registers(1, 0x00FA, 3, time_regs) != MODBUS_OK) {
            ESP_LOGW(TAG, "Failed to read time registers");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 2. Чтение состояния СУ (1 регистр)
        if (modbus_read_registers(1, 0x00FF, 1, &status) != MODBUS_OK) {
            ESP_LOGW(TAG, "Failed to read status register");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 3. Чтение параметров (16 регистров: 0x0101 … 0x0110)
        if (modbus_read_registers(1, 0x0101, 16, params) != MODBUS_OK) {
            ESP_LOGW(TAG, "Failed to read parameter registers");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // === Распаковка времени ===
        uint8_t month  = (time_regs[0] >> 8) & 0xFF;
        uint8_t day    = time_regs[0] & 0xFF;
        uint8_t hour   = (time_regs[1] >> 8) & 0xFF;
        uint8_t year   = time_regs[1] & 0xFF;
        uint8_t second = (time_regs[2] >> 8) & 0xFF;
        uint8_t minute = time_regs[2] & 0xFF;

        char datetime[32];
        snprintf(datetime, sizeof(datetime), "%02d.%02d.%02d %02d:%02d:%02d",
                 day, month, year, hour, minute, second);

        // === Распаковка параметров (индекс 0 → 0x0101) ===
        uint16_t insulation = params[0];  // кОм
        uint16_t uab        = params[1];  // В
        uint16_t ubc        = params[2];
        uint16_t uac        = params[3];
        uint16_t ia_raw     = params[4];  // 0.1 А
        uint16_t ib_raw     = params[5];
        uint16_t ic_raw     = params[6];
        uint16_t cos_raw    = params[7];  // 0.001
        uint16_t load_raw   = params[8];  // 0.1 %
        uint16_t ua         = params[9];  // В
        uint16_t ub         = params[10];
        uint16_t uc         = params[11];
        int16_t  freq_raw   = (int16_t)params[12]; // 0.1 Гц
        uint16_t vfd_current_raw = params[13];     // 0.1 А
        uint16_t dc_voltage = params[14];          // В
        uint16_t heatsink_temp = params[15];       // °C

        // Преобразование в физические значения
        float ia = ia_raw / 10.0f;
        float ib = ib_raw / 10.0f;
        float ic = ic_raw / 10.0f;
        float cos_phi = cos_raw / 1000.0f;
        float load = load_raw / 10.0f;
        float freq = freq_raw / 10.0f;
        float vfd_current = vfd_current_raw / 10.0f;

        // === Распаковка статуса ===
        bool ped_running = (status >> 15) & 0x01;
        bool start_block = (status >> 14) & 0x01;
        uint8_t mode = (status >> 12) & 0x03;
        uint8_t start_reason = (status >> 8) & 0x0F;
        uint16_t stop_reason = status & 0xFF;

        const char *mode_str = "НЕИЗВЕСТНО";
        switch (mode) {
            case 0: mode_str = "ОТКЛ"; break;
            case 1: mode_str = "РУЧНОЙ"; break;
            case 3: mode_str = "АВТОМАТ"; break;
        }

        // === Формирование JSON ===
        snprintf(json, sizeof(json),
            "{"
            "\"datetime\":\"%s\","
            "\"ped_running\":%d,"
            "\"start_block\":%d,"
            "\"mode\":\"%s\","
            "\"start_reason\":\"%s\","
            "\"stop_reason\":\"%s\","
            "\"insulation\":%u,"
            "\"uab\":%.1f,"
            "\"ubc\":%.1f,"
            "\"uac\":%.1f,"
            "\"ua\":%.1f,"
            "\"ub\":%.1f,"
            "\"uc\":%.1f,"
            "\"ia\":%.1f,"
            "\"ib\":%.1f,"
            "\"ic\":%.1f,"
            "\"vfd_current\":%.1f,"
            "\"cosphi\":%.3f,"
            "\"load\":%.1f,"
            "\"freq\":%.1f,"
            "\"dc_voltage\":%u,"
            "\"heatsink_temp\":%u"
            "}",
            datetime,
            ped_running,
            start_block,
            mode_str,
            get_start_reason(start_reason),
            get_stop_reason(stop_reason),
            insulation,
            (double)uab,
            (double)ubc,
            (double)uac,
            (double)ua,
            (double)ub,
            (double)uc,
            (double)ia,
            (double)ib,
            (double)ic,
            (double)vfd_current,
            (double)cos_phi,
            (double)load,
            (double)freq,
            dc_voltage,
            heatsink_temp
        );

        web_server_send(json);
        ESP_LOGI(TAG, "%s", json);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Задача чтения экрана терминала и отправки через WebSocket
 * @param arg не используется
 * @details Пропускает итерации, если идёт загрузка архива (download_in_progress).
 */
static void terminal_task(void *arg)
{
    uint8_t screen[4096];

    uint16_t screen_len;

    while (1)
    {
        // Если идёт загрузка, пропускаем опрос экрана
        if (load_page_active || download_in_progress) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        bool ok =
            terminal_read_screen(
                1,
                screen,
                &screen_len
            );

        if (ok && screen_len > 0) {
            web_server_send_binary(screen, screen_len);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Запускает задачу terminal_task
 */
void terminal_task_start(void)
{
    xTaskCreate(
        terminal_task,
        "terminal_task",
        16384,
        NULL,
        5,
        NULL
    );
}
