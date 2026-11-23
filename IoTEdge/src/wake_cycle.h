/* wake_cycle.h - one wake-up of the deep sleep design (APP_DEEP_SLEEP).
 *
 * Every APP_ALARM_CHECK_S the RTC timer wakes the ESP32-C6 from deep sleep,
 * which is a full reboot (RAM is gone). main() then calls wake_cycle_run():
 *
 *   1. Wi-Fi up, read the SimPLC's live values, buffer status and coils
 *   2. alarms: evaluate the limits, write changed coils (buzzer/lamp)
 *   3. send if the oldest buffered sample is APP_SEND_AGE_S old, an alarm
 *      changed, or this is a power-on (not a wake from deep sleep):
 *        read the whole buffer -> MQTT session over the modem ->
 *        set the SimPLC's clock (UTC from the modem's NTP) ->
 *        acknowledge what was sent, so the SimPLC can remove it
 *   4. Wi-Fi down, deep sleep until the next check
 *
 * Nothing has to survive the deep sleep: the SimPLC keeps the samples, the
 * alarm state (its coils) and the clock; bdSeq is in flash (bdseq_store.c).
 */
#pragma once

/* Runs one wake-up and goes to deep sleep. Never returns. */
void wake_cycle_run(void);
