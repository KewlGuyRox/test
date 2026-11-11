#!/bin/sh
# Blink one candidate GPIO at a time so a human can see which pad really drives
# the panel backlight, enable or reset. Nothing is written to flash and no pin
# outside the safe list is touched:
#   6 (panel CS), 7-10 (SPI flash), 11 (PPS input), 12-13 (serial console),
#   38 (reset key), 44 (status LED), 45-46 (uart1), 4-5 (i2c) are all excluded.
# If the console or the network dies, power-cycle: this image is RAM only.
base=512
safe="0 1 2 3 14 15 16 17 18 19 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 39 40 41 42 43"
echo "watch the glass and the backlight; each pad is high for 1 s then low for 1 s"
for off in $safe; do
	g=$((base + off))
	if [ ! -d "/sys/class/gpio/gpio$g" ]; then
		echo "$g" > /sys/class/gpio/export 2>/dev/null || continue
		sleep 1
	fi
	echo out > "/sys/class/gpio/gpio$g/direction" 2>/dev/null || continue
	echo "=== pad $off (gpio$g): HIGH ==="
	echo 1 > "/sys/class/gpio/gpio$g/value"
	sleep 1
	echo "=== pad $off (gpio$g): LOW ==="
	echo 0 > "/sys/class/gpio/gpio$g/value"
	sleep 1
done
echo "scan done"
