#pragma once

// ---------- Network ----------
#define HTTP_PORT 80
#define MDNS_NAME "claude-usage"  // -> http://claude-usage.local

// ---------- Timing ----------
#define STALE_AFTER_S 600       // no update for 10 min -> data shown as stale
#define REDRAW_INTERVAL_MS 1000  // countdown refresh

// ---------- Color thresholds (% used) ----------
#define PCT_WARN 50
#define PCT_DANGER 80

// ---------- Board ----------
#define BOOT_BTN 0  // press BOOT to toggle the backlight
#define LED_R 4     // on-board RGB LED, active LOW
#define LED_G 16
#define LED_B 17
