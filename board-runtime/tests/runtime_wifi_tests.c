#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "runtime_wifi.h"

static int failures = 0;
static void check(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", message); failures += 1; }
  else { fprintf(stdout, "ok: %s\n", message); }
}

int main(void) {
  /* SSID happy path */
  check(br_wifi_credential_valid("HomeWifi-2G", 64, false), "plain ASCII SSID");
  check(br_wifi_credential_valid("公司-WiFi", 64, false), "UTF-8 multibyte SSID");
  check(br_wifi_credential_valid("a", 64, false), "single-char SSID");
  /* PSK happy path */
  check(br_wifi_credential_valid("password1234", 64, true), "plain PSK");
  check(br_wifi_credential_valid("", 64, true), "empty PSK allowed for open network");

  /* Length */
  char too_long[80];
  memset(too_long, 'a', 79); too_long[79] = '\0';
  check(!br_wifi_credential_valid(too_long, 64, false), "rejects >64 bytes");
  check(br_wifi_credential_valid("0123456789012345678901234567890123456789012345678901234567890123", 64, false),
        "accepts exactly 64 bytes");

  /* Empty SSID disallowed */
  check(!br_wifi_credential_valid("", 64, false), "rejects empty SSID");
  check(!br_wifi_credential_valid(NULL, 64, false), "rejects NULL");

  /* Control chars */
  check(!br_wifi_credential_valid("line1\nline2", 64, false), "rejects newline");
  check(!br_wifi_credential_valid("with\ttab", 64, false), "rejects tab");
  check(!br_wifi_credential_valid("with\rcr", 64, false), "rejects CR");

  /* Shell metachar per spec */
  check(!br_wifi_credential_valid("ssid;ls", 64, false), "rejects semicolon");
  check(!br_wifi_credential_valid("ssid$VAR", 64, false), "rejects dollar");
  check(!br_wifi_credential_valid("ssid`cmd`", 64, false), "rejects backtick");
  check(!br_wifi_credential_valid("ssid\\esc", 64, false), "rejects backslash");

  /* Allowed shell-special chars (NOT special inside double quotes) */
  check(br_wifi_credential_valid("a|b&c<d>e(f)g", 64, false), "allows pipe/amp/redirect/paren");
  check(br_wifi_credential_valid("My SSID With Spaces", 64, false), "allows spaces");
  check(br_wifi_credential_valid("p@ssw0rd!#%^&*", 64, true), "allows common PSK punctuation");

  if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
  fprintf(stdout, "all tests passed\n");
  return 0;
}
