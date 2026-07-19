# Deployment — always-on VPS for trading + phone control

This directory provisions a rented Linux box that (a) runs the market maker
24/7 and (b) lets you drive it from your phone via Claude Code Remote Control.
The Phase 32 supervision pieces (alerting + log rotation) also install on the
CURRENT machine for unattended demo soaks — no cloud needed (Hosting gate).

| File | Purpose |
|------|---------|
| `setup.sh` | Install toolchain, build, test, install git hook (mirrors CI) |
| `kalshi-mm.service` | systemd unit for the trading bot (paper by default) |
| `kalshi-alert.service` | Telegram alert watcher (halts, DRIFT, CARRIED, errors) |
| `kalshi-notify-failure.service` | oneshot Telegram ping when the bot unit fails |
| `logrotate-kalshi` | rotation for the units' append-mode service logs |
| `claude-rc.service` | systemd unit for Claude Code Remote Control |

## Where to host

Decided (PLAN L1): **plain EC2 `t4g.small` (~$12/mo) inside Kalshi's AWS
region**, chosen by measuring — probe us-east-1 vs us-east-2 RTT first, not by
third-party claims. The full walkthrough (region probe, launch, build, secret
transfer, first-session latency comparison) is
[docs/AWS_SETUP.md](../docs/AWS_SETUP.md); this directory holds the service
units it installs. The public hosts sit behind CloudFront, so `ping` measures
the nearest CDN edge, not the matching engine — only an in-region session
comparison (order-path RTT via `scripts/latency_report.py`) is a real answer.

Sizing is driven by the **build**, not the bot (the bot is a few WS
connections + a quoting loop; its decision path is sub-µs). A 2 vCPU / 2 GB
arm64 box builds the tree in ~10–15 min and runs the full test suite; use
Ubuntu 24.04 LTS to match CI.

## One box or two?

Start with **one** box (dev + Claude + paper/demo trading). For **real-money
production, split into two** — for *security*, not performance:

> The live trading RSA private key plus an always-on Claude with filesystem
> access on the **same** box is a real risk surface. Claude could place live
> orders, and a host compromise exposes funds.

- **Trading box:** holds the `.pem`, runs only `kalshi-mm.service`, locked down.
- **Dev box:** runs `claude-rc.service`, no live key.

If you keep them combined, at minimum run Remote Control with `--sandbox` and
scope the live key to a tight account. Two cheap Hetzner boxes (~$32/mo total)
is a clean production split.

## Quick start

```bash
# On the fresh box, as your normal (non-root) user:
git clone git@github.com:jfreun123/kalshi-market-maker.git
cd kalshi-market-maker
bash deploy/setup.sh

# Configure (secrets.json is gitignored — NEVER commit it):
cp secrets.example.json secrets.json
#   - edit base_url/ws_url (demo vs prod), api_key, private_key_path
#   - copy your .pem onto the box, e.g. ~/kalshi-private-key.pem
#   (the committed config.json holds strategy params and points at
#    secrets.json via secrets_path)

# Smoke test:
./build/source/kalshi_mm --paper config.json
```

### Install the systemd services

Replace the placeholder user in both units, then enable them:

```bash
# Substitute your actual username for the REPLACE_USER placeholder:
sudo sed "s/REPLACE_USER/$USER/g" deploy/kalshi-mm.service \
  | sudo tee /etc/systemd/system/kalshi-mm.service >/dev/null
sudo sed "s/REPLACE_USER/$USER/g" deploy/claude-rc.service \
  | sudo tee /etc/systemd/system/claude-rc.service >/dev/null

sudo systemctl daemon-reload
sudo systemctl enable --now kalshi-mm     # paper mode until you drop --paper
sudo systemctl enable --now claude-rc     # after a one-time `claude` /login

# Watch:
journalctl -u kalshi-mm -f
journalctl -u claude-rc -f
```

`claude-rc.service` requires a **one-time interactive `claude` /login** as your
user first (Pro/Max plan, claude.ai OAuth — API keys are rejected). See the
header of `claude-rc.service` for the full prerequisite steps.

## Phase 32 — unattended supervision on the current machine (WSL)

Everything below runs on the existing WSL box; it is what the 30h demo soak
requires. WSL2 needs systemd enabled once: put `[boot]\nsystemd=true` in
`/etc/wsl.conf`, then `wsl --shutdown` from Windows and reopen.

**1. Create the Telegram bot (one time).** Message `@BotFather` → `/newbot`
→ copy the token. Message your new bot once (any text), then find your chat
id: `curl -s https://api.telegram.org/bot<TOKEN>/getUpdates | python3 -m
json.tool` → `message.chat.id`. Add both to the gitignored `secrets.json`:

```json
  "telegram_bot_token": "123456:ABC...",
  "telegram_chat_id": "1234567890"
```

**2. Smoke-test the pipeline** (a real message should arrive on your phone):

```bash
python3 scripts/alert_watch.py --send "alert pipeline test"
```

**3. Install the units + rotation:**

```bash
for unit in kalshi-mm kalshi-alert kalshi-notify-failure; do
  sudo sed "s/REPLACE_USER/$USER/g" "deploy/$unit.service" \
    | sudo tee "/etc/systemd/system/$unit.service" >/dev/null
done
sudo sed "s/REPLACE_USER/$USER/g" deploy/logrotate-kalshi \
  | sudo tee /etc/logrotate.d/kalshi >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now kalshi-alert          # watcher first
sudo systemctl enable --now kalshi-mm             # then the bot
```

You get: a "watcher armed" message at alert-service start, an immediate
message for every distinct `[critical]` (halt, reconcile DRIFT, ws-stale,
flatten CARRIED — repeats collapse per 5-minute class cooldown), an
`[error]` digest at most every 15 minutes, and a "bot is down" ping if the
unit crash-loops past its restart limit. Silence = no news, because the
armed message proves the pipeline.

**4. Soak configuration.** The committed unit ships `--paper config.json`
for safety. For the demo soak, override the ExecStart without editing the
committed file:

```bash
sudo systemctl edit kalshi-mm
# in the editor:
#   [Service]
#   ExecStart=
#   ExecStart=/home/<you>/kalshi-market-maker/build/source/kalshi_mm config-demo.json
```

Watch: `journalctl -u kalshi-mm -f` and `journalctl -u kalshi-alert -f`;
laptop sleep pauses both cleanly (known demo quirk: the session is wasted,
not corrupted).

## Networking / firewall

Remote Control and the bot both make **outbound HTTPS only** and never open
inbound ports — no port-forwarding or tunnels needed. Lock the firewall to:

- **Inbound:** SSH (:22) from your IP only. Nothing else.
- **Outbound:** :443 (Anthropic API + Kalshi + github.com for FetchContent).

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow from <YOUR_IP> to any port 22
sudo ufw enable
```
