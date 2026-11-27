#!/usr/bin/env node
// Claude Code status line -> ESP32 CYD usage screen.
// Claude Code pipes session JSON (incl. rate_limits) to stdin on every status line refresh.
// This script prints a one-line status for the terminal and sends the usage to the ESP32.
// The HTTP send runs in a detached child process, so the status line never waits on the network.
//
//   node statusline.mjs           normal mode (used by Claude Code statusLine)
//   node statusline.mjs --test    send fake data in the foreground and print the result

import { spawn } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const SELF = fileURLToPath(import.meta.url);
const CACHE = join(tmpdir(), "claude-cyd-usage-cache.json");
const RESEND_MS = 60_000; // resend unchanged data once a minute (keeps "Updated" on the screen fresh)
const OFFLINE_BACKOFF_MS = 30_000; // after a failed send, wait before trying again
const TIMEOUT_MS = 3000;

function loadConfig() {
  try {
    return JSON.parse(readFileSync(join(dirname(SELF), "config.json"), "utf8"));
  } catch {
    return null;
  }
}

function readJson(file, fallback) {
  try {
    return JSON.parse(readFileSync(file, "utf8") || "null") ?? fallback;
  } catch {
    return fallback;
  }
}

function limit(rl) {
  if (!rl) return { pct: null, resets_at: null };
  const pct = Number(rl.used_percentage);
  const reset = Number(rl.resets_at);
  return {
    pct: Number.isFinite(pct) ? Math.round(pct * 10) / 10 : null,
    resets_at: Number.isFinite(reset) ? Math.floor(reset) : null,
  };
}

function fakeInput() {
  const now = Math.floor(Date.now() / 1000);
  return {
    model: { display_name: "Test" },
    rate_limits: {
      five_hour: { used_percentage: 42, resets_at: now + 2 * 3600 + 15 * 60 },
      seven_day: { used_percentage: 67, resets_at: now + 3 * 86400 + 5 * 3600 },
    },
  };
}

// fetch's abort can't interrupt a slow DNS (.local) lookup, so race it with a hard timer.
function send(cfg, payload) {
  const timer = new Promise((_, reject) =>
    setTimeout(
      () => reject(Object.assign(new Error("timeout"), { name: "TimeoutError" })),
      TIMEOUT_MS,
    ),
  );
  const post = fetch(`${cfg.host.replace(/\/$/, "")}/usage`, {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-Token": cfg.token },
    body: JSON.stringify(payload),
  }).then((res) => {
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
  });
  return Promise.race([post, timer]);
}

// Sends and records the outcome in the cache. Returns an error string or "".
async function sendAndRecord(cfg, payload, key) {
  try {
    await send(cfg, { ...payload, now: Math.floor(Date.now() / 1000) });
    writeFileSync(CACHE, JSON.stringify({ key, at: Date.now() }));
    return "";
  } catch (e) {
    writeFileSync(CACHE, JSON.stringify({ key: null, at: 0, failedAt: Date.now() }));
    return e.name === "TimeoutError" ? "timeout" : e.message;
  }
}

function fmtLeft(resetsAt) {
  if (!resetsAt) return "";
  const s = Math.max(0, resetsAt - Math.floor(Date.now() / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return h > 0 ? `${h}h${String(m).padStart(2, "0")}m` : `${m}m`;
}

function buildPayload(input) {
  const session = limit(input.rate_limits?.five_hour);
  const weekly = limit(input.rate_limits?.seven_day);
  const model = input.model?.display_name ?? "";
  return { session, weekly, model };
}

async function main() {
  const cfg = loadConfig();
  const mode = process.argv[2];

  // Child: the detached sender started below.
  if (mode === "--send") {
    const payload = JSON.parse(Buffer.from(process.argv[3], "base64").toString("utf8"));
    await sendAndRecord(cfg, payload, JSON.stringify(payload));
    process.exit(0);
  }

  const test = mode === "--test";
  const input = test ? fakeInput() : readJson(0, {});
  const payload = buildPayload(input);
  const { session, weekly, model } = payload;

  const parts = [model];
  if (session.pct !== null) parts.push(`Session ${session.pct}% (${fmtLeft(session.resets_at)})`);
  if (weekly.pct !== null) parts.push(`Week ${weekly.pct}%`);

  const hasUsage = session.pct !== null || weekly.pct !== null;
  if (!cfg) {
    parts.push("no pc/config.json");
  } else if (test) {
    const err = await sendAndRecord(cfg, payload, JSON.stringify(payload));
    parts.push(err ? `CYD offline (${err})` : `sent to ${cfg.host}`);
  } else if (hasUsage) {
    const key = JSON.stringify(payload);
    const c = readJson(CACHE, {});
    const backingOff = c.failedAt && Date.now() - c.failedAt < OFFLINE_BACKOFF_MS;
    if (backingOff) parts.push("CYD offline");
    else if (c.key !== key || Date.now() - c.at > RESEND_MS) {
      // Claim the slot first so rapid refreshes don't start several senders.
      writeFileSync(CACHE, JSON.stringify({ key, at: Date.now() }));
      const arg = Buffer.from(key).toString("base64");
      spawn(process.execPath, [SELF, "--send", arg], {
        detached: true,
        stdio: "ignore",
        windowsHide: true,
      }).unref();
    }
  }

  process.stdout.write(parts.filter(Boolean).join(" | "), () => process.exit(0));
}

main();
