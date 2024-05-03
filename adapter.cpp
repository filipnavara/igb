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
		IGB_MAX_TX_QUEUES);

	txCapabilities.FragmentRingNumberOfElementsHint = IGB_TX_BUF_NUM;
	txCapabilities.MaximumNumberOfFragments = IGB_MAX_PHYS_BUF_COUNT;

	NET_ADAPTER_DMA_CAPABILITIES rxDmaCapabilities;
	NET_ADAPTER_DMA_CAPABILITIES_INIT(&rxDmaCapabilities, adapter->DmaEnabler);

	SIZE_T maxRxQueues = IGB_MAX_RX_QUEUES;

	if (adapter->MsiInterrupts > 1)
		maxRxQueues = min(maxRxQueues, adapter->MsiInterrupts - 1);

	NET_ADAPTER_RX_CAPABILITIES rxCapabilities;
	NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED_DMA(
		&rxCapabilities,
		&rxDmaCapabilities,
		IGB_BUF_SIZE,
		maxRxQueues);

	rxCapabilities.FragmentBufferAlignment = IGB_RX_BUFFER_ALIGN;
	rxCapabilities.FragmentRingNumberOfElementsHint = IGB_RX_BUF_NUM;

	NetAdapterSetDataPathCapabilities(adapter->NetAdapter, &txCapabilities, &rxCapabilities);
}

static
void
EvtAdapterOffloadSetGso(
	_In_ NETADAPTER netAdapter,
	_In_ NETOFFLOAD offload)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	DBGPRINT("EvtAdapterOffloadSetGso IPv4: %d IPV6: %d\n",
		NetOffloadIsLsoIPv4Enabled(offload),
		NetOffloadIsLsoIPv6Enabled(offload));

	adapter->LSOv4 = NetOffloadIsLsoIPv4Enabled(offload);
	adapter->LSOv6 = NetOffloadIsLsoIPv6Enabled(offload);
}

static
void
EvtAdapterOffloadSetTxChecksum(
	_In_ NETADAPTER netAdapter,
	_In_ NETOFFLOAD offload)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	DBGPRINT("EvtAdapterOffloadSetTxChecksum IP: %d TCP: %d UDP: %d\n",
		NetOffloadIsTxChecksumIPv4Enabled(offload),
		NetOffloadIsTxChecksumTcpEnabled(offload),
		NetOffloadIsTxChecksumUdpEnabled(offload));

	adapter->TxIpHwChkSum = NetOffloadIsTxChecksumIPv4Enabled(offload);
	adapter->TxTcpHwChkSum = NetOffloadIsTxChecksumTcpEnabled(offload);
	adapter->TxUdpHwChkSum = NetOffloadIsTxChecksumUdpEnabled(offload);
}

static
void
EvtAdapterOffloadSetRxChecksum(
	_In_ NETADAPTER netAdapter,
	_In_ NETOFFLOAD offload)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	DBGPRINT("EvtAdapterOffloadSetRxChecksum IP: %d TCP: %d UDP: %d\n",
		NetOffloadIsRxChecksumIPv4Enabled(offload),
		NetOffloadIsRxChecksumTcpEnabled(offload),
		NetOffloadIsRxChecksumUdpEnabled(offload));

	u32 rxcsum = E1000_READ_REG(&adapter->Hw, E1000_RXCSUM);
	rxcsum &= ~(E1000_RXCSUM_IPOFL | E1000_RXCSUM_TUOFL);
	rxcsum |= NetOffloadIsRxChecksumIPv4Enabled(offload) ? E1000_RXCSUM_IPOFL : 0;
	rxcsum |= NetOffloadIsRxChecksumTcpEnabled(offload) ? E1000_RXCSUM_TUOFL : 0;
	rxcsum |= NetOffloadIsRxChecksumUdpEnabled(offload) ? E1000_RXCSUM_TUOFL : 0;	
	E1000_WRITE_REG(&adapter->Hw, E1000_RXCSUM, rxcsum);

	adapter->RxIpHwChkSum = NetOffloadIsRxChecksumIPv4Enabled(offload);
	adapter->RxTcpHwChkSum = NetOffloadIsRxChecksumTcpEnabled(offload);
	adapter->RxUdpHwChkSum = NetOffloadIsRxChecksumUdpEnabled(offload);
}


static
NTSTATUS
EvtAdapterReceiveScalingEnable(
	_In_ NETADAPTER netAdapter,
	_In_ NET_ADAPTER_RECEIVE_SCALING_HASH_TYPE hashType,
	_In_ NET_ADAPTER_RECEIVE_SCALING_PROTOCOL_TYPE protocolType)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	DBGPRINT("EvtAdapterReceiveScalingEnable\n");

	u32 mrqc = E1000_MRQC_ENABLE_RSS_4Q;

	if (protocolType & NetAdapterReceiveScalingProtocolTypeIPv4)
	{
		mrqc |= E1000_MRQC_RSS_FIELD_IPV4;
		if (protocolType & NetAdapterReceiveScalingProtocolTypeTcp)
			mrqc |= E1000_MRQC_RSS_FIELD_IPV4_TCP;
	}

	if (protocolType & NetAdapterReceiveScalingProtocolTypeIPv6)
	{
		mrqc |= E1000_MRQC_RSS_FIELD_IPV6;
		if (protocolType & NetAdapterReceiveScalingProtocolTypeTcp)
			mrqc |= E1000_MRQC_RSS_FIELD_IPV6_TCP;
	}

	// TODO: E1000_MRQC_RSS_FIELD_IPV4_UDP,	E1000_MRQC_RSS_FIELD_IPV6_UDP,
	// E1000_MRQC_RSS_FIELD_IPV6_UDP_EX, E1000_MRQC_RSS_FIELD_IPV6_TCP_EX

	E1000_WRITE_REG(&adapter->Hw, E1000_MRQC, mrqc);

	return STATUS_SUCCESS;
}

static
VOID
EvtAdapterReceiveScalingDisable(
	_In_ NETADAPTER netAdapter)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	DBGPRINT("EvtAdapterReceiveScalingDisable\n");

	E1000_WRITE_REG(&adapter->Hw, E1000_MRQC, 0);
}

static
NTSTATUS
EvtAdapterReceiveScalingSetHashSecretKey(
	_In_ NETADAPTER netAdapter,
	_In_ NET_ADAPTER_RECEIVE_SCALING_HASH_SECRET_KEY const* hashSecretKey)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	DBGPRINT("EvtAdapterReceiveScalingSetHashSecretKey\n");

	for (int i = 0; i < 10 && i < hashSecretKey->Length; i++)
	{
		E1000_WRITE_REG_ARRAY(&adapter->Hw, E1000_RSSRK(0), i, hashSecretKey->Key[i]);
	}

	return STATUS_SUCCESS;
}

static
NTSTATUS
EvtAdapterReceiveScalingSetIndirectionEntries(
	_In_ NETADAPTER netAdapter,
	_In_ NET_ADAPTER_RECEIVE_SCALING_INDIRECTION_ENTRIES* indirectionEntries)
{
	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	DBGPRINT("EvtAdapterReceiveScalingSetIndirectionEntries\n");

	for (size_t i = 0; i < indirectionEntries->Length; i++)
	{
		const ULONG queueId = IgbGetRxQueueContext(indirectionEntries->Entries[i].PacketQueue)->QueueId;
		const UINT32 index = indirectionEntries->Entries[i].Index;
		if (index < 128)
		{
			u32 reta = E1000_READ_REG(&adapter->Hw, E1000_RETA(index >> 2));
			u32 shift = (index & 2) << 3;
			reta ^= reta & (0xff << shift);
			reta |= queueId << shift;
			E1000_WRITE_REG(&adapter->Hw, E1000_RETA(index >> 2), reta);
		}
	}

	return STATUS_SUCCESS;

}

static
void
IgbAdapterSetOffloadCapabilities(
	_In_ IGB_ADAPTER const* adapter)
{
	NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES gsoOffloadCapabilities;
	NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES_INIT(
		&gsoOffloadCapabilities,
		NetAdapterOffloadLayer3FlagIPv4NoOptions |
		NetAdapterOffloadLayer3FlagIPv4WithOptions |
		NetAdapterOffloadLayer3FlagIPv6NoExtensions |
		NetAdapterOffloadLayer3FlagIPv6WithExtensions,
		NetAdapterOffloadLayer4FlagTcpNoOptions |
		NetAdapterOffloadLayer4FlagTcpWithOptions |
		NetAdapterOffloadLayer4FlagUdp,
		0xffff,
		1,
		EvtAdapterOffloadSetGso);
	gsoOffloadCapabilities.Layer4HeaderOffsetLimit = 240;
	NetAdapterOffloadSetGsoCapabilities(adapter->NetAdapter, &gsoOffloadCapabilities);

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

	NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES rxChecksumOffloadCapabilities;
	NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES_INIT(
		&rxChecksumOffloadCapabilities,
		EvtAdapterOffloadSetRxChecksum);
	NetAdapterOffloadSetRxChecksumCapabilities(adapter->NetAdapter, &rxChecksumOffloadCapabilities);

	NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES ieee8021qTagOffloadCapabilities;
	NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES_INIT(
		&ieee8021qTagOffloadCapabilities,
		NetAdapterOffloadIeee8021PriorityTaggingFlag |
		NetAdapterOffloadIeee8021VlanTaggingFlag);
	NetAdapterOffloadSetIeee8021qTagCapabilities(adapter->NetAdapter, &ieee8021qTagOffloadCapabilities);
}

static
void
IgbAdapterSetRssCapabilities(
	_In_ IGB_ADAPTER const* adapter)
{
	NET_ADAPTER_RECEIVE_SCALING_CAPABILITIES receiveScalingCapabilities;
	NET_ADAPTER_RECEIVE_SCALING_CAPABILITIES_INIT(
		&receiveScalingCapabilities,
		IGB_MAX_RX_QUEUES,
		NetAdapterReceiveScalingUnhashedTargetTypeHashIndex,
		NetAdapterReceiveScalingHashTypeToeplitz,
		NetAdapterReceiveScalingProtocolTypeIPv4 |
		NetAdapterReceiveScalingProtocolTypeIPv6 |
		NetAdapterReceiveScalingProtocolTypeTcp,
		EvtAdapterReceiveScalingEnable,
		EvtAdapterReceiveScalingDisable,
		EvtAdapterReceiveScalingSetHashSecretKey,
		EvtAdapterReceiveScalingSetIndirectionEntries);
	receiveScalingCapabilities.SynchronizeSetIndirectionEntries = true;
	NetAdapterSetReceiveScalingCapabilities(adapter->NetAdapter, &receiveScalingCapabilities);
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
	IgbAdapterSetRssCapabilities(adapter);

	IgbUpdateReceiveFilters(adapter);

	e1000_init_hw(&adapter->Hw);

	GOTO_IF_NOT_NT_SUCCESS(
		Exit, status,
		NetAdapterStart(adapter->NetAdapter));

Exit:
	DBGPRINT("IntelAdapterStart - %x\n", status);

	return status;
}
