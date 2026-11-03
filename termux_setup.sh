#!/data/data/com.termux/files/usr/bin/bash
# One-time setup inside Termux (install Termux from F-Droid, NOT the Play Store).
set -e
pkg update -y && pkg install -y python clang make cmake git binutils
pip install capstone pefile
echo "[+] setup done. Next: ./device_check.sh"
