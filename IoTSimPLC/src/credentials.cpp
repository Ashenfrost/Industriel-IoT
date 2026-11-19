#include "credentials.h"

#include <Arduino.h>
#include <Preferences.h>

static const char* NVS_NAMESPACE = "wifi";
static const char* KEY_SSID      = "ssid";
static const char* KEY_PSK       = "psk";

bool credentialsLoad(char* ssid, size_t ssidLen, char* psk, size_t pskLen) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;

  String s = prefs.getString(KEY_SSID, "");
  String p = prefs.getString(KEY_PSK, "");
  prefs.end();

  if (s.length() == 0 || s.length() >= ssidLen || p.length() >= pskLen) return false;

  snprintf(ssid, ssidLen, "%s", s.c_str());
  snprintf(psk, pskLen, "%s", p.c_str());
  return true;
}

bool credentialsSave(const char* ssid, const char* psk) {
  size_t ssidLen = strlen(ssid);
  size_t pskLen  = strlen(psk);
  if (ssidLen == 0 || ssidLen > 32 || pskLen > 63) return false;

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;

  bool ok = (prefs.putString(KEY_SSID, ssid) == ssidLen);
  if (pskLen > 0) {
    ok = ok && (prefs.putString(KEY_PSK, psk) == pskLen);
  } else {
    prefs.remove(KEY_PSK);   // open network, no password stored
  }
  prefs.end();
  return ok;
}

void credentialsClear(void) {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }
  Serial.println("Stored Wi-Fi settings cleared");
}

// Read one line from the serial monitor. The idle timeout restarts on every key.
// The serial monitor does not echo, so we echo ourselves (or '*' for the password).
static bool readLine(char* buf, size_t size, unsigned long idleTimeoutMs, bool mask) {
  size_t len = 0;
  unsigned long lastKey = millis();

  while (millis() - lastKey < idleTimeoutMs) {
    if (!Serial.available()) {
      delay(10);
      continue;
    }

    lastKey = millis();
    char c = Serial.read();

    if (c == '\r' || c == '\n') {
      // Swallow the second half of a CR+LF pair
      delay(20);
      while (Serial.available() && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
        Serial.read();
      }
      Serial.println();
      buf[len] = '\0';
      return true;
    }

    if ((c == 8 || c == 127) && len > 0) {   // backspace
      len--;
      Serial.print("\b \b");
      continue;
    }

    if (c >= 32 && c < 127 && len < size - 1) {
      buf[len++] = c;
      Serial.print(mask ? '*' : c);
    }
  }

  buf[0] = '\0';
  return false;
}

bool credentialsProvisionSerial(unsigned long timeoutMs) {
  char ssid[CRED_SSID_MAX];
  char psk[CRED_PSK_MAX];

  while (Serial.available()) Serial.read();   // drop anything typed earlier

  Serial.println();
  Serial.println("=== Wi-Fi setup ===");
  Serial.println("Type the value and press Enter.");

  Serial.print("SSID: ");
  if (!readLine(ssid, sizeof ssid, timeoutMs, false) || ssid[0] == '\0') {
    Serial.println("\nSetup cancelled");
    return false;
  }

  Serial.print("Password (empty = open network): ");
  if (!readLine(psk, sizeof psk, timeoutMs, true)) {
    Serial.println("\nSetup cancelled");
    return false;
  }

  if (!credentialsSave(ssid, psk)) {
    Serial.println("Could not save the Wi-Fi settings");
    return false;
  }

  Serial.println("Wi-Fi settings saved");
  return true;
}

bool credentialsResetRequested(unsigned long windowMs) {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  Serial.println("Hold BOOT now to change the Wi-Fi settings...");

  unsigned long start = millis();
  while (millis() - start < windowMs) {
    if (digitalRead(BOOT_PIN) == LOW) {
      delay(50);   // debounce
      if (digitalRead(BOOT_PIN) == LOW) return true;
    }
    delay(10);
  }
  return false;
}
