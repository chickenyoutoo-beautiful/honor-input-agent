#!/system/bin/sh
# HonorInputAgent helper script.
# Installs the two relay daemons and starts/stops them as a unit. Stopping the
# daemons releases the physical devices, so normal Android input comes back.

TMP=/data/local/tmp
TP=$TMP/touchpad_relay
KBD=$TMP/kbd_relay

case "$1" in
  install)
    cp -f "$2" $TP 2>/dev/null
    cp -f "$3" $KBD 2>/dev/null
    chmod 755 $TP $KBD
    echo installed
    ;;
  start)
    pkill -f touchpad_relay 2>/dev/null
    pkill -f kbd_relay 2>/dev/null
    sleep 1
    setsid $TP >$TMP/touchpad_relay.log 2>&1 </dev/null &
    setsid $KBD >$TMP/kbd_relay.log 2>&1 </dev/null &
    sleep 1
    echo started
    ;;
  stop)
    pkill -f touchpad_relay 2>/dev/null
    pkill -f kbd_relay 2>/dev/null
    sleep 1
    echo stopped
    ;;
  status)
    t=$(ps -A -o NAME 2>/dev/null | grep -c '^touchpad_relay$')
    k=$(ps -A -o NAME 2>/dev/null | grep -c '^kbd_relay$')
    echo "$t $k"
    ;;
  *)
    echo "usage: agent.sh install <touchpad> <keyboard> | start | stop | status"
    ;;
esac
