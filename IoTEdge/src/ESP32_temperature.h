// pragma once ensures that the header file is only included once
#pragma once

/* Checks that the internal die temperature sensor is ready. 0 = ok. */
int ESP32_temperature_initialize(void);

/* One reading in degrees C. 0 = ok, negative errno on failure. */
int ESP32_temperature_read(float *celsius);
