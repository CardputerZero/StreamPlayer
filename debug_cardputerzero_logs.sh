#!/usr/bin/env bash
set -euo pipefail

CONFIG="${M5_STREAMPLAYER_CONFIG:-/home/pi/.config/m5streamplayer/config.ini}"
APP_LOG="${M5_STREAMPLAYER_APP_LOG:-/tmp/m5streamplayer-app.log}"
PLAYER_LOG="${M5_STREAMPLAYER_PLAYER_LOG:-/tmp/m5streamplayer-player.log}"
CONTROL_FIFO="${M5_STREAMPLAYER_CONTROL:-/tmp/m5streamplayer-control}"

redact() {
    sed -E 's/(api_key=)[^&[:space:]]+/\1<redacted>/g; s/(X-Emby-Token: )[^\r[:space:]]+/\1<redacted>/g'
}

print_state() {
    echo "== process =="
    ps -eo pid,user,ppid,stat,lstart,args | grep -E 'M5CardputerZero|ffplay|ffmpeg|vlc|mpv' | grep -v grep || true
    echo
    echo "== control fifo =="
    ls -l "$CONTROL_FIFO" 2>/dev/null || echo "missing $CONTROL_FIFO"
    echo
    echo "== audio sinks =="
    wpctl status 2>/dev/null | sed -n '/Sinks:/,/Sources:/p' | head -n 30 || true
    echo
    echo "== bluetooth audio =="
    bluetoothctl devices 2>/dev/null || true
    wpctl status 2>/dev/null | grep -i -E 'blue|bluetooth|headset|a2dp' || true
    pw-dump 2>/dev/null | awk -F'"' '/"node.name": "bluez_output\./ { print $4; exit }' || true
    echo

    if [ -f "$CONFIG" ]; then
        local base token
        base="$(awk -F= '$1=="base_url"{print $2}' "$CONFIG" | sed 's:/*$::')"
        token="$(awk -F= '$1=="access_token"{print $2}' "$CONFIG")"
        if [ -n "$base" ] && [ -n "$token" ] && command -v curl >/dev/null 2>&1; then
            echo "== emby sessions =="
            curl -fsS --connect-timeout 3 --max-time 6 \
                -H "X-Emby-Token: $token" \
                "$base/Sessions" 2>/dev/null |
                tr '{' '\n' |
                grep -E 'M5CardputerZero|NowPlayingItem|TranscodingInfo|PlayMethod|VideoCodec|AudioCodec|TranscodeReasons' |
                head -n 80 || true
            echo
        fi
    fi

    echo "== app log =="
    tail -n 80 "$APP_LOG" 2>/dev/null | redact || true
    echo
    echo "== player log =="
    tail -n 80 "$PLAYER_LOG" 2>/dev/null | redact || true
}

case "${1:-tail}" in
    once)
        print_state
        ;;
    tail)
        print_state
        echo
        echo "== live logs =="
        touch "$APP_LOG" "$PLAYER_LOG"
        tail -F "$APP_LOG" "$PLAYER_LOG" | redact
        ;;
    clear)
        : > "$APP_LOG"
        : > "$PLAYER_LOG"
        echo "cleared $APP_LOG and $PLAYER_LOG"
        ;;
    *)
        echo "usage: $0 [tail|once|clear]" >&2
        exit 2
        ;;
esac
