#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
PY_VER=3.10.15
VENV_PATH="$HOME/mediapipe_env"
LG_DIR="$PROJECT_DIR/../_deps/lg-master"
EPD_C_DIR="$PROJECT_DIR/components/e-Paper/Quectel-Pi-H1/c"

echo "=== [1/7] System dependencies ==="
sudo apt update
sudo apt install -y make build-essential libssl-dev zlib1g-dev libbz2-dev \
    libreadline-dev libsqlite3-dev curl llvm libncursesw5-dev xz-utils tk-dev \
    libxml2-dev libxmlsec1-dev libffi-dev liblzma-dev libncurses-dev git \
    wget unzip python3-pip python3-setuptools ffmpeg
sudo apt install -y libdouble-conversion3 libxcb-cursor0 || true

echo "=== [2/7] pyenv + Python $PY_VER ==="
if [ ! -d "$HOME/.pyenv" ]; then
    git clone https://github.com/pyenv/pyenv.git "$HOME/.pyenv"
fi
if ! grep -q 'PYENV_ROOT' "$HOME/.bashrc" 2>/dev/null; then
    cat >> "$HOME/.bashrc" <<'EOF'

# pyenv
export PYENV_ROOT="$HOME/.pyenv"
export PATH="$PYENV_ROOT/bin:$PATH"
eval "$(pyenv init --path)"
eval "$(pyenv init -)"
EOF
fi
export PYENV_ROOT="$HOME/.pyenv"
export PATH="$PYENV_ROOT/bin:$PATH"
eval "$(pyenv init --path)"
eval "$(pyenv init -)"
pyenv install --skip-existing "$PY_VER"
pyenv global "$PY_VER"

echo "=== [3/7] LG library ==="
if [ -d "$LG_DIR" ] && [ -f "$LG_DIR/Makefile" ]; then
    cd "$LG_DIR"
else
    cd /tmp && wget https://github.com/joan2937/lg/archive/master.zip -O lg-master.zip
    unzip -o lg-master.zip && cd lg-master
fi
make clean 2>/dev/null; make -j"$(nproc)" && sudo make install

echo "=== [4/7] Install dependencies ==="
pip3 install --upgrade pip
pip3 install -r "$PROJECT_DIR/requirements.txt"

echo "=== [5/7] Build EPD driver ==="
cd "$EPD_C_DIR"
make clean 2>/dev/null || true
make CC=gcc EPD=epd7in5V2

echo "=== [6/7] System configuration (udev/input group/SPI/sudoers) ==="
echo 'KERNEL=="uinput", MODE="0660", GROUP="input"' | sudo tee /etc/udev/rules.d/99-uinput.rules > /dev/null
sudo usermod -aG input video "$USER" 2>/dev/null || true
sudo qpi-config 40pin set 2>/dev/null || echo "  Please run manually: sudo qpi-config 40pin set"
echo "$USER ALL=(ALL) NOPASSWD: $EPD_C_DIR/epd" | sudo tee /etc/sudoers.d/demo-inkscreen-reader > /dev/null
sudo chmod 0440 /etc/sudoers.d/demo-inkscreen-reader

echo "=== [7/7] Permissions ==="
sudo chmod -R 755 "$PROJECT_DIR"

echo ""
echo "Deployment complete! Please execute: sudo reboot"
echo "After reboot, run: cd $PROJECT_DIR && ./build.sh"
