# Intel® GbE Family Community Driver for Windows 11

**Disclaimer: I am not affiliated with Intel Corporation.**

This is basic Windows 11 driver for Intel I210 network controller that can be compiled for ARM64 architecture. It is not supposed to replace the Intel drivers and merely serves to fill in the void until official ARM64 drivers are available directly from Intel.

## Credits

The driver contains code from the Intel FreeBSD igb driver version 2.5.30.

Thanks to *CoolStar* for writing open source [RealTek driver](https://github.com/coolstar/if_re-win), and to *alotipac* for the [Raspberry Pi GENET Ethernet driver](https://github.com/raspberrypi/windows-drivers/). Both of them were invaluable resources that helped me bootstrap the prototype.

## TODO

- [ ] Efficient MSI/MSI-X interrupt handling
- [ ] Offloading
  - [x] Checksums
  - [ ] Large send offload
- [ ] Energy Efficient Ethernet
- [ ] RSS
- [ ] Enable support for more NIC models
