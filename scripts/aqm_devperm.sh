#!/bin/sh
# aqm starts before udev has applied device permissions (udev runs late on
# this board). Set the same groups/modes its rules would, then start.
chgrp gpio /dev/i2c-2 && chmod 0660 /dev/i2c-2
chgrp input /dev/input/event* && chmod 0660 /dev/input/event*
chgrp dialout /dev/ttyS1 /dev/ttyS4 && chmod 0660 /dev/ttyS1 /dev/ttyS4
L=/sys/class/leds/aqm:alarm
[ -d "$L" ] && chgrp gpio "$L/brightness" "$L/trigger" && chmod g+w "$L/brightness" "$L/trigger"
exit 0
