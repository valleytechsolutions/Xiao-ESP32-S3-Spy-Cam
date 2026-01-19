# Xiao-ESP32-S3-Spy-Cam
A mini Nanny cam for the Seeed Studio XIAO ESP32-S3 Sense
# XIAO ESP32-S3 Sense Security Camera (MJPEG Stream + SD Recording + Motion + One-Click Download)

This project turns a **Seeed Studio XIAO ESP32-S3 Sense** into a small security camera that:
- Serves a **live MJPEG stream** in your browser
- Records JPEG frames to **microSD** (clip folders)
- Optionally records **only on motion**
- Lets you **download a full clip with one click** (as a `.tar` archive)

---

## What you need

### Hardware
- [Seeed Studio **XIAO ESP32-S3 Sense**](https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html)
- microSD card (8–32GB recommended)
  - **Must be FAT32**
- microSD card reader (for your PC)
- USB-C cable (data capable)
- (If you are using an external SPI microSD module instead of a shield/board) proper wiring

### Software
- **Arduino IDE 2.x** (recommended)
- USB driver support (usually automatic on Windows/macOS)
- ESP32 Arduino core installed via Boards Manager

---

## Project overview

### Web endpoints
- `/` : main UI page
- `/stream` : live MJPEG video stream
- `/status` : JSON status
- `/control` : controls recording and motion toggles
- `/clips` : list stored clips/frames
- `/download?clip=current` : download the current clip as a TAR

---

## Step-by-step Arduino IDE install guide

### 1) Format the microSD card (important)
1. Insert the microSD card into your PC.
2. Format as **FAT32**.
   - Windows: use “Format…” → File system **FAT32** (if FAT32 isn’t shown, use a FAT32 formatter tool).
   - macOS: Disk Utility → Erase → **MS-DOS (FAT)** (this is FAT32) → Master Boot Record.

---

### 2) Install Arduino IDE
1. Download and install Arduino IDE 2.x.
2. Open Arduino IDE.

---

### 3) Add ESP32 boards to Arduino IDE (Boards Manager)
1. In Arduino IDE: **File → Preferences**
2. In “Additional boards manager URLs” add this (if you already have others, separate with commas):
