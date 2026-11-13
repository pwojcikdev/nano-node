#!/bin/bash
# usage: build_all.sh <sim dir> <name>...
# Builds a Release slow_test for each tree prepared by setup_trees.py and copies it to <sim dir>/bin/slow_test_<name>
# NINJA names the ninja binary when it is not on PATH
SIM="$1"
shift
NINJA="${NINJA:-$(command -v ninja)}"
mkdir -p "$SIM/bin" "$SIM/build"
for name in "$@"; do
    echo "== $name $(date +%H:%M:%S)"
    cmake -S "$SIM/src/$name" -B "$SIM/build/$name" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_BUILD_TYPE=Release -DNANO_TEST=ON -DNANO_GUI=OFF > "$SIM/build/$name.configure.log" 2>&1 \
        || { echo "configure failed: $name"; tail -20 "$SIM/build/$name.configure.log"; exit 1; }
    cmake --build "$SIM/build/$name" --target slow_test > "$SIM/build/$name.build.log" 2>&1 \
        || { echo "build failed: $name"; grep -E "error|FAILED" -A3 "$SIM/build/$name.build.log" | head -40; exit 1; }
    cp "$SIM/build/$name/slow_test" "$SIM/bin/slow_test_$name"
    echo "built $name $(date +%H:%M:%S)"
done
echo "all builds done"
