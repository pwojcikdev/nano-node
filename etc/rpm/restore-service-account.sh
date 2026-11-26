#!/bin/sh
# Old RPMs ran userdel after installing their replacement, including on upgrades.
getent passwd nanocurrency >/dev/null && exit 0

fail() {
    echo "Cannot restore the Nano service account: $*" >&2
    exit 1
}

home_dir=/var/nanocurrency
[ -d "$home_dir" ] && [ ! -L "$home_dir" ] || fail "missing or invalid $home_dir"
owner=$(stat -c '%u:%g' "$home_dir") || fail "cannot read ownership of $home_dir"
uid=${owner%:*}
gid=${owner#*:}
case "$owner" in
    0:*|*:0|65534:*|*:65534) fail "unexpected ownership $owner on $home_dir" ;;
esac

# The RPM preinstall script assigned both the shared home and the network
# directories to this account. They must still agree before reclaiming the IDs.
found_dir=false
for dir in "$home_dir/Nano" "$home_dir/NanoBeta"; do
    if [ -e "$dir" ] || [ -L "$dir" ]; then
        [ -d "$dir" ] && [ ! -L "$dir" ] && [ "$(stat -c '%u:%g' "$dir")" = "$owner" ] ||
            fail "unexpected directory or ownership at $dir"
        found_dir=true
    fi
done
[ "$found_dir" = true ] || fail "no network directory under $home_dir"

getent passwd "$uid" >/dev/null && fail "UID $uid already belongs to another account"
if group=$(getent group nanocurrency); then
    [ "$(getent group "$gid")" = "$group" ] || fail "the nanocurrency group no longer owns $home_dir"
else
    getent group "$gid" >/dev/null && fail "GID $gid already belongs to another group"
    groupadd --system --gid "$gid" nanocurrency || fail "cannot restore group $gid"
fi
useradd --system --uid "$uid" --gid "$gid" --no-create-home --home-dir "$home_dir" \
    --shell /bin/bash --comment "Nanocurrency Daemon user" nanocurrency
