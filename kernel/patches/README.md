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
| 0007 sdhci-omap | `sdhci-omap ...: supply pbias/vqmmc not found, using dummy regulator` |
| 0008 usb phy-generic | `am335x-phy-driver ...: dummy supplies not allowed for exclusive requests (id=vbus)` |
| 0009 tps65217 | `tps65217-pmic / tps65217-bl: Failed to locate of_node [id: -1]` |
| 0010 mfd-core | `tps6521x_pwrbutton tps65217-pwrbutton: DMA mask not set` |
| 0011 mmc core | `card claims to support voltages below defined range` (now debug) |
| 0012 dm | `CONFIG_IMA_DISABLE_HTABLE is disabled...` (now info) |
| 0013 wlcore | `Detected unconfigured mac address in nvs` (now info), `no fw rx ba on tid 6` (now debug) |

0001-0012 are built into the zImage. 0013 changes one module, wlcore.ko
(drivers/net/wireless/ti/wlcore), which is replaced on its own:

    git am kernel/patches/00*.patch
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- LOCALVERSION=-bone-rt-r73 zImage
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- LOCALVERSION=-bone-rt-r73 \
         KBUILD_MODPOST_WARN=1 drivers/net/wireless/ti/wlcore/wlcore.ko
