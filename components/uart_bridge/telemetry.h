#pragma once

typedef struct
{
    float voltage;

    float current;

    float temperature;

} telemetry_data_t;


void telemetry_update(
    telemetry_data_t *data
);

telemetry_data_t telemetry_get(void);