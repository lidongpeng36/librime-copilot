#!/bin/sh
# Report the active tmux pane's identity to the running Rime copilot plugin.
#
# Invoked from .tmux.conf hooks (see README, "Context memory"). Writes one JSON
# Lines message to the IME Bridge socket and exits. It carries NO ascii_mode
# decision: the plugin owns the memory table, this only says where the caret is.
#
# Deliberately a `type: "identity"` message and not an `ascii` one -- an ascii
# message registers a bridge client, and this script connects and disconnects
# on every single pane switch, so a registered client would have a reset
# synthesized on each disconnect and flip ascii_mode on an idle machine.
set -eu

SOCK="${RIME_COPILOT_IME_SOCK:-/tmp/rime_copilot_ime.sock}"
[ -S "$SOCK" ] || exit 0

# Told, not asked. tmux expands `#{pane_id}` in a hook's command against the
# HOOK's own target; `display-message -p` with no `-t` does NOT -- it resolves
# against the client that invoked the command. Measured on tmux 3.7c
# 2026-08-31: `tmux select-window -t copilot:3` issued from a pane in another
# session fired this hook, and the hook's own `display-message -p` answered
# `librime:1 %3 claude` instead of `%5`. Plain `run-shell` did it too, so it is
# not a race with `-b`. Interactive prefix-key switches happen to be correct
# only because there the invoking client IS the client being switched.
#
# $TMUX is the exception: the hook's environment carries the hook target's own
# TMUX (verified in the same session -- session id differed from the invoking
# shell's), so the derived socket was right all along. It stays as the fallback
# for a tmux too old to report #{socket_path}.
pane="${1:-}"
command="${2:-}"
socket_arg="${3:-}"

if [ -z "$pane" ]; then
  # No arguments: a hand-run invocation, or a .tmux.conf written before
  # 2026-08-31. Ask, and be wrong in the narrow scripted-switch case rather
  # than silently do nothing.
  TMUX_BIN="${TMUX_BIN:-tmux}"
  line=$("$TMUX_BIN" display-message -p -F '#{pane_id}|#{pane_current_command}' 2>/dev/null) || exit 0
  pane=${line%%|*}
  command=${line#*|}
fi
[ -n "$pane" ] || exit 0

# ${TMUX} unset would abort the whole script under `set -u`, and a hook that
# aborts makes every pane switch look broken. Default it, then derive.
#
# The FULL socket path, not its basename. Both identity rungs must build the
# same key for the same pane or a machine orphans everything it remembered
# (see the design's "The key"), and the polled rung keys on the socket path --
# `#{socket_path}`, or `copilot/tmux_source/socket` when tmux is too old to
# report it. `basename` disagreed with that for any non-default socket and
# additionally collided /tmp/tmux-501/default with /tmp/tmux-1000/default.
socket="$socket_arg"
if [ -z "$socket" ]; then
  TMUX_ENV="${TMUX:-}"
  if [ -n "$TMUX_ENV" ]; then
    socket=${TMUX_ENV%%,*}
  fi
fi

# `-w 1` is load-bearing and is NOT a timeout for slow networks: macOS's nc
# does not exit on stdin EOF, it waits for the remote to close, and
# ImeBridgeServer::HandleConnection never closes -- it only unwinds when the
# client hangs up. Without a timeout every pane switch leaks an nc process, a
# detached thread parked in read(), an entry in client_fds_ and an fd inside
# Squirrel, reclaimed only by restarting it. Do NOT "improve" this to `-N`:
# macOS's -N takes an argument, swallows -U, and the reporter stops working
# entirely (`nc: invalid tcp adaptive write timeout value`, exit 1).
printf '{"v":1,"ns":"rime.ime","type":"identity","data":{"socket":"%s","pane":"%s","command":"%s"}}\n' \
  "$socket" "$pane" "$command" | nc -w 1 -U "$SOCK" >/dev/null 2>&1 &
exit 0
