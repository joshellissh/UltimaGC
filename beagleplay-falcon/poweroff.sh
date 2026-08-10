#!/usr/bin/env bash
# Cleanly shut down the BeaglePlay over SSH — no serial console needed.
#
# Repeatedly yanking power has corrupted this board's rootfs (both SD and
# eMMC, in separate incidents — see NOTES.md "Always shut down cleanly") badly
# enough that fsck couldn't auto-repair it. sync; poweroff is cheap; a reflash
# afterward is not.
#
# Usage:
#   ./poweroff.sh [user@host]        default root@beagleplay-ti.local

set -euo pipefail

HOST="${1:-root@beagleplay-ti.local}"

# See emmc-push.sh for why host-key pinning is deliberately skipped: dropbear
# mints a fresh key on every first boot of a freshly-flashed card, and this is
# a lab board with passwordless root — there's no secret here to protect.
SSH_OPTS=(-o ConnectTimeout=10 -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

echo "Connecting to $HOST..."

# `;`, not `&&`, between sync/echo and the backgrounded subshell. A trailing
# `&` backgrounds the single preceding command — but with `&&`, that "single
# command" is the *entire* AND-list, so sync and echo never actually run before
# the remote shell returns and the SSH session (and everything riding on it)
# can be torn down. Verified locally: `A && B && (C) &` prints nothing before
# returning; `A; B; (C) &` runs A and B in the foreground first.
#
# poweroff kills the SSH session by design, so its own exit status isn't
# meaningful — background just that part and disconnect deliberately, after
# sync has genuinely completed. </dev/null on the subshell: a background job
# that inherits an open stdin can hold the SSH channel open and hang the
# client.
OUT=$(ssh "${SSH_OPTS[@]}" "$HOST" \
    'sync; echo SYNCED; (sleep 1; poweroff) </dev/null >/dev/null 2>&1 &') || {
    echo "Could not reach $HOST — is it powered on and on the network?" >&2
    exit 1
}
if [ "$OUT" != "SYNCED" ]; then
    echo "Unexpected response from $HOST: '$OUT' — sync may not have completed." >&2
    exit 1
fi

echo "Synced. Board is powering off — wait for the LEDs to go dark before removing power or a card."
