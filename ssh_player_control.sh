#!/usr/bin/env bash
set -euo pipefail

FIFO="${M5_STREAMPLAYER_CONTROL:-/tmp/m5streamplayer-control}"

if [ ! -p "$FIFO" ]; then
    mkfifo "$FIFO"
    chmod 666 "$FIFO"
fi

echo "M5 StreamPlayer SSH control"
echo "Arrows/F/X/Z/C move, Enter play, Space pause/detail, O output, N next, M menu, S sort, Esc back."
echo "Ctrl-C exits this SSH controller only."
echo
echo "Waiting for StreamPlayer to open $FIFO ..."
exec 3>"$FIFO"
echo "Connected."

old_tty="$(stty -g)"
cleanup() {
    stty "$old_tty" 2>/dev/null || true
    echo
}
trap cleanup EXIT INT TERM

stty raw -echo min 0 time 1

send_key() {
    printf '%s\n' "$1" >&3
}

while true; do
    key=""
    IFS= read -rsn1 key || true
    [ -n "$key" ] || continue

    case "$key" in
        $'\x03')
            exit 0
            ;;
        $'\x1b')
            seq=""
            IFS= read -rsn2 -t 0.04 seq || true
            case "$seq" in
                "[A") send_key "UP" ;;
                "[B") send_key "DOWN" ;;
                "[C") send_key "RIGHT" ;;
                "[D") send_key "LEFT" ;;
                "") send_key "ESC" ;;
                *) send_key "ESC" ;;
            esac
            ;;
        $'\r'|$'\n')
            send_key "ENTER"
            ;;
        " ")
            send_key "SPACE"
            ;;
        $'\t')
            send_key "TAB"
            ;;
        *)
            send_key "$key"
            ;;
    esac
done
