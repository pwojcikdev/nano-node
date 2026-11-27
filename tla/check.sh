#!/usr/bin/env bash
# Runs the checks listed in checks.txt with TLC and compares every outcome with the expected one.
#
#   ./check.sh                           every check
#   ./check.sh fork                      the checks whose id contains "fork"
#   ./check.sh -t fork-split             also print the trace TLC found
#   CHECKS=checks-long.txt ./check.sh    the checks that take minutes each
#
# Environment:
#   CHECKS      list of checks to run (default: checks.txt)
#   JAVA        java binary (default: java)
#   TLA2TOOLS   tla2tools.jar (default: .tlc/tla2tools.jar, fetched when missing)
#   WORKERS     TLC worker threads (default: auto)
#   HEAP        JVM heap size, for example 8g (default: the JVM's own)
#   LIMIT       seconds after which a check is stopped and reported as "timeout" (default: none)

set -u

dir=$(cd "$(dirname "$0")" && pwd)
java=${JAVA:-java}
work=$dir/.tlc
jar=${TLA2TOOLS:-$work/tla2tools.jar}
workers=${WORKERS:-auto}

show_trace=0
if [ "${1:-}" = "-t" ]; then
    show_trace=1
    shift
fi
filter=${1:-}

case ${CHECKS:-checks.txt} in
    /*) checks=${CHECKS} ;;
    *) checks=$dir/${CHECKS:-checks.txt} ;;
esac

mkdir -p "$work/tmp"

if [ ! -f "$jar" ]; then
    echo "Fetching tla2tools.jar"
    curl -sSL -o "$jar" https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar || exit 2
fi

trim () {
    local value=$1
    value=${value#"${value%%[![:space:]]*}"}
    value=${value%"${value##*[![:space:]]}"}
    printf '%s' "$value"
}

# Writes the configuration of one check: the scenario's own, with constants replaced and the checked property added
make_config () {
    local module=$1 check=$2 overrides=$3 target=$4
    cp "$dir/$module.cfg" "$target"

    local override key
    for override in $overrides; do
        case $override in
            *'<-'*) key=${override%%<-*}; line="    $key <- ${override#*<-}" ;;
            *=*) key=${override%%=*}; line="    $key = ${override#*=}" ;;
            *) echo "bad override: $override" >&2; return 1 ;;
        esac
        if ! grep -Eq "^[[:space:]]*$key[[:space:]]*(=|<-)" "$target"; then
            echo "override of unknown constant: $key" >&2
            return 1
        fi
        awk -v key="$key" -v line="$line" '$1 == key { print line; next } { print }' "$target" > "$target.new" && mv "$target.new" "$target"
    done

    set -- $check
    case $1 in
        base) ;;
        invariant) printf '\nINVARIANT %s\n' "$2" >> "$target" ;;
        property)
            # Liveness needs the fair specification and cannot use symmetry
            sed -e 's/^SPECIFICATION Spec$/SPECIFICATION FairSpec/' -e '/^SYMMETRY/d' "$target" > "$target.new" && mv "$target.new" "$target"
            printf '\nPROPERTY %s\n' "$2" >> "$target"
            ;;
        *) echo "bad check: $check" >&2; return 1 ;;
    esac
}

failures=0
ran=0

while IFS='|' read -r id module check expected overrides; do
    id=$(trim "$id")
    case $id in ''|'#'*) continue ;; esac
    case $id in *"$filter"*) ;; *) continue ;; esac

    module=$(trim "$module")
    check=$(trim "$check")
    expected=$(trim "$expected")
    overrides=$(trim "$overrides")

    config=$work/$id.cfg
    log=$work/$id.log
    if ! make_config "$module" "$check" "$overrides" "$config"; then
        failures=$((failures + 1))
        continue
    fi

    rm -rf "$work/states/$id"
    started=$(date +%s)
    # With a limit, perl turns into the JVM so that the alarm stops TLC itself
    (cd "$dir" && ${LIMIT:+perl -e 'alarm shift; exec @ARGV' "$LIMIT"} \
        "$java" -XX:+UseParallelGC ${HEAP:+-Xmx$HEAP} "-Djava.io.tmpdir=$work/tmp" -cp "$jar" tlc2.TLC \
        -workers "$workers" -deadlock -difftrace -noGenerateSpecTE -metadir "$work/states/$id" -config "$config" "$module.tla") > "$log" 2>&1
    status=$?
    elapsed=$(($(date +%s) - started))
    rm -rf "$work/states/$id"

    # A violated invariant is named, so a probe never passes because something else broke first
    violated=$(sed -n 's/^Error: Invariant \(.*\) is violated.*/\1/p' "$log" | head -1)
    if [ -n "$violated" ]; then
        outcome="violated"
        set -- $check
        if [ "$1" != invariant ] || [ "$2" != "$violated" ]; then
            outcome="violated $violated"
        fi
    elif grep -Eq "^Error: Temporal propert(y|ies) .*violated" "$log"; then
        outcome="violated"
    elif grep -q "No error has been found" "$log"; then
        outcome="holds"
    elif [ "$status" = 142 ]; then
        outcome="timeout"
    else
        outcome="error"
    fi

    states=$(sed -n 's/^\([0-9,]*\) states generated, \([0-9,]*\) distinct states found.*/\2/p' "$log" | tail -1)
    ran=$((ran + 1))
    if [ "$outcome" = "$expected" ]; then
        verdict=ok
    else
        verdict=MISMATCH
        failures=$((failures + 1))
    fi
    printf '%-8s %-28s %-36s %-10s %8ss %14s states\n' "$verdict" "$id" "$check" "$outcome" "$elapsed" "${states:-?}"

    if [ "$outcome" = error ]; then
        grep -v "^Parsing\|^Semantic\|^Linting" "$log" | tail -15
    elif [ "$show_trace" = 1 ] && [ "$outcome" != holds ]; then
        sed -n '/^Error: /,/^[0-9,]* states generated/p' "$log"
    fi
done < "$checks"

echo "$ran checks, $failures not as expected; logs in $work"
[ "$failures" = 0 ]
