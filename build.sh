#!/bin/bash

# ebook reader startup script
# Runs both Python eye tracking control script and E-ink display program

# Define variables to store child process PIDs
PYTHON_PID=0
EPD_PID=0

# Exit handling function
cleanup() {
    echo ""
    echo "Received exit signal, terminating all child processes..."
    
    # Terminate Python process
    if [ $PYTHON_PID -ne 0 ] && kill -0 $PYTHON_PID 2>/dev/null; then
        echo "Terminating Python eye tracking control script (PID: $PYTHON_PID)"
        kill -TERM $PYTHON_PID 2>/dev/null
        wait $PYTHON_PID 2>/dev/null
    fi
    
    # Terminate EPD process
    if [ $EPD_PID -ne 0 ] && kill -0 $EPD_PID 2>/dev/null; then
        echo "Terminating E-ink display program (PID: $EPD_PID)"
        kill -TERM $EPD_PID 2>/dev/null
        wait $EPD_PID 2>/dev/null
    fi
    
    echo "All programs terminated"
    exit 0
}

# Capture exit signals
trap cleanup SIGINT SIGTERM

echo "Starting ebook reader system..."

# Get the absolute path of the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Go to ebook-reader directory
cd "$SCRIPT_DIR"
# ======================
# Single-instance guard: stop the previous run before starting a new one
# ======================
# SINGLE_INSTANCE_PATCHED
RUN_DIR="$SCRIPT_DIR/.run"
PID_FILE="$RUN_DIR/build.pid"
[ -d "$RUN_DIR" ] || mkdir -p "$RUN_DIR"

project_pids() {
    local pids="" p
    if [ -f "$PID_FILE" ]; then
        p="$(tr -dc '0-9' < "$PID_FILE" 2>/dev/null)"
        if [ -n "$p" ] && [ "$p" != "$$" ] && kill -0 "$p" 2>/dev/null; then
            pids="$pids $p"
        fi
    fi
    # exact project paths: trust the command line (also works for the
    # root-owned sudo wrapper of the e-Paper program)
    for p in $(pgrep -f -- "$SCRIPT_DIR/src/main.py" 2>/dev/null) \
             $(pgrep -f -- "$SCRIPT_DIR/components/e-Paper/Quectel-Pi-H1/c/epd" 2>/dev/null); do
        pids="$pids $p"
    done
    # loose patterns (older runs with relative paths): require matching cwd
    for p in $(pgrep -f -- 'src/main\.py' 2>/dev/null) \
             $(pgrep -f -- 'Quectel-Pi-H1/c/epd' 2>/dev/null); do
        if [ "$(readlink -f "/proc/$p/cwd" 2>/dev/null)" = "$SCRIPT_DIR" ]; then
            pids="$pids $p"
        fi
    done
    for p in $pids; do echo "$p"; done | sort -u | tr '\n' ' '
}

stop_previous_instance() {
    local pids p alive i
    pids="$(project_pids)"
    if [ -z "$(echo $pids)" ]; then
        rm -f "$PID_FILE"
        return 0
    fi
    echo "Previous instance running (PID:$pids) - stopping it first..."
    kill -TERM $pids 2>/dev/null
    i=0
    while [ $i -lt 25 ]; do
        alive=""
        for p in $pids; do
            kill -0 "$p" 2>/dev/null && alive="$alive $p"
        done
        [ -z "$alive" ] && break
        sleep 1
        i=$((i + 1))
    done
    if [ -n "$alive" ]; then
        echo "Still alive after 25s, force killing:$alive"
        kill -KILL $alive 2>/dev/null
        sleep 1
    fi
    rm -f "$PID_FILE"
    sleep 1   # give the kernel a moment to release camera / SPI / port 8088
    echo "Previous instance stopped."
}

case "${1:-}" in
    stop|--stop)
        stop_previous_instance
        echo "Done."
        exit 0
        ;;
esac

stop_previous_instance
echo $$ > "$PID_FILE"



# Create log directory
mkdir -p logs

# Start Python eye tracking control script (run in background)
echo "Starting Python eye tracking control script..."
PYBIN="/home/pi/.pyenv/versions/3.10.15/bin/python3"
[ -x "$PYBIN" ] || PYBIN="${HOME:-/home/pi}/.pyenv/versions/3.10.15/bin/python3"
[ -x "$PYBIN" ] || PYBIN="$(command -v python3)"
"$PYBIN" "$SCRIPT_DIR/src/main.py" > logs/python_eye_tracking.log 2>&1 &
PYTHON_PID=$!

# Check if Python process started successfully
if ! kill -0 $PYTHON_PID 2>/dev/null; then
    echo "Error: Failed to start Python eye tracking control script"
    exit 1
fi

# Start E-ink display program (run in background)
echo "Starting E-ink display program..."
sudo "$SCRIPT_DIR/components/e-Paper/Quectel-Pi-H1/c/epd" > logs/epd_program.log 2>&1 &
EPD_PID=$!

# Check if EPD process started successfully
if ! kill -0 $EPD_PID 2>/dev/null; then
    echo "Error: Failed to start E-ink display program"
    kill $PYTHON_PID 2>/dev/null
    exit 1
fi

echo "Programs started:"
echo "  - Python eye tracking control script PID: $PYTHON_PID"
echo "  - E-ink display program PID: $EPD_PID"
echo "Press Ctrl+C to exit"

# Stop children gracefully so the e-Paper panel gets cleared on exit
# STOP_TRAP_PATCHED
stop_children() {
    echo ""
    echo "Stopping programs (e-Paper screen will be cleared)..."
    # SIGTERM: children of a non-interactive shell ignore SIGINT
    kill -TERM "$EPD_PID" 2>/dev/null
    kill -TERM "$PYTHON_PID" 2>/dev/null
    wait "$EPD_PID" 2>/dev/null
    wait "$PYTHON_PID" 2>/dev/null
    rm -f "$PID_FILE"
    echo "All programs stopped."
}
trap stop_children INT TERM HUP

# Wait for all child processes to finish
wait $PYTHON_PID $EPD_PID 2>/dev/null

echo "All programs completed"