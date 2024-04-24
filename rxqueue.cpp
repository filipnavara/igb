#include "precomp.h"

#include "device.h"
#include "trace.h"
#include "adapter.h"
#include "rxqueue.h"
#include "interrupt.h"
#include "link.h"

#undef DBGPRINT
#define DBGPRINT(...)

_Use_decl_annotations_
NTSTATUS
EvtAdapterCreateRxQueue(
	_In_ NETADAPTER netAdapter,
	_Inout_ NETRXQUEUE_INIT* rxQueueInit)
{
	NET_EXTENSION_QUERY extension;
	NTSTATUS status = STATUS_SUCCESS;

	DBGPRINT("EvtAdapterCreateRxQueue\n");

	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	WDF_OBJECT_ATTRIBUTES rxAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&rxAttributes, IGB_RXQUEUE);

	rxAttributes.EvtDestroyCallback = EvtRxQueueDestroy;

	NET_PACKET_QUEUE_CONFIG rxConfig;
	NET_PACKET_QUEUE_CONFIG_INIT(
		&rxConfig,
		EvtRxQueueAdvance,
		EvtRxQueueSetNotificationEnabled,
		EvtRxQueueCancel);
	rxConfig.EvtStart = EvtRxQueueStart;
	rxConfig.EvtStop = EvtRxQueueStop;

	const ULONG queueId = NetRxQueueInitGetQueueId(rxQueueInit);
	NETPACKETQUEUE rxQueue;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		NetRxQueueCreate(rxQueueInit, &rxAttributes, &rxConfig, &rxQueue));

	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);
	rx->QueueId = queueId;

	NET_EXTENSION_QUERY_INIT(
		&extension,
		NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
		NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
		NetExtensionTypeFragment);
	NetRxQueueGetExtension(rxQueue, &extension, &rx->LogicalAddressExtension);

	NET_EXTENSION_QUERY_INIT(
		&extension,
		NET_PACKET_EXTENSION_CHECKSUM_NAME,
		NET_PACKET_EXTENSION_CHECKSUM_VERSION_1,
		NetExtensionTypePacket);
	NetRxQueueGetExtension(rxQueue, &extension, &rx->ChecksumExtension);

	NET_EXTENSION_QUERY_INIT(
		&extension,
		NET_PACKET_EXTENSION_IEEE8021Q_NAME,
		NET_PACKET_EXTENSION_IEEE8021Q_VERSION_1,
		NetExtensionTypePacket);
	NetRxQueueGetExtension(rxQueue, &extension, &rx->Ieee8021qExtension);

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbRxQueueInitialize(rxQueue, adapter));

Exit:
	DBGPRINT("EvtAdapterCreateRxQueue = %x\n", status);

	return status;
}

void
RxIndicateReceives(
	_In_ IGB_RXQUEUE* rx)
{
	DBGPRINT("RxIndicateReceives\n");

	NET_RING* pr = NetRingCollectionGetPacketRing(rx->Rings);
	NET_RING* fr = NetRingCollectionGetFragmentRing(rx->Rings);

	while (fr->BeginIndex != fr->NextIndex)
	{
		UINT32 const fragmentIndex = fr->BeginIndex;

		union e1000_adv_rx_desc const* rxd = &rx->RxdBase[fragmentIndex];

		if ((rxd->wb.upper.status_error & E1000_RXD_STAT_DD) == 0)
			break;
		if (pr->BeginIndex == pr->EndIndex)
			break;

		// If there is a packet available we are guaranteed to have a fragment as well
		NT_FRE_ASSERT(fr->BeginIndex != fr->EndIndex);

		NET_FRAGMENT* fragment = NetRingGetFragmentAtIndex(fr, fragmentIndex);
		fragment->ValidLength = rxd->wb.upper.length;
		fragment->Capacity = fragment->ValidLength;
		fragment->Offset = 0;

		// Link fragment and packet
		UINT32 const packetIndex = pr->BeginIndex;
		NET_PACKET* packet = NetRingGetPacketAtIndex(pr, packetIndex);
		packet->FragmentIndex = fragmentIndex;
		packet->FragmentCount = 1;

		if (rx->Ieee8021qExtension.Enabled)
		{
			NET_PACKET_IEEE8021Q* ieee8021q = NetExtensionGetPacketIeee8021Q(&rx->Ieee8021qExtension, packetIndex);
			if ((rxd->wb.upper.status_error & E1000_RXD_STAT_VP) == 0)
			{
				u16 vlanId = rxd->wb.upper.vlan;
				ieee8021q->PriorityCodePoint = vlanId >> 13;
				ieee8021q->VlanIdentifier = vlanId & 0xfff;
			}
		}

		if (rx->ChecksumExtension.Enabled)
		{
			NET_PACKET_CHECKSUM* checksum = NetExtensionGetPacketChecksum(&rx->ChecksumExtension, packetIndex);
			u32 ptype = rxd->wb.lower.lo_dword.data & (E1000_RXDADV_PKTTYPE_ILMASK | E1000_RXDADV_PKTTYPE_TLMASK);

			packet->Layout.Layer2Type = NetPacketLayer2TypeEthernet;
			
			if (rx->Adapter->RxIpHwChkSum)
			{
				if ((ptype & E1000_RXDADV_PKTTYPE_IPV4) == E1000_RXDADV_PKTTYPE_IPV4)
					packet->Layout.Layer3Type = NetPacketLayer3TypeIPv4NoOptions;
				else if ((ptype & E1000_RXDADV_PKTTYPE_IPV4_EX) == E1000_RXDADV_PKTTYPE_IPV4_EX)
					packet->Layout.Layer3Type = NetPacketLayer3TypeIPv4WithOptions;
				else if ((ptype & E1000_RXDADV_PKTTYPE_IPV6) == E1000_RXDADV_PKTTYPE_IPV6)
					packet->Layout.Layer3Type = NetPacketLayer3TypeIPv6NoExtensions;
				else if ((ptype & E1000_RXDADV_PKTTYPE_IPV6_EX) == E1000_RXDADV_PKTTYPE_IPV6_EX)
					packet->Layout.Layer3Type = NetPacketLayer3TypeIPv6WithExtensions;
				else
					packet->Layout.Layer3Type = NetPacketLayer3TypeUnspecified;

				if ((rxd->wb.upper.status_error & E1000_RXD_STAT_IPCS) != 0)
				{
					if ((rxd->wb.upper.status_error & E1000_RXDEXT_STATERR_IPE) == 0)
						checksum->Layer3 = NetPacketRxChecksumEvaluationValid;
					else
						checksum->Layer3 = NetPacketRxChecksumEvaluationInvalid;
				}
			}

			if (rx->Adapter->RxTcpHwChkSum && (ptype & E1000_RXDADV_PKTTYPE_TCP) == E1000_RXDADV_PKTTYPE_TCP)
			{
				packet->Layout.Layer4Type = NetPacketLayer4TypeTcp;

				if ((rxd->wb.upper.status_error & E1000_RXD_STAT_TCPCS) != 0)
				{
					if ((rxd->wb.upper.status_error & E1000_RXDEXT_STATERR_TCPE) == 0)
						checksum->Layer4 = NetPacketRxChecksumEvaluationValid;
					else
						checksum->Layer4 = NetPacketRxChecksumEvaluationInvalid;
				}
			}

			if (rx->Adapter->RxUdpHwChkSum && (ptype & E1000_RXDADV_PKTTYPE_UDP) == E1000_RXDADV_PKTTYPE_UDP)
			{
				packet->Layout.Layer4Type = NetPacketLayer4TypeUdp;

				if ((rxd->wb.upper.status_error & E1000_RXD_STAT_UDPCS) != 0)
				{
					if ((rxd->wb.upper.status_error & E1000_RXDEXT_STATERR_TCPE) == 0)
						checksum->Layer4 = NetPacketRxChecksumEvaluationValid;
					else
						checksum->Layer4 = NetPacketRxChecksumEvaluationInvalid;
				}
			}
		}

		packet->Layout.Layer2Type = NetPacketLayer2TypeEthernet;

		pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
		fr->BeginIndex = NetRingIncrementIndex(fr, fr->BeginIndex);
	}
}

static
void
IgbPostRxDescriptor(
	_In_ union e1000_adv_rx_desc* desc,
	_In_ NET_FRAGMENT_LOGICAL_ADDRESS const* logicalAddress)
{
	RtlZeroMemory(desc, sizeof(*desc));
	desc->read.pkt_addr = logicalAddress->LogicalAddress;
	desc->read.hdr_addr = 0;
	MemoryBarrier();
}

void
RxPostBuffers(
	_In_ IGB_RXQUEUE* rx)
{
	DBGPRINT("RxPostBuffers\n");

	NET_RING* fr = NetRingCollectionGetFragmentRing(rx->Rings);

	while (fr->NextIndex != fr->EndIndex)
	{
		UINT32 const index = fr->NextIndex;

		IgbPostRxDescriptor(
			&rx->RxdBase[index],
			NetExtensionGetFragmentLogicalAddress(&rx->LogicalAddressExtension, index));
		E1000_WRITE_REG(&rx->Adapter->Hw, E1000_RDT(rx->QueueId), index);

		fr->NextIndex = NetRingIncrementIndex(fr, fr->NextIndex);
	}
}

NTSTATUS
IgbRxQueueInitialize(
	_In_ NETPACKETQUEUE rxQueue,
	_In_ IGB_ADAPTER* adapter
)
{
	NTSTATUS status = STATUS_SUCCESS;

	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);

	DBGPRINT("IntelRxQueueInitialize\n");

	rx->Adapter = adapter;
	rx->Interrupt = adapter->Interrupt;
	rx->Rings = NetRxQueueGetRingCollection(rxQueue);

	// allocate descriptors
	NET_RING* pr = NetRingCollectionGetPacketRing(rx->Rings);
	NET_RING* fr = NetRingCollectionGetFragmentRing(rx->Rings);
	rx->NumRxDesc = (USHORT)(fr->NumberOfElements > 4096 ? 4096 : fr->NumberOfElements);

	ULONG rxSize;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		RtlULongMult(rx->NumRxDesc, sizeof(union e1000_adv_rx_desc), &rxSize));

	UINT32 const rxdSize = pr->NumberOfElements * sizeof(union e1000_adv_rx_desc);
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfCommonBufferCreate(
			rx->Adapter->DmaEnabler,
			rxdSize,
			WDF_NO_OBJECT_ATTRIBUTES,
			&rx->RxdArray));

	rx->RxdBase = static_cast<union e1000_adv_rx_desc*>(WdfCommonBufferGetAlignedVirtualAddress(rx->RxdArray));
	rx->RxdSize = rxdSize;

Exit:
	DBGPRINT("IntelRxQueueInitialize = %x\n", status);

	return status;
}

void
IgbRxQueueSetInterrupt(
	_In_ IGB_RXQUEUE* rx,
	_In_ BOOLEAN notificationEnabled)
{
	InterlockedExchange(&rx->Interrupt->RxNotifyArmed[rx->QueueId], notificationEnabled);

	WdfInterruptAcquireLock(rx->Interrupt->Handle);
	E1000_WRITE_REG(&rx->Adapter->Hw, notificationEnabled ? E1000_IMS : E1000_IMC, E1000_IMS_RXT0 | E1000_IMS_RXDMT0 | E1000_IMS_RXSEQ);
	E1000_WRITE_FLUSH(&rx->Adapter->Hw);
	WdfInterruptReleaseLock(rx->Interrupt->Handle);

	if (!notificationEnabled)
	{
		// block this thread until we're sure any outstanding DPCs are complete.
		// This is to guarantee we don't return from this function call until
		// any outstanding rx notification is complete.
		KeFlushQueuedDpcs();
	}
}

_Use_decl_annotations_
void
EvtRxQueueStart(
	NETPACKETQUEUE rxQueue)
{
	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);
	IGB_ADAPTER* adapter = rx->Adapter;
	struct e1000_hw* hw = &rx->Adapter->Hw;
	u32 srrctl;

	DBGPRINT("EvtRxQueueStart\n");

	ASSERT(rx->QueueId < IGB_MAX_RX_QUEUES);

	RtlZeroMemory(rx->RxdBase, rx->RxdSize);

	PHYSICAL_ADDRESS pa = WdfCommonBufferGetAlignedLogicalAddress(rx->RxdArray);

	// Disable receives
	u32 rctl = E1000_READ_REG(hw, E1000_RCTL);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);

	// Use advanced descriptor format without header split/replication
	srrctl = E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;

	rctl &= ~E1000_RCTL_LPE;
	rctl |= E1000_RCTL_SZ_2048;

	E1000_WRITE_REG(hw, E1000_RDLEN(rx->QueueId), rx->RxdSize);
	E1000_WRITE_REG(hw, E1000_RDBAH(rx->QueueId), (u32)pa.HighPart);
	E1000_WRITE_REG(hw, E1000_RDBAL(rx->QueueId), (u32)pa.LowPart);
	E1000_WRITE_REG(hw, E1000_SRRCTL(rx->QueueId), srrctl);

	// Setup the Receive Control Register
	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO | E1000_RCTL_RDMTS_HALF |
		(hw->mac.mc_filter_type << E1000_RCTL_MO_SHIFT);
	// Strip CRC bytes.
	rctl |= E1000_RCTL_SECRC;
	// Make sure VLAN Filters are off
	rctl &= ~E1000_RCTL_VFE;
	// Don't store bad packets
	rctl &= ~E1000_RCTL_SBP;

	// Enable this Queue
	u32 rxdctl = E1000_READ_REG(hw, E1000_RXDCTL(rx->QueueId));
	rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;
	rxdctl &= 0xFFF00000;
	rxdctl |= /*IGB_RX_PTHRESH*/8;
	rxdctl |= /*IGB_RX_HTHRESH*/8 << 8;
	rxdctl |= /*IGB_RX_WTHRESH*/1 << 16;
	E1000_WRITE_REG(hw, E1000_RXDCTL(rx->QueueId), rxdctl);

	WdfSpinLockAcquire(adapter->Lock);

	adapter->RxQueues[rx->QueueId] = rxQueue;

	//RtAdapterUpdateRcr(adapter);

	WdfSpinLockRelease(adapter->Lock);

	// Enable Receives
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);

	// Heads & tails?
	E1000_WRITE_REG(hw, E1000_RDH(rx->QueueId), 0);
	E1000_WRITE_REG(hw, E1000_RDT(rx->QueueId), rx->NumRxDesc - 1);
}

_Use_decl_annotations_
VOID
EvtRxQueueStop(
	NETPACKETQUEUE rxQueue)
{
	DBGPRINT("EvtRxQueueStop\n");
	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);

	ASSERT(rx->QueueId < IGB_MAX_RX_QUEUES);

	WdfSpinLockAcquire(rx->Adapter->Lock);

	IgbRxQueueSetInterrupt(rx, false);
	rx->Adapter->RxQueues[rx->QueueId] = WDF_NO_HANDLE;

	WdfSpinLockRelease(rx->Adapter->Lock);
}

_Use_decl_annotations_
VOID
EvtRxQueueDestroy(
	_In_ WDFOBJECT rxQueue)
{
	DBGPRINT("EvtRxQueueDestroy\n");
	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);

	if (rx->RxdArray)
		WdfObjectDelete(rx->RxdArray);
	rx->RxdArray = NULL;
}

_Use_decl_annotations_
VOID
EvtRxQueueSetNotificationEnabled(
	_In_ NETPACKETQUEUE rxQueue,
	_In_ BOOLEAN notificationEnabled)
{
	DBGPRINT("EvtRxQueueSetNotificationEnabled\n");
	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);
	IgbRxQueueSetInterrupt(rx, notificationEnabled);
}

_Use_decl_annotations_
VOID
EvtRxQueueAdvance(
	_In_ NETPACKETQUEUE rxQueue)
{
	DBGPRINT("EvtRxQueueAdvance\n");
	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);
	RxIndicateReceives(rx);
	RxPostBuffers(rx);
	DBGPRINT("EvtRxQueueAdvance done\n");
}

_Use_decl_annotations_
VOID
EvtRxQueueCancel(
	_In_ NETPACKETQUEUE rxQueue)
{
	DBGPRINT("EvtRxQueueCancel\n");

	IGB_RXQUEUE* rx = IgbGetRxQueueContext(rxQueue);
	struct e1000_hw* hw = &rx->Adapter->Hw;

	WdfSpinLockAcquire(rx->Adapter->Lock);

	// Disable the queue
	u32 rxdctl = E1000_READ_REG(hw, E1000_RXDCTL(rx->QueueId));
	rxdctl &= ~E1000_RXDCTL_QUEUE_ENABLE;
	E1000_WRITE_REG(hw, E1000_RXDCTL(rx->QueueId), rxdctl);

	WdfSpinLockRelease(rx->Adapter->Lock);

	// try (but not very hard) to grab anything that may have been
	// indicated during rx disable. advance will continue to be called
	// after cancel until all packets are returned to the framework.
	RxIndicateReceives(rx);

	DBGPRINT("EvtRxQueueCancel - in the middle\n");

	NET_RING* pr = NetRingCollectionGetPacketRing(rx->Rings);

	while (pr->BeginIndex != pr->EndIndex)
	{
		NET_PACKET* packet = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
		packet->Ignore = 1;

		pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
	}

	NET_RING* fr = NetRingCollectionGetFragmentRing(rx->Rings);

	fr->BeginIndex = fr->EndIndex;

	DBGPRINT("EvtRxQueueCancel - done\n");
}