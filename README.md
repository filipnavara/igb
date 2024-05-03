# Intel® GbE Family Community Driver for Windows 11

**Disclaimer: I am not affiliated with Intel Corporation.**

This is basic Windows 11 driver for Intel I210 network controller that can be compiled for ARM64 architecture. It is not supposed to replace the Intel drivers and merely serves to fill in the void until official ARM64 drivers are available directly from Intel.

## Credits

The driver contains code from the Intel FreeBSD igb driver version 2.5.30.

Thanks to *CoolStar* for writing open source [RealTek driver](https://github.com/coolstar/if_re-win), and to *alotipac* for the [Raspberry Pi GENET Ethernet driver](https://github.com/raspberrypi/windows-drivers/). Both of them were invaluable resources that helped me bootstrap the prototype.

## Technical details

The driver supports one 1 TX queue and up to 4 RX queues. This was chosen to balance the supported send offloading methods, receive side scaling, and the number of interrupt vectors supported by the card. For transmission the NIC supports offloading of sending large IPv4, IPv6, TCP, and UDP packets. Since the NIC doesn't support receive side coalescing, we rely mostly on receive side scaling to distribute the load of RX queues.

### Interrupt mapping

In MSI/Legacy mode the RX queues are mapped into the first 4 bits of the Extended Interrupt Cause register. The TX queue uses the legacy bits.

In MSI-X mode we map the RX queues to the first 4 interrupts. The 5th interrupt is used for link status changes and the TX queue.

## TODO

- [ ] Energy Efficient Ethernet
- [ ] Wake on LAN
- [ ] Manual Duplex/Speed setting
- [ ] VLAN filtering
- [ ] Enable support for more NIC models
