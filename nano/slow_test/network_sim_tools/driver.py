import json, os, subprocess, sys, time

# usage: driver.py <sim dir> <plan file> <build>...
# Each plan line is a scenario (LABEL=... NAME=value ...); every build runs it back to back as <label>_<build>, with <sim dir>/bin/slow_test_<build>
# Results are appended to <sim dir>/results.jsonl; a label already there is skipped, so an interrupted batch resumes where it stopped
# NANO_SIM_APP sets the application path of the runs, a RAM disk keeps the ledgers off the SSD
SIM = sys.argv[1]
PLAN = sys.argv[2]
BUILDS = sys.argv[3:]
OUT = os.path.join(SIM, 'results.jsonl')
APP = os.environ.get('NANO_SIM_APP', '/Volumes/RAMDisk/network_sim')


def done():
    if not os.path.exists(OUT):
        return set()
    return {json.loads(line)['label'] for line in open(OUT) if line.strip()}


for line in open(PLAN):
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    words = dict(word.split('=', 1) for word in line.split())
    for build in BUILDS:
        label = f"{words['LABEL']}_{build}"
        if label in done():
            continue
        words_build = dict(words, LABEL=label, OUT=OUT)
        os.makedirs(APP, exist_ok=True)
        with open(os.path.join(APP, 'network_sim_batch.txt'), 'w') as batch:
            batch.write(' '.join(f'{k}={v}' for k, v in words_build.items()) + '\n')
        env = dict(os.environ, NANO_APP_PATH=APP, NANO_BACKEND='lmdb')
        began = time.time()
        try:
            run = subprocess.run([os.path.join(SIM, 'bin', f'slow_test_{build}'), '--gtest_filter=network_sim.batch'], env=env, capture_output=True, text=True, timeout=1500)
            status = 'ok' if run.returncode == 0 else f'rc={run.returncode}'
            if run.returncode != 0:
                with open(os.path.join(SIM, 'driver_errors.log'), 'a') as errors:
                    errors.write(f'== {label}\n{run.stdout[-4000:]}\n{run.stderr[-4000:]}\n')
        except subprocess.TimeoutExpired:
            status = 'timeout'
        print(f'{label} {status} {time.time() - began:.0f}s', flush=True)
print('plan done', flush=True)
