# Kernel patches (6.12.111-bone-rt-r73)

Applied on top of the `6.12.111-bone-rt-r73` tag of
https://github.com/RobertCNelson/linux-stable-rcn-ee. Each one removes a
boot-log error or warning that was harmless but printed on every boot.

| Patch | Boot message it removes |
|---|---|
| 0001 ti-sysc clkdev | `...pmu@0:fck: device ID is greater than 24` (also makes that clock lookup work) |
| 0002 OMAP2+ voltage | `omap_voltage_late_init: Voltage driver support not added` |
| 0003 pm33xx defer | `PM: Cannot get wkup_m3_ipc handle` |
| 0004 pm33xx rtc-only | `PM: bootloader does not support rtc-only!` (now info) |
| 0005 dmaengine debugfs | `debugfs: Directory '49000000.dma' ... already present!` |
| 0006 fw_devlink | `ti-sysc 44e31000/48040000: Failed to create device link (0x180) with ocp` |

Rebuild only the zImage (all changed code is built in; modules unchanged):

    git am kernel/patches/00*.patch
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- LOCALVERSION=-bone-rt-r73 zImage
