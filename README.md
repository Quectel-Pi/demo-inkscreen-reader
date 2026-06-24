# E-Ink Reader

[中文](README_zh.md) | English

## Project Overview

This project is a smart e-ink screen reader based on the [Quectel Pi H1 Smart Single-Board Computer](https://developer.quectel.com/doc/sbc/Quectel-Pi-H1/en/Applications/Open-Source-Projects/e_ink_reader/e_ink_reader.html). The system combines the low-power display characteristics of e-ink screens with camera-based eye-tracking technology to achieve a natural page-turning reading experience without manual operation. Page turning is controlled by detecting changes in the user's eye gaze, and physical buttons are used as auxiliary input to improve system reliability.

In terms of display, the system adopts a partial refresh and partition rendering strategy, supports automatic typesetting and continuous reading of Chinese and English text, and also has page memory and quick wake-up functions, making it suitable for long-term reading and embedded smart terminal application scenarios.

![Interface Preview](assets/main_reader.jpg)

## 🌟 Core Features

| Feature | Description |
|---------|-------------|
| **Eye-tracking Page Turning** | Page turning is achieved by detecting eye movement direction. When reaching the bottom of the reader, simply looking towards the top of the screen triggers the page turn |
| **Smart Screen Off** | Automatically turns off the screen after a set time if no face is detected, protecting privacy and saving battery power |
| **Multi-language Support** | Supports correct rendering of pure English, pure Chinese (GB2312), and mixed Chinese-English text |
| **Automatic Typesetting** | No character cropping, automatic line wrapping, supports cross-page content continuation, Chinese first-line indentation |
| **Page Memory** | Supports returning to previous pages with pixel-level consistency, accurately recording reading position |
| **Multi-book Management** | Supports switching between different books via long-press physical buttons |
| **Efficient Refresh** | Uses partial refresh technology to reduce flickering and improve refresh speed |

## 👁️ Eye-tracking Control Instructions

### Startup Process
1. Run `./build.sh`, the script simultaneously launches the eye-tracking script and e-ink display program
2. The camera automatically detects available devices and begins monitoring eye movements
3. Initialization takes 4 seconds - maintain normal reading posture during this period

### Page Turning Operations
- **Next Page**: Maintain a reading posture and read at a normal pace, starting from the top of the screen and moving downwards. When your gaze reaches the bottom of the screen, simply shift your eyes back to the top to trigger the page turn
- **Previous Page**: Requires physical button operation (short press KEY2)
- **Page Turn Cooldown**: 1-second cooldown between page turns to prevent accidental triggers

### Screen Off/Wake-up Function
- **Auto Screen Off**: Automatically sends screen-off signal after 60 seconds of no face detection
- **Auto Wake-up**: Automatically wakes the screen when face is detected again
- **Event Cleanup**: Clears input events during screen-off period upon wake-up to prevent accidental page turns

## ⌨️ Physical Button Functions

| Operation | Button | Function |
|-----------|--------|---------|
| Short Press | KEY1 | Turn to next page |
| Short Press | KEY2 | Turn to previous page |
| Long Press | KEY1 | Switch to next book |
| Long Press | KEY2 | Switch to previous book |

## 🛠️ System Requirements

### Hardware Requirements
- **Main Controller**: Quectel Pi H1 Smart Single-Board Computer
- **Display**: Waveshare 7.5" Black and White E-Ink Display
- **Camera**: OV5693 USB Camera (for eye tracking)
### E-Ink Display Pin Connections
| EPD Pin | BCM2835 Numbering | Board Physical Pin |
|---------|-------------------|--------------------|
| VCC     | 3.3V              | 3.3V               |
| GND     | GND               | GND                |
| DIN     | MOSI              | 19                 |
| CLK     | SCLK              | 23                 |
| CS      | CE0               | 24                 |
| DC      | 25                | 22                 |
| RST     | 17                | 11                 |
| BUSY    | 24                | 18                 |
| PWR     | 18                | 12                 |
### Software Requirements

- Operating System: Debian 13 (Quectel Pi H1 default system)
- Python version: Python 3.9~3.12
- Dependencies:
    - OpenCV-Python == 4.8.1.78
    - MediaPipe == 0.10.9
    - evdev == 1.9.2
    - numpy == 1.24.3

## 🚀 Complete Deployment Guide

> The one-click deployment script `deploy.sh` can complete all environment configuration from scratch on a fresh system, no manual steps required.

### Project Implementation

1. Open a terminal on the smart board and clone the project code with git.

```shell
sudo apt update
sudo apt install -y git
git clone https://github.com/Quectel-Pi/demo-inkscreen-reader.git
```

After execution, a `demo-inkscreen-reader` folder should be created in the current directory.

2. Run the `deploy.sh` deployment script. If the terminal shows `Deployment complete`, deployment is successful.

```bash
cd demo-inkscreen-reader
sudo chmod 755 deploy.sh
./deploy.sh
```

If an error occurs while executing `./deploy.sh`, or if the terminal does not display `Deployment complete`, do not continue with the following steps. Check whether the network connection is normal, then return to the project root directory and run `deploy.sh` again.

3. After deployment is complete, reboot the device first.

```bash
sudo reboot
```

4. After rebooting, open the terminal again and run the program in the `demo-inkscreen-reader` directory.

```bash
cd demo-inkscreen-reader
sudo chmod 755 build.sh
./build.sh
```

### Prepare Book Files

Place `.txt` files into the `demo-inkscreen-reader/books/` directory. The system automatically recognizes UTF-8 and GB2312 encodings.

> Tip: For Windows users, in Notepad choose Save As, then select UTF-8 or ANSI (GB2312) encoding.

### Compilation Notes

The EPD e-paper driver is written in C and built with Makefile:

```bash
cd demo-inkscreen-reader/components/e-Paper/Quectel-Pi-H1/c
make clean
make CC=gcc EPD=epd7in5V2
```
The build output is the `epd` executable, which must be run with `sudo` privileges (GPIO access required).

### Enable Auto-start at Boot (Optional)

This project provides `setup_autostart.sh` to configure a system-level service.

```bash
cd ~/demo-inkscreen-reader
sudo chmod 755 setup_autostart.sh
./setup_autostart.sh
```

Service management:

```bash
sudo systemctl status inkscreen-reader
sudo systemctl restart inkscreen-reader
sudo systemctl stop inkscreen-reader
sudo systemctl disable inkscreen-reader
```

### Font Generation (Optional)

If you need custom fonts, use `tools/generate_noto_font12cn.py` to extract a bitmap font of a specified size from a Chinese font file.


## 📁 Directory Structure

```
demo-inkscreen-reader/
├── README.md                              # Project description (English)
├── README_zh.md                           # Project description (Chinese)
├── build.sh                               # Startup script
├── deploy.sh                              # One-click deployment script
├── setup_autostart.sh                     # Auto-start setup script
├── requirements.txt                       # Python dependency list
├── assets/
│   └── main_reader.jpg                    # Main interface preview
├── books/                                 # Book files directory
│   ├── test_cn.txt                        # Chinese test book
│   └── test_en.txt                        # English test book
├── components/
│   └── e-Paper/
│       └── Quectel-Pi-H1/
│           └── c/                         # C driver source
│               ├── epd                    # Compiled executable
│               ├── Makefile
│               ├── examples/
│               │   └── EPD_7in5_V2_reader_txt.c  # Main display program
│               ├── lib/                   # Driver libraries (Config/GUI/e-Paper/Fonts)
│               └── pic/
│                   └── 2.bmp              # Screen-off image
├── src/
│   └── main.py                            # Python eye-tracking main program
└── tools/
    └── generate_noto_font12cn.py          # Font generation tool
```


## 🔍 Troubleshooting

| Issue | Solution |
|-------|----------|
| Camera cannot open | Check device permissions, use `ls /dev/video*` to confirm device node exists |
| Eye-tracking unresponsive | Check if camera is occupied by other programs, verify MediaPipe installation |
| Screen no display or abnormal | Check SPI connection stability and GPIO configuration |
| Buttons not working | Use `cat /proc/bus/input/devices` to find event device and check permissions |
| Compilation failure | Check cross-compilation toolchain existence and path |

## Reporting Issues
We welcome Issues and Pull Requests to improve this project.