/* bdseq_store.h - keeps the Sparkplug bdSeq in flash across reboots.
 *
 * IEC 62443-4-2 CR 3.8 (session integrity): every MQTT session must get a
 * new bdSeq, also after a reboot or deep sleep (both clear RAM). Otherwise
 * the host can't tell a new session from an old one replayed.
 *
 * Stored with Zephyr's settings subsystem on NVS in the "storage" flash
 * partition (see prj.conf).
 */
#pragma once

#include <stdint.h>

/* Reads the last saved bdSeq into *out. Returns 0, -ENOENT if nothing has
 * been saved yet (new device), or another negative errno. */
int bdseq_store_load(uint8_t *out);

/* Saves bdSeq. Returns 0 or a negative errno. */
int bdseq_store_save(uint8_t bd_seq);
