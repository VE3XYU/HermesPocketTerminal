# Flashing from a PC with USB access

1. `python3 -m pip install esptool pyserial` (once).
2. Fetch `firmware/build/flash-pack.zip` from the build host and unzip it.
3. Put the device in download mode if needed (hold the Record/BOOT button while
   plugging in USB), find the serial port (Windows: Device Manager COMx;
   macOS/Linux: `ls /dev/tty.*` or `/dev/ttyACM*`).
4. Run the command in `FLASH-COMMAND.txt`, adding `--port <PORT>`.
5. Serial monitor: `python3 -m serial.tools.miniterm <PORT> 115200`
   (exit: Ctrl-]). Copy/paste the log lines back into the session.
