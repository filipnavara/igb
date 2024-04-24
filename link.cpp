#include "precomp.h"
#include "trace.h"
#include "adapter.h"
#include "device.h"
#include "link.h"
#include "interrupt.h"

#define MBit 1000000ULL

void IgbResetLink(_In_ IGB_ADAPTER* adapter)
{
	DBGPRINT("IntelResetLink\n");

	// Clear bad data from Rx FIFOs
	e1000_rx_fifo_flush_base(&adapter->Hw);

	e1000_clear_hw_cntrs_base_generic(&adapter->Hw);

	adapter->Hw.mac.get_link_status = 1;

	WdfInterruptAcquireLock(adapter->Interrupt->Handle);

	// Clears any pending interrupts
	E1000_READ_REG(&adapter->Hw, E1000_ICR);
	// Enable link interrupts
	E1000_WRITE_REG(&adapter->Hw, E1000_IMS, E1000_IMS_LSC);
	E1000_WRITE_FLUSH(&adapter->Hw);

	E1000_WRITE_REG(&adapter->Hw, E1000_VET, /*ETHERTYPE_VLAN*/0x8100);
	e1000_get_phy_info(&adapter->Hw);

	WdfInterruptReleaseLock(adapter->Interrupt->Handle);

	IgbCheckLinkStatus(adapter);
}

void IgbCheckLinkStatus(_In_ IGB_ADAPTER* adapter)
{
	DBGPRINT("IntelCheckLinkStatus\n");

	struct e1000_hw* hw = &adapter->Hw;    
	bool link_up;

	e1000_check_for_link(hw);
	link_up = !hw->mac.get_link_status;

	if (link_up)
	{
		u16 link_speed;
		u16 link_duplex;

		e1000_get_speed_and_duplex(hw, &link_speed, &link_duplex);
		DBGPRINT("UP speed: %d duplex: %d\n", link_speed, link_duplex);

		NET_ADAPTER_PAUSE_FUNCTION_TYPE pauseFunction = NetAdapterPauseFunctionTypeUnknown;
		switch (hw->fc.current_mode)
		{
			case e1000_fc_none:
				pauseFunction = NetAdapterPauseFunctionTypeUnsupported;
				break;
			case e1000_fc_rx_pause:
				pauseFunction = NetAdapterPauseFunctionTypeReceiveOnly;
				break;
			case e1000_fc_tx_pause:
				pauseFunction = NetAdapterPauseFunctionTypeSendOnly;
				break;
			case e1000_fc_full:
				pauseFunction = NetAdapterPauseFunctionTypeSendAndReceive;
				break;
		}

		NET_ADAPTER_LINK_STATE linkState;
		NET_ADAPTER_LINK_STATE_INIT(
			&linkState,
			link_speed * MBit,
			MediaConnectStateConnected,
			link_duplex == FULL_DUPLEX ? MediaDuplexStateFull : MediaDuplexStateHalf,
			pauseFunction,
			NetAdapterAutoNegotiationFlagXmitLinkSpeedAutoNegotiated |
			NetAdapterAutoNegotiationFlagRcvLinkSpeedautoNegotiated |
			NetAdapterAutoNegotiationFlagDuplexAutoNegotiated);
		NetAdapterSetLinkState(adapter->NetAdapter, &linkState);
	}
	else
	{
		DBGPRINT("DOWN\n");

		NET_ADAPTER_LINK_STATE linkState;
		NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&linkState);
		NetAdapterSetLinkState(adapter->NetAdapter, &linkState);
	}
}
