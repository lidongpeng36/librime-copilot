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
# Read this early -- before the "ask tmux" fallback below -- so the bare-
# invocation case can tell local mode from remote mode without waiting on the
# tmux round trip. See the definition below for what a non-empty value means.
ime_host="${4:-}"

if [ -z "$pane" ]; then
  # No arguments: a hand-run invocation, or a .tmux.conf written before
  # 2026-08-31 -- which never had a 4th argument either, so $ime_host is
  # empty here too and this is local mode by construction. Before paying for
  # a `tmux display-message` posix_spawn, fail exactly as this script always
  # did when identity wasn't yet a hook argument: if the local bridge has no
  # socket, there is nothing to send regardless of what tmux would answer, so
  # exit now rather than asking. This does NOT run in remote mode --
  # `ime_host` is only ever non-empty when the hook passed all four
  # arguments, which means `pane` was non-empty too and this branch was never
  # entered -- and a remote host has no such socket to check regardless.
  if [ -z "$ime_host" ]; then
    SOCK="${RIME_COPILOT_IME_SOCK:-/tmp/rime_copilot_ime.sock}"
    [ -S "$SOCK" ] || exit 0
  fi
  # Ask, and be wrong in the narrow scripted-switch case rather than silently
  # do nothing.
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

# The fourth argument ($ime_host, read above) is the machine Rime runs on,
# delivered by tmux as #{E:LC_RIME_IME_HOST}. Empty means "this machine" --
# the local case, and the case for every .tmux.conf written before
# 2026-08-31.
#
# One script, two modes, because one tmux.conf is synced to every machine: the
# hook line has to be correct on the laptop AND on every remote, and a script
# that has to be chosen per machine is a script that will one day be missing
# where the hook expects it.
if [ -z "$ime_host" ]; then
  # ---- local mode: a unix socket on this machine ----
  SOCK="${RIME_COPILOT_IME_SOCK:-/tmp/rime_copilot_ime.sock}"
  [ -S "$SOCK" ] || exit 0
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
fi

# ---- remote mode: a forwarded loopback port ----
#
# A PORT, not a socket file. sshd never unlinks a forwarded socket file and
# refuses to bind over one, so the socket-file form of the tunnel works
# exactly once per host and is dead after.
CACHE="${XDG_RUNTIME_DIR:-/tmp}/rime_ctx_endpoint.$(id -u)"

# Without XDG_RUNTIME_DIR (common on a remote with no systemd user session),
# $CACHE sits directly under the world-writable /tmp, at a path any local
# user can predict (it's just this uid). Anyone can pre-create it pointing at
# their own listener before this script ever runs, and dialling a cache with
# no greeting check (see below) would hand them this host's hostname, tmux
# socket path, pane id and current command with no further prompt. So a
# cache file is trusted only if it is a regular file THIS user owns with no
# group or other write bit -- checked on every read, not just at creation,
# since the file can be replaced out from under an old one between switches.
_cache_is_safe() {
  _f="$1"
  [ -f "$_f" ] || return 1
  # `ls -ldn` over `stat`: `stat`'s flags differ between BSD (`-f`) and GNU
  # coreutils (`-c`), so a single invocation that works on both this laptop
  # and a Linux remote needs a tool whose output format doesn't depend on
  # that. `-n` prints numeric uid/gid so this doesn't need /etc/passwd (or
  # its remote-host equivalent) to agree with `id -u`.
  _ls=$(ls -ldn -- "$_f" 2>/dev/null) || return 1
  set -- $_ls
  _perm="$1"
  _owner_uid="$3"
  [ "$_owner_uid" = "$(id -u)" ] || return 1
  # Permission string is `-rwxrwxrwx`: group write is column 6, other write
  # is column 9 (1-indexed). Either set means untrusted.
  _gw=$(printf '%s' "$_perm" | cut -c6)
  _ow=$(printf '%s' "$_perm" | cut -c9)
  [ "$_gw" = "-" ] && [ "$_ow" = "-" ]
}

# Writes $2 to cache path $1, creating it (or replacing an untrusted one)
# with no group/other permission bits from the start -- refusing an unsafe
# cache on READ is not enough on its own if every successful send keeps
# widening whatever permissions a pre-existing file happened to have.
# `umask 077` inside the subshell scopes the restriction to this one file
# creation instead of the rest of the script.
#
# If an existing file at the path fails _cache_is_safe (wrong owner, or a
# permissive mode), it is unlinked first so ownership can be regained. Best
# effort: on a sticky-bit /tmp, unlinking another user's file is refused by
# the kernel regardless of directory permissions, and the write below then
# only overwrites CONTENT, not ownership -- which is fine, because
# _cache_is_safe rejects that file again on the next read exactly as it does
# now. It never becomes trusted by being written to.
_cache_write() {
  _f="$1"
  _val="$2"
  if [ -e "$_f" ] && ! _cache_is_safe "$_f"; then
    rm -f "$_f" 2>/dev/null || true
  fi
  ( umask 077 && printf '%s' "$_val" > "$_f" ) 2>/dev/null || true
}

# RIME_CTX_HOSTNAME is a test seam and an escape hatch for a machine whose
# `hostname` collides with another remote's.
HOST="${RIME_CTX_HOSTNAME:-$(hostname -s 2>/dev/null || hostname)}"
[ -n "$HOST" ] || exit 0

# `expect` is the recipient. The cache is per-uid and two laptops into one
# remote account share it; a reconnect re-allocates the ephemeral port, so a
# cached port can belong to somebody else's tunnel by the time it is used.
# Naming the recipient makes every send self-verifying with no extra round
# trip -- a misdelivered message is dropped rather than filed.
# Built without the trailing "\n" -- `$()` strips trailing newlines from
# command substitution, so embedding one in the printf format here would
# silently vanish and every remote send would omit the JSON-lines terminator.
# send_to() adds it back at the point the message actually goes out.
MSG=$(printf '{"v":1,"ns":"rime.ime","type":"identity","data":{"expect":"%s","host":"%s","socket":"%s","pane":"%s","command":"%s"}}' \
  "$ime_host" "$HOST" "$socket" "$pane" "$command")

send_to() {
  _ep="$1"
  _h=${_ep%:*}
  _p=${_ep##*:}
  [ -n "$_h" ] && [ -n "$_p" ] || return 1
  printf '%s\n' "$MSG" | nc -w 1 "$_h" "$_p" >/dev/null 2>&1
}

# RIME_IME_ENDPOINT is authoritative, not merely first-tried: it is the
# escape hatch named in the design for a shared macOS remote where the
# candidate_ports() uid check has no /proc to run against, and a user who
# pinned it did so because THEY decided where this goes. Falling through to
# discovery on a failed send here would let that decision undo itself on
# exactly the transient failure it exists to survive -- the window between a
# redeploy and the next keystroke, where the laptop's bridge briefly has no
# socket (see the "no socket" row in the design's Degradation table) -- and
# start probing loopback ports from a host that was told precisely where to
# send. So a pin exits either way, success or failure, and never reaches the
# cache or discovery below.
pinned="${RIME_IME_ENDPOINT:-}"
if [ -n "$pinned" ]; then
  if send_to "$pinned"; then
    _cache_write "$CACHE" "$pinned"
  fi
  exit 0
fi

# Unlike a pin, a cached endpoint keeps the fall-through-on-failure behaviour
# it always had: it is this script's own inference, not the user's explicit
# instruction, so a failed send here means "go find the real one" rather than
# "stop, something is wrong". _cache_is_safe rejects a file this user doesn't
# own or that is group/other writable -- see its definition above -- so a
# hostile or stale-permission cache is treated as though it were simply
# absent, not read.
endpoint=""
if _cache_is_safe "$CACHE"; then
  endpoint=$(tr -d '\n' < "$CACHE" 2>/dev/null)
fi

if [ -n "$endpoint" ]; then
  if send_to "$endpoint"; then
    _cache_write "$CACHE" "$endpoint"
    exit 0
  fi
  # Fall through WITHOUT clearing the cache -- see
  # test_a_failed_send_leaves_the_cached_endpoint_alone.
fi

# Candidate loopback ports. RIME_CTX_PORTS is a test seam and a manual
# override; otherwise enumerate the ports THIS user is listening on.
#
# The uid check is not optional. On a shared host a port belongs to whoever
# binds it first, so without it any other user could squat the endpoint and
# receive what you type. clients/neovim does the same check for the same
# reason.
candidate_ports() {
  if [ -n "${RIME_CTX_PORTS:-}" ]; then
    printf '%s\n' $RIME_CTX_PORTS
    return 0
  fi
  # RIME_CTX_PROC_TCP / RIME_CTX_PORT_RANGE_FILE are test seams, in the same
  # spirit as RIME_CTX_PORTS above: each defaults to the real system path, so
  # production behaviour is unchanged, but a test can point them at a fixture
  # to drive the awk/uid/range logic below for real instead of bypassing it.
  _proc_tcp="${RIME_CTX_PROC_TCP:-/proc/net/tcp}"
  _range_file="${RIME_CTX_PORT_RANGE_FILE:-/proc/sys/net/ipv4/ip_local_port_range}"
  _uid=$(id -u)
  if [ -r "$_proc_tcp" ]; then
    # st 0A is LISTEN; local_address 0100007F is 127.0.0.1; field 8 is uid.
    # The hex is converted in the shell because POSIX awk has no strtonum.
    #
    # Narrowed to the ephemeral range, exactly as the Neovim client does
    # (endpoint.lua's listen_ports/parse_port_range): a bind to port 0 is
    # served from exactly that range, so a long-lived service on a well-known
    # port (8080, 11434, ...) cannot be one of our tunnels and does not
    # deserve a probe.
    #
    # The range is READ, never assumed. Measured 2026-08-31 on this project's
    # own remote: `10240 65535`, not the 32768-60999 default -- and the port
    # sshd actually allocated was 28175, which a hardcoded 32768 floor would
    # have excluded. An unreadable or unparseable range means "do not filter",
    # never "drop everything" -- and unparseable is not the same as unreadable:
    # a readable file whose fields are missing or non-numeric (`read` still
    # succeeds; `_hi` ends up empty or garbage) must be caught too, or
    # `[ "$_p" -le "$_hi" ]` errors ("Illegal number") on every candidate,
    # which drops all of them -- the exact failure this comment warns against.
    # So both fields are validated as non-empty all-digit strings before
    # either is trusted, and if either fails validation both are cleared
    # together: a lo with no hi (or vice versa) is not a usable range.
    _lo=""; _hi=""
    if [ -r "$_range_file" ]; then
      read -r _lo _hi < "$_range_file" || { _lo=""; _hi=""; }
    fi
    case "$_lo" in ''|*[!0-9]*) _lo=""; _hi="" ;; esac
    case "$_hi" in ''|*[!0-9]*) _lo=""; _hi="" ;; esac
    for _hex in $(awk 'NR>1 && $4=="0A" {split($2,a,":"); if (a[1]=="0100007F" && $8==U) print a[2]}' \
                    U="$_uid" "$_proc_tcp" 2>/dev/null); do
      _p=$(( 0x$_hex ))
      if [ -z "$_lo" ] || { [ "$_p" -ge "$_lo" ] && [ "$_p" -le "$_hi" ]; }; then
        printf '%s\n' "$_p"
      fi
    done
  else
    # A macOS remote: no /proc, so the uid check above is unavailable. lsof
    # names the owning PROCESS instead, so restrict to sshd's listeners --
    # which is a stronger filter than uid, not a weaker one. clients/neovim
    # makes the same call; on a shared Mac, pin RIME_IME_ENDPOINT instead.
    lsof -nP -iTCP@127.0.0.1 -sTCP:LISTEN -a -c sshd -u"$_uid" 2>/dev/null \
      | awk 'NR>1 {split($9,a,":"); print a[2]}' | sort -u
  fi
}

# One unprompted greeting line names the machine the bridge runs on.
# ImeBridgeServer writes it the instant it accepts, before reading anything,
# so a client that dialled the wrong tunnel learns so without having sent a
# single keystroke. Field order is nlohmann's (alphabetical), so match the two
# fields independently rather than assuming a layout.
greeting_matches() {
  # Not `... || return 1`: in a pipeline the exit status is the LAST
  # command's (head's), never nc's, so that guard never actually fires --
  # an empty $_line already falls through to "no match" in the case below.
  _line=$(printf '' | nc -w 1 127.0.0.1 "$1" 2>/dev/null | head -1)
  case "$_line" in
    *'"type":"hello"'*) ;;
    *) return 1 ;;
  esac
  case "$_line" in
    *"\"host\":\"$ime_host\""*) return 0 ;;
    *) return 1 ;;
  esac
}

for _port in $(candidate_ports); do
  if greeting_matches "$_port"; then
    endpoint="127.0.0.1:$_port"
    if send_to "$endpoint"; then
      _cache_write "$CACHE" "$endpoint"
    fi
    exit 0
  fi
done

exit 0
