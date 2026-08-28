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

SELF="$SRC/$MACHINE.jsonl"
if [ ! -f "$SELF" ]; then
  echo "no $SELF" >&2
  if ls "$SRC"/*.jsonl >/dev/null 2>&1; then
    echo "The directory holds other machines' files but not this machine's." >&2
    echo "Either installation_id was just changed (the running Squirrel is" >&2
    echo "still writing the old name until it is restarted) or nothing has" >&2
    echo "been typed since telemetry was enabled." >&2
  else
    echo "Nothing has been recorded on this machine. Type some Chinese and" >&2
    echo "run this again." >&2
  fi
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

# ONLY this machine's file, which is what the plugin's own auto_sync does
# (telemetry.cc's SyncToDir enumerates <machine>.jsonl and its generations and
# never globs). This used to be `cp "$SRC"/*.jsonl`, and the difference is not
# cosmetic: a local directory can hold another machine's file -- left behind by
# a rename, since the filename comes from installation_id -- and globbing
# republishes it over whatever the shared copy has become. On 2026-08-28 the
# shared MacBookPro-M1.jsonl was merged into MacBookAir-M4.jsonl after it turned
# out to be the Air's own data under a stale name; the Air's local copy still
# exists, and one glob would have undone that merge and reported success.
# CLAUDE.md's rule for this directory is "one file per machine, so collecting is
# concatenation" -- a machine publishes its own file and nothing else.
echo "copying from $SRC"
name="$MACHINE.jsonl"
cp "$SELF" "$DEST/$name"
# Compare after the copy rather than trusting cp's exit status alone: the
# destination is an iCloud-backed directory, where a write can be accepted
# and then not be what you wrote.
src_size=$(wc -c < "$SELF" | tr -d ' ')
dst_size=$(wc -c < "$DEST/$name" | tr -d ' ')
if [ "$src_size" != "$dst_size" ]; then
  echo "  $name: copied $src_size bytes but destination has $dst_size" >&2
  exit 1
fi
v=$(last_v "$SELF")
[ -n "$v" ] || v="?"
echo "  $name  $src_size bytes  last line v$v  <- this machine"
echo "copied 1 file to $DEST"

# Named, not copied. A file here under another machine's name is nearly always
# this machine's own data written before installation_id was corrected, and
# saying so beats leaving it to be rediscovered.
for f in "$SRC"/*.jsonl; do
  [ -e "$f" ] || continue
  other=$(basename "$f")
  [ "$other" != "$name" ] || continue
  echo "  $other  NOT copied -- not this machine's name."
  echo "    If this machine was renamed, these lines are yours under the old"
  echo "    name; merge them into $name rather than publishing them."
done

echo
echo "run the report with:"
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
# SYNC_DIR is computed here and is not exported to the caller's shell, so a
# documented command that referenced it would expand to nothing and produce an
# empty glob -- which looks exactly like telemetry being broken.
echo "  $SELF_DIR/analyze_telemetry.py \"$DEST\"/*.jsonl"
