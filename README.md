# am2301

Updated to work with modern Raspberry PI Linux kernel, and updated reading code from Adafruit's DHT library to fix reading bugs

Kernel module for AM2301 (DHT21) temperature/relative humidity sensor.

This is a driver for Raspberry PI.
It uses GPIO24 as default, GPIO is configurable as module parameter (gpio).

The driver uses procfs interface (/proc/am2301), 
reporting T, RH and read status [OK/error, checksum/error, no data]

## Deploy
1. Install kernel headers using apt-get.
2. Run make in the folder.
3. Run `sudo insmod am2301.ko` to install the module.
4. Add `am2301` to your `/etc/modules` to make the module auto-load on reboot.

If you need to rebuild the module, run `sudo rmmod am2301` to remove the old, then insmod to add the new.
