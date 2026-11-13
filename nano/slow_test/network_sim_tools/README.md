# Network simulation tools

Scripts around `nano/slow_test/network_sim.cpp`, a multi-node network simulation that runs inside `slow_test`. The harness describes itself at the top of the file: nodes over loopback TCP, every timing given in live network units and run `TIMESCALE` times faster, per node upload limits, forks, restarts and partitions. Each run appends one JSON line of measurements.

## One run

Write the parameters as one line of `NAME=value` words into `network_sim_batch.txt` in the application path and run the batch test:

```bash
echo "LABEL=try NODES=24 REPS=16 LOAD=240 DRAIN=600 TIMESCALE=2 BANDWIDTH=uniform RATE=5 ACCOUNTS=150 SEED=1 OUT=/tmp/sim.jsonl" > /Volumes/RAMDisk/sim/network_sim_batch.txt
NANO_APP_PATH=/Volumes/RAMDisk/sim NANO_BACKEND=lmdb slow_test --gtest_filter=network_sim.batch
```

`network_sim.custom` takes the same parameters as `NANO_TEST_SIM_<NAME>` environment variables, with `NANO_TEST_SIM=1` to enable it.

Network shape: `NODES`, `REPS`, `SKEW`, `BANDWIDTH` (uniform, mixed or starved), `DEGREE`, `SHARED_REPS`. Load: `RATE` (blocks per second), `LOAD` and `DRAIN` (seconds), `ACCOUNTS`, `FORKS`, `ORIGIN` (`live` injects blocks as relayed traffic, `local` as a wallet's own block). Disruptions: `RESTARTS`, `RESTART_DOWN`, `RESTART_REPS`, `PARTITION`, `PARTITION_LEN`, `PARTITION_MINORITY`. Solicitation caps: `REQUESTS`, `BROADCASTS`, `CHANNEL_MESSAGES`.

Election and flooding timings default to the build's own live values, scaled. These override them, in live seconds unless noted, and fail on a build without the constant: `BROADCAST_DELAY`, `BLOCK_INTERVAL`, `FLOOD_INTERVAL`, `COOLDOWN`, `PASSIVE_DURATION`, `REQUEST_INTERVAL`, `FANOUT_SCALE` (a multiplier).

## Comparing builds

1. Export each commit to compare: `git archive -o <sim dir>/<name>.tar <commit>`.
2. `python3 setup_trees.py <sim dir> <repo>/submodules <repo>/nano/slow_test/network_sim.cpp <name>...` extracts them and adds what the harness needs where a tree lacks it, the same in every tree: LMDB on POSIX semaphores, the harness in `slow_test`, a rep crawler query interval constant and an election base latency constant.
3. `NINJA=<path> ./build_all.sh <sim dir> <name>...` builds a Release `slow_test` per tree into `<sim dir>/bin`.
4. `python3 driver.py <sim dir> plan.txt <name>...` runs every plan line on every build back to back, appending to `<sim dir>/results.jsonl`; an interrupted batch resumes where it stopped.
5. `python3 compare.py <sim dir>/results.jsonl <name>...` prints one row per scenario and build with the seeds pooled.

## Pitfalls

- Run on an otherwise idle machine and never build while runs are measuring: latency figures follow machine load. `flood_never`, the share of node/block pairs no flood ever reached, does not depend on timing.
- A 24 node network with 16 representatives starves the ordinary nodes of votes; 40 nodes with 10 representatives and `SKEW=0.5` is closer to the live rep share, and needs `TIMESCALE=1`.
- Use `ORIGIN=local` for blocks made by wallets: those go to every principal representative first.
- LMDB needs POSIX semaphores on macOS; SysV semaphores run out with the many environments of one run.
