#!/bin/sh
# Copy this machine's telemetry into the Rime sync directory, where iCloud
# picks it up and every other machine can read it.
#
# The plugin can now do this itself -- `copilot/telemetry/auto_sync: true`,
# every 30 minutes plus once at session end (src/telemetry.h, SyncToDir). This
# script stays the manual path, and is what to reach for when auto_sync is off
# or when you want the copy to happen exactly now.
#
# Only half of the original objection to doing it in the plugin has died. It
# was: appending inside an iCloud directory would re-upload the file on every
# keystroke, and iCloud may evict a file it considers cold. That still stands,
# and auto_sync does not violate it -- the append stays in the local
# private/ directory and only a periodic whole-file copy reaches this
# destination. What changed is the assumption that a manual step would be
# run: a copy nobody remembers to make is days stale exactly when someone
# comes to analyse it.
set -eu

RIME_DIR="${RIME_DIR:-$HOME/Library/Rime}"
SRC="$RIME_DIR/private/copilot_telemetry"

SYNC_DIR=$(sed -n 's/^sync_dir: *"\(.*\)"$/\1/p' "$RIME_DIR/installation.yaml")
if [ -z "$SYNC_DIR" ]; then
  echo "no sync_dir in $RIME_DIR/installation.yaml" >&2
  exit 1
fi

# Flat, unlike $SRC: the local copy lives under private/ because
# $RIME_DIR is commonly a git repo (private/ is the conventional gitignore
# line for it); the sync dir is not a git repo, so private/ would buy
# nothing there and a flat name is clearer.
DEST="$SYNC_DIR/copilot_telemetry"
mkdir -p "$DEST"
# Not into $SYNC_DIR/<installation_id>/, which Rime's own sync task manages.
cp "$SRC"/*.jsonl "$DEST"/ 2>/dev/null || true
echo "copied to $DEST"
ls -l "$DEST"

# Print the analysis command with the path already resolved. SYNC_DIR is
# computed here and is not exported to the caller's shell, so a documented
# command that referenced it would expand to nothing and produce an empty glob
# — which looks exactly like telemetry being broken.
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
echo
echo "run the report with:"
echo "  $SELF_DIR/analyze_telemetry.py \"$DEST\"/*.jsonl"
