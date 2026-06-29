# Deployment — always-on VPS for trading + phone control

This directory provisions a rented Linux box that (a) runs the market maker
24/7 and (b) lets you drive it from your phone via Claude Code Remote Control.

| File | Purpose |
|------|---------|
| `setup.sh` | Install toolchain, build, test, install git hook (mirrors CI) |
| `kalshi-mm.service` | systemd unit for the trading bot (paper by default) |
| `claude-rc.service` | systemd unit for Claude Code Remote Control |

## Where to host: AWS us-east-2 (Ohio)

Kalshi's **matching engine runs in AWS us-east-2 (Ohio)**. The public REST/WS
hosts (`api.elections.kalshi.com`) sit behind CloudFront, so a `ping` measures
the nearest CDN edge (~1 ms from anywhere), **not** the engine. The real engine
round-trip is ~10 ms from Chicago; beating that needs an instance physically
inside us-east-2.

**Reality check:** this strategy is *not* latency-bound. Per our research
(Palumbo: underwriting not market-making; Bürgi-Deng-Whelan: makers earn on a
hold-to-resolution horizon) and the 10 orders/sec Basic rate limit, the edge is
adverse-selection avoidance and spread/E_win discipline — not milliseconds.
So: **put it in us-east-2 to remove latency as a variable cheaply, but do not
overpay for HFT colocation.**

## Sizing

Driven by the **build**, not the bot. The bot is featherweight (a few WS
connections + a quoting loop). The C++23 build compiles googletest, IXWebSocket,
spdlog, and benchmark from source via FetchContent, runs 197 tests, plus
asan/tsan presets and clang-tidy.

- **Minimum to run + build (slowly):** 2 vCPU / 4 GB / 60 GB SSD
- **Recommended (comfortable builds):** 2–4 vCPU / 8 GB / 80 GB SSD
- **OS:** Ubuntu 24.04 LTS (matches CI's `ubuntu-latest`)
- Use **consistent (non-burstable) CPU** for the live trading box — a burstable
  `t3`-class instance can throttle on depleted CPU credits at the worst moment.

## Pricing (approximate, mid-2026 USD/month)

Prices move; treat as ballpark. AWS = on-demand; reserved/savings plans cut
~30–40%. EBS storage on EC2 adds ~$0.08/GB-mo (80 GB ≈ $6.40).

| Provider / region | Specs | ~ $/mo | Notes |
|---|---|---|---|
| **Hetzner Cloud — Ashburn VA** (us-east-1) | CPX31: 4 vCPU / 8 GB / 160 GB | **~$16** | Best value. ~12–15 ms extra to Ohio — fine here. |
| Hetzner Cloud — Ashburn VA | CPX41: 8 vCPU / 16 GB / 240 GB | ~$30 | Roomy builds, still cheap. |
| **Vultr — Chicago** | 4 vCPU / 8 GB / 160 GB | **~$48** | ~10 ms to Ohio, simple flat rate. |
| DigitalOcean — NYC | 4 vCPU / 8 GB / 160 GB | ~$48 | Comparable to Vultr. |
| **AWS Lightsail — us-east-2** | 2 vCPU / 8 GB / 160 GB | **~$44** | Region match, flat rate, simplest AWS. |
| AWS Lightsail — us-east-2 | 4 vCPU / 16 GB / 320 GB | ~$84 | Lightsail couples CPU+RAM; 4 vCPU forces 16 GB. |
| AWS EC2 — us-east-2 | `m7i.large` 2 vCPU / 8 GB | ~$74 + storage | Flexible, grows into PrivateLink/FIX later. |
| AWS EC2 — us-east-2 | `c7i.xlarge` 4 vCPU / 8 GB | ~$124 + storage | Strong consistent CPU; ~$80 reserved. |

**My pick by goal:**
- **Cheapest that's genuinely fine:** Hetzner Ashburn CPX31 (~$16/mo). The extra
  ~12 ms to Ohio is irrelevant for this strategy.
- **Region-matched + simplest:** AWS Lightsail us-east-2 8 GB (~$44/mo).
- **Production-grade, room to grow:** AWS EC2 us-east-2 `c7i.xlarge` (~$124/mo,
  cheaper reserved) — only worth it once real money and uptime SLAs matter.

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

# Configure (NEVER commit these):
cp config.example.json config.json
#   - edit base_url/ws_url (demo vs prod), add your RSA key path
#   - copy your .pem onto the box, e.g. ~/kalshi-private-key.pem

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
