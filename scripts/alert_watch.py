#!/usr/bin/env python3
"""Telegram alerting for unattended sessions (Phase 32 minimum).

Follows the newest bot log and pushes alert-worthy lines to a Telegram chat:
- every `[critical]` line (halts, reconcile DRIFT, ws-stale, flatten CARRIED)
  immediately, collapsed per message class with a cooldown so a repeating
  critical (e.g. halted-loop) sends once per window, not once per line;
- `[error]` lines as a periodic digest (count + latest line);
- a startup notice when the watcher arms, so silence means "no news", not
  "watcher dead".

Credentials come from the gitignored secrets file (same one the bot uses):
  "telegram_bot_token": "<from @BotFather>",
  "telegram_chat_id": "<your chat id>"

Usage:
  python3 scripts/alert_watch.py                     # follow logs/, push alerts
  python3 scripts/alert_watch.py --dry-run           # print instead of send
  python3 scripts/alert_watch.py --from-start FILE   # replay a log (validation)
  python3 scripts/alert_watch.py --send "message"    # one-shot (OnFailure unit)

Never raises out of the follow loop: a Telegram outage drops the message and
keeps watching. Exit code 2 means missing/invalid credentials (config error,
systemd should not restart-loop into it silently — the unit logs it).
"""

import argparse
import glob
import json
import os
import re
import socket
import sys
import time
import urllib.parse
import urllib.request

DEFAULT_SECRETS_PATH = "secrets.json"
DEFAULT_LOG_GLOBS = ["logs/app_*.log", "logs/kalshi-mm.err.log"]
POLL_SECONDS = 1.0
RESCAN_SECONDS = 10.0
CRITICAL_COOLDOWN_SECONDS = 300.0
ERROR_DIGEST_SECONDS = 900.0
MESSAGE_LIMIT_CHARS = 500
CLASS_KEY_CHARS = 48
SEND_TIMEOUT_SECONDS = 10
MISSING_CREDENTIALS_EXIT = 2

LEVEL_PATTERN = re.compile(r"\] \[(\w+)\] (.*)")


def load_telegram_credentials(secrets_path):
    try:
        with open(secrets_path) as handle:
            secrets = json.load(handle)
    except (OSError, json.JSONDecodeError) as ex:
        print(f"alert_watch: cannot read {secrets_path}: {ex}", file=sys.stderr)
        return None
    token = secrets.get("telegram_bot_token", "")
    chat_id = str(secrets.get("telegram_chat_id", ""))
    if not token or not chat_id:
        print(
            f"alert_watch: {secrets_path} lacks telegram_bot_token / "
            "telegram_chat_id",
            file=sys.stderr,
        )
        return None
    return token, chat_id


class Notifier:
    def __init__(self, credentials, dry_run):
        self.credentials = credentials
        self.dry_run = dry_run
        self.host = socket.gethostname()

    def send(self, text):
        message = f"[kalshi-mm @ {self.host}] {text}"[:MESSAGE_LIMIT_CHARS]
        if self.dry_run:
            print(f"ALERT: {message}", flush=True)
            return True
        token, chat_id = self.credentials
        url = f"https://api.telegram.org/bot{token}/sendMessage"
        payload = urllib.parse.urlencode(
            {"chat_id": chat_id, "text": message}
        ).encode()
        try:
            with urllib.request.urlopen(
                url, data=payload, timeout=SEND_TIMEOUT_SECONDS
            ) as response:
                return response.status == 200
        except OSError as ex:
            print(f"alert_watch: telegram send failed: {ex}", file=sys.stderr)
            return False


def class_key(message):
    collapsed = re.sub(r"[A-Z0-9][A-Z0-9-]{5,}", "<id>", message)
    collapsed = re.sub(r"\d+", "#", collapsed)
    return collapsed[:CLASS_KEY_CHARS]


class AlertPolicy:
    def __init__(self, notifier, clock=time.monotonic):
        self.notifier = notifier
        self.clock = clock
        self.last_sent_by_class = {}
        self.error_count = 0
        self.last_error_line = ""
        self.last_error_flush = clock()

    def observe(self, line):
        match = LEVEL_PATTERN.search(line)
        if match is None:
            return
        level, message = match.group(1), match.group(2)
        if level == "critical":
            self.observe_critical(message)
        elif level == "error":
            self.error_count += 1
            self.last_error_line = message
        self.flush_error_digest()

    def observe_critical(self, message):
        key = class_key(message)
        now = self.clock()
        last = self.last_sent_by_class.get(key)
        if last is not None and now - last < CRITICAL_COOLDOWN_SECONDS:
            return
        self.last_sent_by_class[key] = now
        self.notifier.send(f"CRITICAL: {message}")

    def flush_error_digest(self, force=False):
        now = self.clock()
        if not force and now - self.last_error_flush < ERROR_DIGEST_SECONDS:
            return
        self.last_error_flush = now
        if self.error_count == 0:
            return
        self.notifier.send(
            f"{self.error_count} error(s) in the last "
            f"{int(ERROR_DIGEST_SECONDS / 60)}m, latest: "
            f"{self.last_error_line}"
        )
        self.error_count = 0
        self.last_error_line = ""


def newest_log(log_globs):
    candidates = []
    for pattern in log_globs:
        candidates.extend(glob.glob(pattern))
    if not candidates:
        return None
    return max(candidates, key=os.path.getmtime)


def replay_file(path, policy):
    with open(path, errors="replace") as handle:
        for line in handle:
            policy.observe(line)
    policy.flush_error_digest(force=True)


def follow(log_globs, policy, notifier):
    current_path, handle, position = None, None, 0
    first_attach = True
    last_rescan = 0.0
    notifier.send(f"watcher armed, following {log_globs}")
    while True:
        now = time.monotonic()
        if now - last_rescan >= RESCAN_SECONDS or handle is None:
            last_rescan = now
            candidate = newest_log(log_globs)
            if candidate is not None and candidate != current_path:
                if handle is not None:
                    handle.close()
                current_path = candidate
                handle = open(candidate, errors="replace")
                if first_attach:
                    handle.seek(0, os.SEEK_END)
                    first_attach = False
                position = handle.tell()
        if handle is None:
            time.sleep(POLL_SECONDS)
            continue
        try:
            size = os.path.getsize(current_path)
        except OSError:
            size = 0
        if size < position:
            handle.seek(0)
            position = 0
        for line in handle:
            policy.observe(line)
        position = handle.tell()
        policy.flush_error_digest()
        time.sleep(POLL_SECONDS)


def main():
    parser = argparse.ArgumentParser(
        description="Telegram alerting for unattended sessions (Phase 32)"
    )
    parser.add_argument("--secrets", default=DEFAULT_SECRETS_PATH)
    parser.add_argument("--log-glob", action="append", default=None)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--from-start", metavar="FILE")
    parser.add_argument("--send", metavar="MESSAGE")
    arguments = parser.parse_args()

    credentials = None
    if not arguments.dry_run:
        credentials = load_telegram_credentials(arguments.secrets)
        if credentials is None:
            return MISSING_CREDENTIALS_EXIT
    notifier = Notifier(credentials, arguments.dry_run)

    if arguments.send is not None:
        return 0 if notifier.send(arguments.send) else 1

    policy = AlertPolicy(notifier)
    if arguments.from_start is not None:
        replay_file(arguments.from_start, policy)
        return 0

    log_globs = arguments.log_glob or DEFAULT_LOG_GLOBS
    follow(log_globs, policy, notifier)
    return 0


if __name__ == "__main__":
    sys.exit(main())
