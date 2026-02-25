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

# Create log directory
# mkdir -p logs

# Start Python eye tracking control script (run in background)
echo "Starting Python eye tracking control script..."
#python3 ./src/main.py > logs/python_eye_control.log 2>&1 &
python3 ./src/main.py > /dev/null 2>&1 &
PYTHON_PID=$!

# Check if Python process started successfully
if ! kill -0 $PYTHON_PID 2>/dev/null; then
    echo "Error: Failed to start Python eye tracking control script"
    exit 1
fi

# Start E-ink display program (run in background)
echo "Starting E-ink display program..."
# sudo /home/pi/e-ink-reader/demo-inkscreen-reader/components/e-Paper/Quectel-Pi-H1/c/epd > logs/epd_program.log 2>&1 &
sudo /home/pi/e-ink-reader/demo-inkscreen-reader/components/e-Paper/Quectel-Pi-H1/c/epd > /dev/null 2>&1 &
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

# Wait for all child processes to finish
wait $PYTHON_PID $EPD_PID 2>/dev/null

echo "All programs completed"