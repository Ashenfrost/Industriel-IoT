#pragma once

/* Builds "device-<mac>" from the chip's eFuse MAC. Returns 0, or a negative
 * errno if no valid MAC could be read: then MQTT must not be started. */
int build_client_id_initialize(void);

/* The ID, or "" if build_client_id_initialize() failed. */
const char *build_client_id_get(void);
