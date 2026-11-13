import json, sys, statistics as st
from collections import defaultdict

# usage: compare.py results.jsonl [build ...]; one row per scenario and build with the seeds pooled, labels are <scenario>_s<seed>_<build>
# Builds come in the order given, or in the order they first appear in the results
rows = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
BUILDS = sys.argv[2:]
groups = defaultdict(list)
for r in rows:
    rest, build = r['label'].rsplit('_', 1)
    scenario = rest.rsplit('_s', 1)[0]
    groups[(scenario, build)].append(r)
    if not sys.argv[2:] and build not in BUILDS:
        BUILDS.append(build)


def per_block(r, key):
    return r[key] / max(1, r['published'])


def share(rs, key, total):
    return 100 * sum(r[key] for r in rs) / max(1, sum(r[total] for r in rs))


cols = [
    ('n', lambda rs: len(rs), '{:d}'),
    ('conv', lambda rs: sum(1 for r in rs if r['converged']), '{:d}'),
    ('fan', lambda rs: rs[0]['fanout'], '{:d}'),
    ('fmiss%', lambda rs: share(rs, 'flood_missed', 'prop_pairs'), '{:.3f}'),
    ('never%', lambda rs: share(rs, 'flood_never', 'prop_pairs'), '{:.3f}'),
    ('blk_fmiss%', lambda rs: share(rs, 'flood_blocks_missed', 'prop_blocks'), '{:.1f}'),
    ('late%', lambda rs: share(rs, 'late', 'prop_pairs'), '{:.3f}'),
    ('arr_p99', lambda rs: st.mean(r['arrival_p99'] for r in rs), '{:.2f}'),
    ('conf_p50', lambda rs: st.mean(r['confirmation_p50'] for r in rs), '{:.3f}'),
    ('conf_p99', lambda rs: st.mean(r['confirmation_p99'] for r in rs), '{:.2f}'),
    ('conf_max', lambda rs: max(r['confirmation_max'] for r in rs), '{:.1f}'),
    ('slow%', lambda rs: share(rs, 'confirmation_slow', 'confirmation_pairs'), '{:.3f}'),
    ('all_p50', lambda rs: st.mean(r['latency_all_p50'] for r in rs), '{:.3f}'),
    ('all_p99', lambda rs: st.mean(r['latency_all_p99'] for r in rs), '{:.2f}'),
    ('all_max', lambda rs: st.mean(r['latency_all_max'] for r in rs), '{:.1f}'),
    ('clean_p99', lambda rs: st.mean(r['cemented_clean_p99'] for r in rs), '{:.2f}'),
    ('pub/blk', lambda rs: st.mean(per_block(r, 'publish_out') for r in rs), '{:.0f}'),
    ('dir/blk', lambda rs: st.mean(per_block(r, 'solicit_broadcasts') for r in rs), '{:.1f}'),
    ('req/blk', lambda rs: st.mean(per_block(r, 'confirm_req_out') for r in rs), '{:.0f}'),
    ('MB', lambda rs: st.mean(r['bytes_out'] for r in rs) / 1e6, '{:.0f}'),
    ('cores', lambda rs: st.mean(r['cpu_cores_used'] for r in rs), '{:.1f}'),
]
scenarios = []
for scenario, _ in groups:
    if scenario not in scenarios:
        scenarios.append(scenario)
width = max((len(s) + 10 for s in scenarios), default=20)
print(' ' * width + ' '.join(f'{c[0]:>10}' for c in cols))
for scenario in scenarios:
    for build in BUILDS:
        rs = groups.get((scenario, build))
        if not rs:
            continue
        cells = []
        for _, fn, fmt in cols:
            try:
                cells.append(fmt.format(fn(rs)))
            except Exception:
                cells.append('-')
        print(f'{scenario + " " + build:<{width}}' + ' '.join(f'{c:>10}' for c in cells))
    print()
