#include "precomp.h"
#include "e1000_api.h"

void
e1000_write_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	// TODO
}

void
e1000_read_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	// TODO
	*value = 0;
}

/*
 * Read the PCI Express capabilities
 */
s32
e1000_read_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	// TODO
	return E1000_ERR_CONFIG;
}

/*
 * Write the PCI Express capabilities
 */
s32
e1000_write_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	// TODO
	return E1000_ERR_CONFIG;
}
