#!/usr/bin/python3
"""This board's 32.768 kHz crystal (Y2) does not oscillate, so the RTC stays
frozen. A frozen RTC never fires the alarm that drives PMIC_POWER_EN low, so
poweroff hangs and ends in a kernel panic. Switch the RTC to the AM335x's
internal 32 kHz clock instead."""
import mmap, os, ctypes, time, sys
f = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(f, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0x44E3E000)
reg = lambda o: ctypes.c_uint32.from_buffer(m, o)
reg(0x6C).value = 0x83E70B13   # KICK0 unlock
reg(0x70).value = 0x95A4F1E0   # KICK1 unlock
reg(0x54).value = 0x40         # OSC: internal 32k source, 32k clock enabled
s0 = reg(0x00).value
time.sleep(1.2)
ok = (reg(0x44).value & 0x02) and reg(0x00).value != s0
print("rtc_int32k: STATUS=%02x OSC=%02x %s" % (reg(0x44).value, reg(0x54).value,
      "running" if ok else "NOT running"))
sys.exit(0 if ok else 1)
