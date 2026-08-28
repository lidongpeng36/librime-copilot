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
#
# WHY THIS SCRIPT REPORTS SO MUCH. It used to be `cp "$SRC"/*.jsonl "$DEST"/
# 2>/dev/null || true` followed by an unconditional `echo "copied to $DEST"`,
# which printed success having copied nothing whenever the source directory was
# empty or absent -- the exact failure shape ("reports success having never
# done the thing") this project keeps meeting in other forms. It cost a real
# diagnosis on 2026-08-28: a machine that had just been upgraded appeared not
# to be syncing, and the script's own output could not distinguish "iCloud has
# not propagated it yet" from "there was nothing to copy". So: refuse when
# there is nothing, name this machine's own file specifically, and print the
# schema version of each file's LAST line, which is what dates the running
# dylib (see CLAUDE.md, "copying the dylib is not loading it").
set -eu

RIME_DIR="${RIME_DIR:-$HOME/Library/Rime}"
SRC="$RIME_DIR/private/copilot_telemetry"
INSTALLATION="$RIME_DIR/installation.yaml"

# Values in installation.yaml may or may not be quoted (Squirrel writes
# `installation_id: "Mac-Mini"` but `distribution_version: 1.1.2`), so strip
# optional quotes rather than requiring them.
read_key() {
  sed -n "s/^$1: *\"\{0,1\}\([^\"]*\)\"\{0,1\} *\$/\1/p" "$INSTALLATION"
}

if [ ! -f "$INSTALLATION" ]; then
  echo "no $INSTALLATION -- is RIME_DIR right? (currently $RIME_DIR)" >&2
  exit 1
fi

SYNC_DIR=$(read_key sync_dir)
if [ -z "$SYNC_DIR" ]; then
  echo "no sync_dir in $INSTALLATION" >&2
  echo "Squirrel never writes this key; add it by hand." >&2
  exit 1
fi

# The same name the plugin writes under: telemetry::Writer is constructed with
# deployer.user_id (copilot_engine.cc), which IS installation_id. Deriving it
# the same way is what lets this script say "your own machine's file is
# missing" instead of only "some files were copied".
MACHINE=$(read_key installation_id)
[ -n "$MACHINE" ] || MACHINE=unknown

if [ ! -d "$SRC" ]; then
  echo "no $SRC" >&2
  echo "Nothing has ever been recorded on this machine. Either" >&2
  echo "copilot/telemetry/enable is false in the schema, or the running" >&2
  echo "Squirrel predates telemetry -- check \`rime-copilot status\` and that" >&2
  echo "the dylib in Squirrel.app is the one you built (a copy is not a load;" >&2
  echo "the process must be killed)." >&2
  exit 1
fi

# Deliberately NOT `|| true`: an unreadable source is a failure to report, not
# a state to paper over. The glob is expanded once, into a counted list.
FOUND=0
for f in "$SRC"/*.jsonl; do
  [ -e "$f" ] || continue
  FOUND=$((FOUND + 1))
done
if [ "$FOUND" -eq 0 ]; then
  echo "no *.jsonl in $SRC -- nothing to copy" >&2
  echo "The directory exists but is empty, so telemetry is configured and has" >&2
  echo "written nothing. Type some Chinese and run this again." >&2
  exit 1
fi

# Flat, unlike $SRC: the local copy lives under private/ because
# $RIME_DIR is commonly a git repo (private/ is the conventional gitignore
# line for it); the sync dir is not a git repo, so private/ would buy
# nothing there and a flat name is clearer.
DEST="$SYNC_DIR/copilot_telemetry"
mkdir -p "$DEST"
# Not into $SYNC_DIR/<installation_id>/, which Rime's own sync task manages.

# `v` of the LAST line, not any line: a file is appended to for months and
# carries a spread of old versions, so a plain grep reads as a stale image when
# nothing is stale (measured: {2: 3, 3: 26, 5: 3} on a machine running v5
# throughout). The last line dates the image that is running now.
last_v() {
  tail -1 "$1" 2>/dev/null | sed -n 's/.*"v":[ ]*\([0-9][0-9]*\).*/\1/p'
}

echo "copying from $SRC"
COPIED_SELF=0
for f in "$SRC"/*.jsonl; do
  [ -e "$f" ] || continue
  name=$(basename "$f")
  cp "$f" "$DEST/$name"
  # Compare after the copy rather than trusting cp's exit status alone: the
  # destination is an iCloud-backed directory, where a write can be accepted
  # and then not be what you wrote.
  src_size=$(wc -c < "$f" | tr -d ' ')
  dst_size=$(wc -c < "$DEST/$name" | tr -d ' ')
  if [ "$src_size" != "$dst_size" ]; then
    echo "  $name: copied $src_size bytes but destination has $dst_size" >&2
    exit 1
  fi
  v=$(last_v "$f")
  [ -n "$v" ] || v="?"
  if [ "$name" = "$MACHINE.jsonl" ]; then
    COPIED_SELF=1
    echo "  $name  $src_size bytes  last line v$v  <- this machine"
  else
    echo "  $name  $src_size bytes  last line v$v"
  fi
done
echo "copied $FOUND file(s) to $DEST"

if [ "$COPIED_SELF" -eq 0 ]; then
  echo >&2
  echo "!!  $MACHINE.jsonl was NOT among them. This machine's own telemetry is" >&2
  echo "    not in the sync directory -- everything copied above came from" >&2
  echo "    somewhere else (a restore, most likely). Nothing this machine typed" >&2
  echo "    will reach the analysis." >&2
  exit 1
fi

echo
echo "run the report with:"
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
# SYNC_DIR is computed here and is not exported to the caller's shell, so a
# documented command that referenced it would expand to nothing and produce an
# empty glob -- which looks exactly like telemetry being broken.
echo "  $SELF_DIR/analyze_telemetry.py \"$DEST\"/*.jsonl"
