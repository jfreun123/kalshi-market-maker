# AWS Setup — L1 of the Low-Latency Package

Goal: run the bot from a small EC2 instance inside Kalshi's own AWS region,
turning the measured ~300ms order-path round trip (L0 baseline, Mac) into
~3–5ms. Decision record in PLAN.md: plain EC2, **not** a third-party "trading
VPS" — placement beats hardware, and a `t4g.small` is ample for a 2-thread
bot whose decision path is ~300ns.

## 0. Prerequisites

- AWS account with billing set up (aws.amazon.com — root account, then create
  an IAM user for daily use; enable MFA on both).
- AWS CLI locally: `brew install awscli && aws configure` (paste the IAM
  user's access key; default region `us-east-1` for now).
- Your Kalshi demo credentials stay in `/Users/jacobfreund/kalshi-demo-key/`
  — they will be copied to the instance, never into git.

## 1. Region probe — measure before choosing

Kalshi is AWS-hosted in US East; third parties claim us-east-2 (Ohio) but it
is unverified. Let the numbers decide. Launch the cheapest possible probe in
each candidate region (or use CloudShell, which needs no instance):

```bash
# In the AWS console: CloudShell in us-east-1, then again in us-east-2
for i in 1 2 3 4 5; do
  curl -so /dev/null -w "%{time_total}s\n" \
    "https://demo-api.kalshi.co/trade-api/v2/markets?limit=1"
done
```

Pick the region with the lower median. Everything below assumes the winner
(shown as `$REGION`).

## 2. Launch the instance

Console → EC2 → Launch instance, or CLI. The important choices:

| Setting | Value | Why |
|---|---|---|
| Region | probe winner (us-east-1 or -2) | the whole point |
| AMI | Ubuntu Server 24.04 LTS, **arm64** | matches t4g (Graviton) |
| Type | `t4g.small` (2 vCPU, 2 GB) | ~$12/mo; build once, run forever |
| Storage | 20 GB gp3 | build tree + logs |
| Key pair | create `kalshi-mm`, download `.pem` | SSH access |
| Security group | **no inbound** except SSH (22) from *My IP* | the bot only makes outbound calls |

```bash
chmod 400 ~/Downloads/kalshi-mm.pem
ssh -i ~/Downloads/kalshi-mm.pem ubuntu@<instance-public-ip>
```

## 3. Build the bot on the instance

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake ninja-build git libssl-dev zlib1g-dev libbrotli-dev
git clone https://github.com/jfreun123/kalshi-market-maker.git
cd kalshi-market-maker
cmake --preset=dev && cmake --build build -j 2   # ~10-15 min on t4g.small
cd build && ctest -j 2 -E DemoConformance        # expect all green
```

## 4. Move the secrets (never through git)

From the Mac:

```bash
scp -i ~/Downloads/kalshi-mm.pem -r \
  /Users/jacobfreund/kalshi-demo-key ubuntu@<ip>:~/kalshi-demo-key
scp -i ~/Downloads/kalshi-mm.pem \
  /Users/jacobfreund/kalshi-market-maker/secrets.json ubuntu@<ip>:~/kalshi-market-maker/
```

Then on the instance, edit `secrets.json` so `private_key_path` points at
`/home/ubuntu/kalshi-demo-key/<key>.pem` (the committed `config.json` finds
it via `secrets_path`; create a local `config-demo.json` copy for the run
scripts).

## 5. First session + the L1 comparison

```bash
cd ~/kalshi-market-maker
./scripts/run_demo.sh --clean 5
python3 scripts/latency_report.py logs/analytics_*.jsonl
```

Compare stage-by-stage against the Mac baseline recorded in PLAN.md (order
path ~294ms, orderbook ~76ms from the Mac). Expect single-digit milliseconds.
Also compare post-only-cross rejects and markout across a few sessions —
that's the real verdict on whether latency was costing edge.

## 6. Unattended operation (Phase 32 minimum)

The `deploy/` directory already has a systemd unit (`kalshi-mm.service`) and
`setup.sh`. On the instance:

```bash
sudo cp deploy/kalshi-mm.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now kalshi-mm
journalctl -u kalshi-mm -f
```

This is what makes the Gate-1 30-hour soak practical — no laptop sleep, no
holiday outages on your side of the wire. Add logrotate and a Telegram alert
on halt/error before leaving it truly unattended (PLAN item 8).

## 7. Costs and hygiene

- `t4g.small` on-demand ≈ **$12/mo** + a few dollars of EBS. Stop the
  instance when idle weeks happen; storage persists.
- Set an AWS budget alert at $20/mo (Billing → Budgets) so a mistake can't
  surprise you.
- The instance clock is NTP-synced by AWS — the host-vs-program boundary
  holds itself.
