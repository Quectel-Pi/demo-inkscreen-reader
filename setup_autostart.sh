#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${SUDO_USER:-$USER}"
SERVICE_NAME="inkscreen-reader"
EPD_BIN="$PROJECT_DIR/components/e-Paper/Quectel-Pi-H1/c/epd"

echo "=== Configure $SERVICE_NAME autostart service ==="

# Grant NOPASSWD sudo for EPD binary (required for systemd service)
SUDOERS_FILE="/etc/sudoers.d/${SERVICE_NAME}"
echo "$USER_NAME ALL=(root) NOPASSWD: $EPD_BIN" | sudo tee "$SUDOERS_FILE" > /dev/null
sudo chmod 0440 "$SUDOERS_FILE"

# Create the systemd service file
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
sudo tee "$SERVICE_FILE" > /dev/null << EOF
[Unit]
Description=Inkscreen E-ink Reader
After=network.target

[Service]
Type=simple
User=$USER_NAME
WorkingDirectory=$PROJECT_DIR
ExecStart=$PROJECT_DIR/build.sh
Restart=on-failure
RestartSec=10
TimeoutStopSec=10
Environment="DISPLAY=:0"
SupplementaryGroups=video

[Install]
WantedBy=multi-user.target
EOF

# Enable and start the service
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"
sudo systemctl start "$SERVICE_NAME"

echo "Done! Service status:"
sudo systemctl status "$SERVICE_NAME" --no-pager
echo ""
echo "Management commands:"
echo "  Check status:   sudo systemctl status $SERVICE_NAME"
echo "  Stop service:   sudo systemctl stop $SERVICE_NAME"
echo "  Restart service: sudo systemctl restart $SERVICE_NAME"
echo "  Disable autostart: sudo systemctl disable $SERVICE_NAME"
