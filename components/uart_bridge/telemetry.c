#include "telemetry.h"

static telemetry_data_t g_data;

void telemetry_update(
    telemetry_data_t *data
)
{
    g_data = *data;
}

telemetry_data_t telemetry_get(void)
{
    return g_data;
}