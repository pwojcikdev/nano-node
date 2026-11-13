import pathlib, shutil, subprocess, sys

# usage: setup_trees.py <sim dir> <submodules dir> <harness .cpp> <name>...
# Extracts <sim dir>/<name>.tar (from `git archive <commit>`) into <sim dir>/src/<name> and adds what the harness needs where the tree lacks it,
# the same in every tree: LMDB on POSIX semaphores, the harness in slow_test, a rep crawler query interval constant and an election base latency constant
SIM = pathlib.Path(sys.argv[1])
SUBMODULES = pathlib.Path(sys.argv[2])
HARNESS = pathlib.Path(sys.argv[3])
NAMES = sys.argv[4:]


def sub(path, old, new):
    text = path.read_text()
    assert text.count(old) == 1, (path, old)
    path.write_text(text.replace(old, new))


for name in NAMES:
    src = SIM / 'src' / name
    if src.exists():
        shutil.rmtree(src)
    src.mkdir(parents=True)
    # -m: extraction time as mtime, so a build never takes an older commit's file for up to date
    subprocess.run(['tar', '-x', '-m', '-f', str(SIM / f'{name}.tar'), '-C', str(src)], check=True)
    submodules = src / 'submodules'
    if submodules.exists():
        shutil.rmtree(submodules)
    submodules.symlink_to(SUBMODULES)

    cmake = src / 'CMakeLists.txt'
    if 'if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Debug")\n  target_compile_definitions(lmdb PRIVATE MDB_USE_POSIX_SEM=1)' in cmake.read_text():
        sub(cmake, 'if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Debug")\n  target_compile_definitions(lmdb PRIVATE MDB_USE_POSIX_SEM=1)',
            'if(APPLE)\n  target_compile_definitions(lmdb PRIVATE MDB_USE_POSIX_SEM=1)')

    slow_cmake = src / 'nano/slow_test/CMakeLists.txt'
    if 'network_sim.cpp' not in slow_cmake.read_text():
        sub(slow_cmake, '  ledger.cpp)', '  ledger.cpp\n  network_sim.cpp)')
    shutil.copy(HARNESS, src / 'nano/slow_test/network_sim.cpp')

    constants = src / 'nano/lib/constants.hpp'
    if 'rep_crawler_query_interval' not in constants.read_text():
        sub(constants, '			rep_crawler_warmup_interval = 500ms;\n', '			rep_crawler_warmup_interval = 500ms;\n			rep_crawler_query_interval = 500ms;\n')
        sub(constants, '	std::chrono::milliseconds rep_crawler_warmup_interval{ 1000 * 3 };\n',
            '	std::chrono::milliseconds rep_crawler_warmup_interval{ 1000 * 3 };\n	std::chrono::milliseconds rep_crawler_query_interval{ 1000 * 60 };\n')
        sub(src / 'nano/node/repcrawler.cpp',
            'std::chrono::milliseconds rep_query_interval = node.network_params.network.is_dev_network () ? std::chrono::milliseconds{ 500 } : std::chrono::milliseconds{ 60 * 1000 };',
            'std::chrono::milliseconds rep_query_interval = node.network_params.network.rep_crawler_query_interval;')

    # Before election_base_latency the base latency was hardcoded in nano::election, the constant keeps its values
    if 'election_base_latency' not in constants.read_text():
        sub(constants, '			block_broadcast_delay = 125ms;\n', '			block_broadcast_delay = 125ms;\n			election_base_latency = 25ms;\n')
        sub(constants, '	std::chrono::milliseconds block_broadcast_delay{ 5s };\n',
            '	std::chrono::milliseconds block_broadcast_delay{ 5s };\n	std::chrono::milliseconds election_base_latency{ 1000ms };\n')
        sub(src / 'nano/node/election.cpp', '	return node.network_params.network.is_dev_network () ? 25ms : 1000ms;',
            '	return node.network_params.network.election_base_latency;')
    print(name, 'ready', flush=True)
