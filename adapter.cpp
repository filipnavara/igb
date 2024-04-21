#include "precomp.h"

#include "trace.h"
#include "device.h"
#include "adapter.h"
#include "txqueue.h"
#include "rxqueue.h"
#include "link.h"

NTSTATUS
IgbInitializeAdapterContext(
	_In_ IGB_ADAPTER* adapter,
	_In_ WDFDEVICE device,
	_In_ NETADAPTER netAdapter)
{
	DBGPRINT("IntelInitializeAdapterContext\n");

	NTSTATUS status = STATUS_SUCCESS;

	adapter->NetAdapter = netAdapter;
	adapter->WdfDevice = device;
	IgbGetDeviceContext(device)->Adapter = adapter;

	WDF_OBJECT_ATTRIBUTES  attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = device;

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfSpinLockCreate(&attributes, &adapter->Lock));

	WDF_OBJECT_ATTRIBUTES timerAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
	timerAttributes.ParentObject = device;

Exit:
	DBGPRINT("IntelInitializeAdapterContext - %x\n", status);

	return status;
}

static void
IgbUpdateReceiveFilters(
	_In_ IGB_ADAPTER* adapter)
{
	NET_PACKET_FILTER_FLAGS packetFilterFlags = adapter->PacketFilterFlags;
	u32 rctl;

	rctl = E1000_READ_REG(&adapter->Hw, E1000_RCTL);
	rctl &= ~(E1000_RCTL_UPE | E1000_RCTL_MPE);
	if ((packetFilterFlags & NetPacketFilterFlagPromiscuous) != 0)
	{
		// Receive everything
		rctl |= E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM;
	}
	else
	{
		if ((packetFilterFlags & NetPacketFilterFlagAllMulticast) != 0)
			rctl |= E1000_RCTL_MPE;

		if ((packetFilterFlags & NetPacketFilterFlagBroadcast) != 0)
			rctl |= E1000_RCTL_BAM;

		e1000_rar_set(&adapter->Hw, adapter->CurrentAddress.Address, 0);

		if ((packetFilterFlags & NetPacketFilterFlagMulticast) != 0)
			e1000_update_mc_addr_list(&adapter->Hw, adapter->MCAddressList, adapter->MCAddressLength / ETH_LENGTH_OF_ADDRESS);
		else
			e1000_update_mc_addr_list(&adapter->Hw, NULL, 0);
	}
	E1000_WRITE_REG(&adapter->Hw, E1000_RCTL, rctl);
}

static void
EvtSetReceiveFilter(
	_In_ NETADAPTER netAdapter,
	_In_ NETRECEIVEFILTER handle)
{
	DBGPRINT("EvtSetReceiveFilter\n");

	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);
	NET_PACKET_FILTER_FLAGS packetFilterFlags = NetReceiveFilterGetPacketFilter(handle);

	adapter->PacketFilterFlags = NetReceiveFilterGetPacketFilter(handle);
	RtlZeroMemory(adapter->MCAddressList, sizeof(adapter->MCAddressList));

	UINT address_count = (UINT)NetReceiveFilterGetMulticastAddressCount(handle);
	NET_ADAPTER_LINK_LAYER_ADDRESS const* address_list = NetReceiveFilterGetMulticastAddressList(handle);
	adapter->MCAddressLength = 0;
	for (int i = 0; i < address_count; i++)
	{
		if (address_list[i].Length == ETH_LENGTH_OF_ADDRESS)
		{
			RtlCopyMemory(
				adapter->MCAddressList + adapter->MCAddressLength,
				address_list[i].Address,
				ETH_LENGTH_OF_ADDRESS);
			adapter->MCAddressLength += ETH_LENGTH_OF_ADDRESS;
		}
	}

	IgbUpdateReceiveFilters(adapter);
}

static
void
IgbAdapterSetReceiveFilterCapabilities(
	_In_ IGB_ADAPTER const* adapter)
{
	NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES rxFilterCaps;
	NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&rxFilterCaps, EvtSetReceiveFilter);
	rxFilterCaps.SupportedPacketFilters =
		NetPacketFilterFlagDirected | NetPacketFilterFlagMulticast |
		NetPacketFilterFlagAllMulticast | NetPacketFilterFlagBroadcast |
		NetPacketFilterFlagPromiscuous;
	rxFilterCaps.MaximumMulticastAddresses = E1000_RAR_ENTRIES;
	NetAdapterSetReceiveFilterCapabilities(adapter->NetAdapter, &rxFilterCaps);
}

static
void
IgbAdapterSetLinkLayerCapabilities(
	_In_ IGB_ADAPTER* adapter)
{
	NET_ADAPTER_LINK_LAYER_CAPABILITIES linkLayerCapabilities;
	NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&linkLayerCapabilities, 1'000'000'000, 1'000'000'000);
	NetAdapterSetLinkLayerCapabilities(adapter->NetAdapter, &linkLayerCapabilities);
	NetAdapterSetLinkLayerMtuSize(adapter->NetAdapter, 1500);
	NetAdapterSetPermanentLinkLayerAddress(adapter->NetAdapter, &adapter->PermanentAddress);
	NetAdapterSetCurrentLinkLayerAddress(adapter->NetAdapter, &adapter->CurrentAddress);
}

static
void
IgbAdapterSetDatapathCapabilities(
	_In_ IGB_ADAPTER const* adapter)
{
	NET_ADAPTER_DMA_CAPABILITIES txDmaCapabilities;
	NET_ADAPTER_DMA_CAPABILITIES_INIT(&txDmaCapabilities, adapter->DmaEnabler);

	NET_ADAPTER_TX_CAPABILITIES txCapabilities;
	NET_ADAPTER_TX_CAPABILITIES_INIT_FOR_DMA(
		&txCapabilities,
		&txDmaCapabilities,
		1);

	txCapabilities.FragmentRingNumberOfElementsHint = /*RE_TX_BUF_NUM*/1024;
	txCapabilities.MaximumNumberOfFragments = /*RE_NTXSEGS*/1;

	NET_ADAPTER_DMA_CAPABILITIES rxDmaCapabilities;
	NET_ADAPTER_DMA_CAPABILITIES_INIT(&rxDmaCapabilities, adapter->DmaEnabler);

	NET_ADAPTER_RX_CAPABILITIES rxCapabilities;
	NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(
		&rxCapabilities,
		&rxDmaCapabilities,
		IGB_BUF_SIZE,
		1);

	rxCapabilities.FragmentBufferAlignment = /*RE_RX_BUFFER_ALIGN*/128;
	rxCapabilities.FragmentRingNumberOfElementsHint = /*RE_RX_BUF_NUM*/1024;

	NetAdapterSetDataPathCapabilities(adapter->NetAdapter, &txCapabilities, &rxCapabilities);
}

static
void
EvtAdapterOffloadSetTxChecksum(
	_In_ NETADAPTER netAdapter,
	_In_ NETOFFLOAD offload)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	/*adapter->TxIpHwChkSum = NetOffloadIsTxChecksumIPv4Enabled(offload);
	adapter->TxTcpHwChkSum = NetOffloadIsTxChecksumTcpEnabled(offload);
	adapter->TxUdpHwChkSum = NetOffloadIsTxChecksumUdpEnabled(offload);*/
	DBGPRINT("EvtAdapterOffloadSetTxChecksum IP: %d TCP: %d UDP: %d\n",
		NetOffloadIsTxChecksumIPv4Enabled(offload),
		NetOffloadIsTxChecksumTcpEnabled(offload),
		NetOffloadIsTxChecksumUdpEnabled(offload));
}

static
void
IgbAdapterSetOffloadCapabilities(
	_In_ IGB_ADAPTER const* adapter)
{
	NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES txChecksumOffloadCapabilities;

	NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES_INIT(
		&txChecksumOffloadCapabilities,
		NetAdapterOffloadLayer3FlagIPv4NoOptions |
		NetAdapterOffloadLayer3FlagIPv4WithOptions |
		NetAdapterOffloadLayer3FlagIPv6NoExtensions |
		NetAdapterOffloadLayer3FlagIPv6WithExtensions,
		EvtAdapterOffloadSetTxChecksum);
	txChecksumOffloadCapabilities.Layer4Flags =
		NetAdapterOffloadLayer4FlagTcpNoOptions |
		NetAdapterOffloadLayer4FlagTcpWithOptions |
		NetAdapterOffloadLayer4FlagUdp;
	txChecksumOffloadCapabilities.Layer4HeaderOffsetLimit = 511;
	NetAdapterOffloadSetTxChecksumCapabilities(adapter->NetAdapter, &txChecksumOffloadCapabilities);

	NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES ieee8021qTagOffloadCapabilities;
	NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES_INIT(
		&ieee8021qTagOffloadCapabilities,
		NetAdapterOffloadIeee8021PriorityTaggingFlag |
		NetAdapterOffloadIeee8021VlanTaggingFlag);
	NetAdapterOffloadSetIeee8021qTagCapabilities(adapter->NetAdapter, &ieee8021qTagOffloadCapabilities);
}

_Use_decl_annotations_
NTSTATUS
IgbAdapterStart(
	IGB_ADAPTER* adapter)
{
	DBGPRINT("IntelAdapterStart\n");

	NTSTATUS status = STATUS_SUCCESS;

	IgbAdapterSetLinkLayerCapabilities(adapter);
	IgbAdapterSetReceiveFilterCapabilities(adapter);
	IgbAdapterSetDatapathCapabilities(adapter);
	IgbAdapterSetOffloadCapabilities(adapter);

	IgbUpdateReceiveFilters(adapter);

	e1000_init_hw(&adapter->Hw);

	GOTO_IF_NOT_NT_SUCCESS(
		Exit, status,
		NetAdapterStart(adapter->NetAdapter));

Exit:
	DBGPRINT("IntelAdapterStart - %x\n", status);

	return status;
}
