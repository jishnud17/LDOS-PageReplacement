#!/usr/bin/env bash
#
# setup_cloudlab.sh - one-shot bootstrap for a fresh CloudLab node.
#
# CloudLab nodes reset to the base image on reboot/timeout, so everything below
# has to be redone each session.  This script is idempotent -- safe to re-run.
#
# Usage (from anywhere on the node, after the manager repo is cloned):
#     bash ~/LDOS-PageReplacement/setup_cloudlab.sh            # env + build, no data downloads
#     bash ~/LDOS-PageReplacement/setup_cloudlab.sh --twitter  # + the 12GB Twitter graph (pr, bc)
#     bash ~/LDOS-PageReplacement/setup_cloudlab.sh --kron     # + kron graph (cc)
#     bash ~/LDOS-PageReplacement/setup_cloudlab.sh --kddb     # + kddb dataset (liblinear)
# Flags combine: --twitter --kron --kddb
#
# Typical fresh-node flow:
#     git clone https://github.com/jishnud17/LDOS-PageReplacement.git
#     cd LDOS-PageReplacement && git checkout heuristics-analysis
#     bash setup_cloudlab.sh
#
set -euo pipefail

WANT_TWITTER=0
WANT_KRON=0
WANT_KDDB=0
for arg in "$@"; do
    case "$arg" in
        --twitter) WANT_TWITTER=1 ;;
        --kron)    WANT_KRON=1 ;;     # kron graph for cc-twitter
        --kddb)    WANT_KDDB=1 ;;     # kddb dataset for liblinear (~2.5GB download)
        *) echo "unknown flag: $arg (valid: --twitter --kron --kddb)"; exit 1 ;;
    esac
done

MANAGER_DIR="$HOME/LDOS-PageReplacement"
BENCH_DIR="$HOME/benchmarks"
WORKLOADS_DIR="$BENCH_DIR/workloads"
GAPBS_DIR="$WORKLOADS_DIR/gapbs"
GAPBS_DIFF_URL="https://raw.githubusercontent.com/cosmoss-jigu/memtis/main/memtis-userspace/bench_dir/gapbs-pr.diff"

say() { printf '\n\033[1;36m[setup] %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
say "1/5  apt dependencies"
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential git python3 python3-pip wget curl bzip2

# ---------------------------------------------------------------------------
say "2/5  build the tiered-memory manager"
cd "$MANAGER_DIR"
git pull --ff-only || true      # pick up any pushed fixes; ignore if detached
make

# ---------------------------------------------------------------------------
say "3/5  clone + build GAPBS workloads"
mkdir -p "$BENCH_DIR"
if [[ ! -d "$WORKLOADS_DIR/.git" ]]; then
    git clone https://github.com/SujayYadalam94/workloads "$WORKLOADS_DIR"
fi
cd "$WORKLOADS_DIR"
git submodule update --init --recursive gapbs

# apply the memtis diff that adds the gen-twitter target (guard against double-apply)
curl -fsSL -o "$HOME/gapbs-pr.diff" "$GAPBS_DIFF_URL"
cd "$GAPBS_DIR"
if git apply --check "$HOME/gapbs-pr.diff" 2>/dev/null; then
    git apply "$HOME/gapbs-pr.diff"
    say "     applied gapbs-pr.diff"
else
    say "     gapbs-pr.diff already applied (or not needed) -- skipping"
fi
make

# ---------------------------------------------------------------------------
say "3b/5  build XSBench + liblinear"
cd "$WORKLOADS_DIR"
git submodule update --init XSBench
make -C XSBench/openmp-threading -s
make -C liblinear-2.47 -s
say "     built XSBench/openmp-threading/XSBench and liblinear-2.47/train"

# ---------------------------------------------------------------------------
say "4/5  kernel permissions for PEBS + userfaultfd"
sudo sysctl -w kernel.perf_event_paranoid=-1
sudo sysctl -w kernel.perf_cpu_time_max_percent=0
echo 1 | sudo tee /proc/sys/vm/unprivileged_userfaultfd >/dev/null

# ---------------------------------------------------------------------------
if [[ "$WANT_TWITTER" == "1" ]]; then
    say "5/5  generating Twitter graph (12GB .sg, downloads ~6GB, slow)"
    cd "$GAPBS_DIR"
    make gen-twitter
else
    say "5/5  skipping Twitter graph (pass --twitter to build it)"
fi

if [[ "$WANT_KRON" == "1" ]]; then
    say "5b   generating kron graph for cc (scale 26, a few minutes)"
    cd "$GAPBS_DIR"
    [[ -f benchmark/graphs/kron.sg ]] || \
        ./converter -g 26 -b benchmark/graphs/kron.sg
fi

if [[ "$WANT_KDDB" == "1" ]]; then
    say "5c   downloading kddb dataset for liblinear (~2.5GB compressed)"
    mkdir -p "$BENCH_DIR/inputs"
    if [[ ! -f "$BENCH_DIR/inputs/kddb" ]]; then
        wget -q --show-progress -O "$BENCH_DIR/inputs/kddb.bz2" \
            "https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/datasets/binary/kddb.bz2"
        bunzip2 "$BENCH_DIR/inputs/kddb.bz2"
    fi
fi

# ---------------------------------------------------------------------------
say "DONE.  Quick smoke test (generated graph, no download):"
cat <<EOF

  cd $MANAGER_DIR
  GAPBS=$GAPBS_DIR
  LDOS_CSV_LABEL=gapbs_pr_smoke \\
    LD_PRELOAD=./lib/libmmap_shim.so \\
    \$GAPBS/pr -u 24 -i 5 -n 1

  # then check:
  grep -c . ml_dataset_gapbs_pr_smoke.csv

Twitter run (after --twitter build):
  LDOS_CSV_LABEL=gapbs_pr_twitter \\
    LD_PRELOAD=./lib/libmmap_shim.so \\
    \$GAPBS/pr -f $GAPBS_DIR/benchmark/graphs/twitter.sg -i 20 -n 1
EOF
