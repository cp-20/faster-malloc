#!/usr/bin/env bash
# campaign.sh -- one autonomous faster-malloc worker cycle.
#
# Pick one backlog task with the free hy4 model, run it in a worktree, push
# its experiment branch and let GitHub Actions do the measurement.  The
# loop that calls this script lives outside the repository:
#
#   nohup bash -c 'while true; do /path/to/bench/ci/campaign.sh; sleep 1800; done' \
#         >/tmp/fm-campaign-loop.log 2>&1 &
#
# Stop it with: kill "$(cat /tmp/fm-campaign.pid)"
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
STATE=/tmp/fm-campaign
mkdir -p "$STATE"

# One worker at a time, even if the loop and a manual run overlap.
exec 9>"$STATE/lock"
flock -n 9 || { echo "campaign: another worker holds the lock"; exit 0; }

git fetch -q origin main || { echo "campaign: fetch failed"; exit 0; }
if [ -z "$(git status --porcelain)" ]; then
    git pull -q --ff-only origin main || true
fi

# Do not pile work onto a saturated CI queue.
active=$(gh run list -L 50 --json workflowName,status \
    --jq '[.[] | select((.workflowName == "exp-bench" or .workflowName == "exp-matrix" or .workflowName == "exp-legacy") and .status == "in_progress")] | length' \
    2>/dev/null || echo 0)
if [ "${active:-0}" -ge 12 ]; then
    echo "campaign: $active CI runs in flight, skipping this cycle"
    exit 0
fi

ts=$(date -u +%Y%m%dT%H%M%SZ)
log="$STATE/worker-$ts.log"
echo "=== campaign cycle $ts ===" | tee -a "$log"
timeout 2400 opencode run \
    --model opencode-go/hy4-flash-free \
    --agent general \
    --auto \
    --archived \
    --title "fm auto $ts" \
    "$(cat docs/exp/AUTO_WORKER.md)" >>"$log" 2>&1
rc=$?
echo "campaign: worker rc=$rc log=$log" | tee -a "$log"
tail -20 "$log"
exit 0
