#include "precomp.h"

#include "device.h"
#include "trace.h"
#include "adapter.h"
#include "txqueue.h"
#include "interrupt.h"
#include "link.h"

//#define DBGPRINT_ADVANCE DBGPRINT
#define DBGPRINT_ADVANCE(...)

_Use_decl_annotations_
NTSTATUS
EvtAdapterCreateTxQueue(
	_In_ NETADAPTER netAdapter,
	_Inout_ NETTXQUEUE_INIT* txQueueInit)
{
	NTSTATUS status = STATUS_SUCCESS;

	DBGPRINT("EvtAdapterCreateTxQueue\n");

	IGB_ADAPTER* adapter = IgbGetAdapterContext(netAdapter);

	WDF_OBJECT_ATTRIBUTES txAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&txAttributes, IGB_TXQUEUE);

	txAttributes.EvtDestroyCallback = EvtTxQueueDestroy;

	NET_PACKET_QUEUE_CONFIG txConfig;
	NET_PACKET_QUEUE_CONFIG_INIT(
		&txConfig,
		EvtTxQueueAdvance,
		EvtTxQueueSetNotificationEnabled,
		EvtTxQueueCancel);
	txConfig.EvtStart = EvtTxQueueStart;
	txConfig.EvtStop = EvtTxQueueStop;

	const ULONG queueId = NetTxQueueInitGetQueueId(txQueueInit);

	NETPACKETQUEUE txQueue;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		NetTxQueueCreate(
			txQueueInit,
			&txAttributes,
			&txConfig,
			&txQueue));

	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);
	tx->QueueId = queueId;

	NET_EXTENSION_QUERY extension;

	NET_EXTENSION_QUERY_INIT(
		&extension,
		NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_NAME,
		NET_FRAGMENT_EXTENSION_LOGICAL_ADDRESS_VERSION_1,
		NetExtensionTypeFragment);
	NetTxQueueGetExtension(txQueue, &extension, &tx->LogicalAddressExtension);

	NET_EXTENSION_QUERY_INIT(
		&extension,
		NET_PACKET_EXTENSION_IEEE8021Q_NAME,
		NET_PACKET_EXTENSION_IEEE8021Q_VERSION_1,
		NetExtensionTypePacket);
	NetTxQueueGetExtension(txQueue, &extension, &tx->Ieee8021qExtension);

	NET_EXTENSION_QUERY_INIT(
		&extension,
		NET_PACKET_EXTENSION_GSO_NAME,
		NET_PACKET_EXTENSION_GSO_VERSION_1,
		NetExtensionTypePacket);
	NetTxQueueGetExtension(txQueue, &extension, &tx->GsoExtension);

	NET_EXTENSION_QUERY_INIT(
		&extension,
		NET_PACKET_EXTENSION_CHECKSUM_NAME,
		NET_PACKET_EXTENSION_CHECKSUM_VERSION_1,
		NetExtensionTypePacket);
	NetTxQueueGetExtension(txQueue, &extension, &tx->ChecksumExtension);

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbTxQueueInitialize(txQueue, adapter));

Exit:
	DBGPRINT("EvtAdapterCreateTxQueue = %x\n", status);

	return status;
}

NTSTATUS
IgbTxQueueInitialize(
	_In_ NETPACKETQUEUE txQueue,
	_In_ IGB_ADAPTER* adapter)
{
	NTSTATUS status = STATUS_SUCCESS;

	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);

	tx->Adapter = adapter;
	tx->Interrupt = adapter->MiscInterrupt;
	tx->Rings = NetTxQueueGetRingCollection(txQueue);

	NET_RING* pr = NetRingCollectionGetPacketRing(tx->Rings);
	NET_RING* fr = NetRingCollectionGetFragmentRing(tx->Rings);
	tx->NumTxDesc = (USHORT)(fr->NumberOfElements > USHORT_MAX ? USHORT_MAX : fr->NumberOfElements);

	WDF_OBJECT_ATTRIBUTES tcbAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&tcbAttributes);
	tcbAttributes.ParentObject = txQueue;
	WDFMEMORY memory = NULL;

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfMemoryCreate(
			&tcbAttributes,
			NonPagedPoolNx,
			0,
			sizeof(IGB_TCB) * pr->NumberOfElements,
			&memory,
			(void**)&tx->PacketContext
		));

	ULONG txSize;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		RtlULongMult(tx->NumTxDesc, sizeof(union e1000_adv_tx_desc), &txSize));

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfCommonBufferCreate(
			tx->Adapter->DmaEnabler,
			txSize,
			WDF_NO_OBJECT_ATTRIBUTES,
			&tx->TxdArray));

	tx->TxdBase = static_cast<union e1000_adv_tx_desc*>(
		WdfCommonBufferGetAlignedVirtualAddress(tx->TxdArray));
	tx->TxSize = txSize;

Exit:
	return status;
}

static
IGB_TCB*
GetTcbFromPacket(
	_In_ IGB_TXQUEUE* tx,
	_In_ UINT32 Index)
{
	return &tx->PacketContext[Index];
}

static
void
IgbPostContextDescriptor(
	_In_ IGB_TXQUEUE* tx,
	_In_ IGB_TCB* tcb,
	_In_ NET_PACKET const* packet,
	_In_ UINT32 packetIndex,
	_Inout_ u32 &cmd_type_len,
	_Inout_ u32 &olinfo_status)
{
	struct e1000_adv_tx_context_desc* ctxd = (struct e1000_adv_tx_context_desc*)&tx->TxdBase[tx->TxDescIndex];
	IGB_ADAPTER* adapter = tx->Adapter;
	bool checksumEnabled = (adapter->TxTcpHwChkSum || adapter->TxIpHwChkSum || adapter->TxUdpHwChkSum);
	bool lsoEnabled = (adapter->LSOv4 || adapter->LSOv6);
	bool contextUsed = false;

	RtlZeroMemory(ctxd, sizeof(*ctxd));
	ctxd->type_tucmd_mlhl = E1000_ADVTXD_DCMD_DEXT | E1000_ADVTXD_DTYP_CTXT;

	if (tx->Ieee8021qExtension.Enabled)
	{
		u16 vlanId = 0;
		NET_PACKET_IEEE8021Q* ieee8021q = NetExtensionGetPacketIeee8021Q(&tx->Ieee8021qExtension, packetIndex);
		if (ieee8021q->TxTagging & NetPacketTxIeee8021qActionFlagPriorityRequired)
			vlanId |= ieee8021q->PriorityCodePoint << 13;
		if (ieee8021q->TxTagging & NetPacketTxIeee8021qActionFlagVlanRequired)
			vlanId |= ieee8021q->VlanIdentifier;

		// vlanId = ((vlanId >> 8) & 0xff) | ((vlanId << 8) & 0xff00); -- Do we need to flip bytes?
		ctxd->vlan_macip_lens |= vlanId << E1000_ADVTXD_VLAN_SHIFT;
		cmd_type_len |= E1000_ADVTXD_DCMD_VLE;
		contextUsed = true;
	}

	if (tx->GsoExtension.Enabled && lsoEnabled)
	{
		NET_PACKET_GSO* gsoInfo = NetExtensionGetPacketGso(&tx->GsoExtension, packetIndex);
		UINT32 mss = packet->Layout.Layer4Type == NetPacketLayer4TypeTcp ? gsoInfo->TCP.Mss : gsoInfo->UDP.Mss;

		if (mss > 0)
		{
			cmd_type_len |= E1000_ADVTXD_DCMD_TSE;
			if (NetPacketIsIpv4(packet))
				olinfo_status |= E1000_TXD_POPTS_IXSM << 8;
			olinfo_status |= E1000_TXD_POPTS_TXSM << 8;
			ctxd->mss_l4len_idx |= mss << E1000_ADVTXD_MSS_SHIFT;
		}
	}

	if (tx->ChecksumExtension.Enabled && checksumEnabled)
	{
		NET_PACKET_CHECKSUM* checksumInfo = NetExtensionGetPacketChecksum(&tx->ChecksumExtension, packetIndex);
		if (checksumInfo->Layer3 == NetPacketTxChecksumActionRequired)
			olinfo_status |= E1000_TXD_POPTS_IXSM << 8;
		if (checksumInfo->Layer4 == NetPacketTxChecksumActionRequired)
			olinfo_status |= E1000_TXD_POPTS_TXSM << 8;
	}

	if ((tx->ChecksumExtension.Enabled && checksumEnabled) || (tx->GsoExtension.Enabled && lsoEnabled))
	{
		ctxd->vlan_macip_lens |= packet->Layout.Layer2HeaderLength << E1000_ADVTXD_MACLEN_SHIFT;
		ctxd->vlan_macip_lens |= packet->Layout.Layer3HeaderLength;
		ctxd->mss_l4len_idx |= packet->Layout.Layer4HeaderLength << E1000_ADVTXD_L4LEN_SHIFT;
		if (NetPacketIsIpv4(packet))
			ctxd->type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_IPV4;
		else if (NetPacketIsIpv6(packet))
		    ctxd->type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_IPV6;
		if (packet->Layout.Layer4Type == NetPacketLayer4TypeTcp)
			ctxd->type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_L4T_TCP;
		else if (packet->Layout.Layer4Type == NetPacketLayer4TypeUdp)
			ctxd->type_tucmd_mlhl |= E1000_ADVTXD_TUCMD_L4T_UDP;

		contextUsed = true;
	}

	if (contextUsed)
	{
		tx->TxDescIndex = (tx->TxDescIndex + 1) % tx->NumTxDesc;
		tcb->NumTxDesc++;
	}
}

static
void
IgbFlushTransaction(
	_In_ IGB_TXQUEUE* tx)
{
	MemoryBarrier();
	E1000_WRITE_REG(&tx->Adapter->Hw, E1000_TDT(tx->QueueId), tx->TxDescIndex);
}

static
bool
IgbIsPacketTransferComplete(
	_In_ IGB_TXQUEUE* tx,
	_In_ IGB_TCB* tcb)
{
	size_t lastTxDescIdx = (tcb->FirstTxDescIdx + tcb->NumTxDesc - 1) % tx->NumTxDesc;
	union e1000_adv_tx_desc* txd = &tx->TxdBase[lastTxDescIdx];
	return (txd->wb.status & E1000_TXD_STAT_DD) != 0;
}

static
void
IgbTransmitPackets(
	_In_ IGB_TXQUEUE* tx)
{
	bool programmedPackets = false;

	NET_RING *packetRing = NetRingCollectionGetPacketRing(tx->Rings);
	NET_RING* fragmentRing = NetRingCollectionGetFragmentRing(tx->Rings);
	NET_PACKET* packet;
	NET_FRAGMENT* fragment;
	ULONG packetIndex = packetRing->NextIndex;
	ULONG fragmentIndex;
	ULONG fragmentEndIndex;
	ULONG availableDescriptors;

	availableDescriptors = tx->NumTxDesc - tx->TxDescIndex;

	while (packetIndex != packetRing->EndIndex)
	{
		packet = NetRingGetPacketAtIndex(packetRing, packetIndex);
		if (!packet->Ignore)
		{
			// Bail out if there are not enough TX descriptors to describe the
			// whole packet. We reserve a space for one extra context descriptor
			// even if it's not always used.
			if (packet->FragmentCount + 1 > availableDescriptors)
			{
				break;
			}

			IGB_TCB* tcb = GetTcbFromPacket(tx, packetIndex);
			u32 cmd_type_len = E1000_ADVTXD_DCMD_DEXT | E1000_ADVTXD_DTYP_DATA | E1000_ADVTXD_DCMD_IFCS;
			u32 olinfo_status = 0;
			u32 packet_length = 0;

			tcb->FirstTxDescIdx = tx->TxDescIndex;
			tcb->NumTxDesc = 0;

			IgbPostContextDescriptor(tx, tcb, packet, packetIndex, cmd_type_len, olinfo_status);

			fragmentIndex = packet->FragmentIndex;
			fragmentEndIndex = NetRingAdvanceIndex(fragmentRing, fragmentIndex, packet->FragmentCount);

			// Calculate the full packet length
			for (int i = 0; fragmentIndex != fragmentEndIndex; i++)
			{
				NET_FRAGMENT const* fragment = NetRingGetFragmentAtIndex(fragmentRing, fragmentIndex);
				packet_length += (u16)fragment->ValidLength;
				fragmentIndex = NetRingIncrementIndex(fragmentRing, fragmentIndex);
			}
			olinfo_status |= packet_length << E1000_ADVTXD_PAYLEN_SHIFT;

			fragmentIndex = packet->FragmentIndex;
			union e1000_adv_tx_desc* lastTxd = 0;
			for (; fragmentIndex != fragmentEndIndex; tcb->NumTxDesc++)
			{
				NET_FRAGMENT const* fragment = NetRingGetFragmentAtIndex(fragmentRing, fragmentIndex);
				union e1000_adv_tx_desc* txd = lastTxd = &tx->TxdBase[tx->TxDescIndex];
				NET_FRAGMENT_LOGICAL_ADDRESS const* logicalAddress = NetExtensionGetFragmentLogicalAddress(
					&tx->LogicalAddressExtension, fragmentIndex);

				RtlZeroMemory(txd, sizeof(*txd));
				txd->read.buffer_addr = logicalAddress->LogicalAddress + fragment->Offset;
				txd->read.cmd_type_len = cmd_type_len | (u16)fragment->ValidLength;
				txd->read.olinfo_status = olinfo_status;
				tx->TxDescIndex = (tx->TxDescIndex + 1) % tx->NumTxDesc;

				fragmentIndex = NetRingIncrementIndex(fragmentRing, fragmentIndex);
			}
			lastTxd->read.cmd_type_len |= E1000_ADVTXD_DCMD_EOP | E1000_ADVTXD_DCMD_RS;
			fragmentRing->NextIndex = fragmentIndex;

			programmedPackets = true;
			availableDescriptors -= tcb->NumTxDesc;
		}

		packetIndex = NetRingIncrementIndex(packetRing, packetIndex);
	}
	packetRing->NextIndex = packetIndex;

	if (programmedPackets)
	{
		IgbFlushTransaction(tx);
	}
}

static
void
IgbCompleteTransmitPackets(
	_In_ IGB_TXQUEUE* tx)
{
	NET_RING* packetRing = NetRingCollectionGetPacketRing(tx->Rings);
	NET_RING* fragmentRing = NetRingCollectionGetFragmentRing(tx->Rings);
	NET_PACKET* packet;
	ULONG packetIndex = packetRing->BeginIndex;
	ULONG fragmentIndex = fragmentRing->BeginIndex;

	while (packetIndex != packetRing->NextIndex)
	{
		packet = NetRingGetPacketAtIndex(packetRing, packetIndex);
		if (!packet->Ignore)
		{
			IGB_TCB* tcb = GetTcbFromPacket(tx, packetIndex);
			if (!IgbIsPacketTransferComplete(tx, tcb))
			{
				break;
			}
			fragmentIndex = NetRingAdvanceIndex(fragmentRing, fragmentIndex, packet->FragmentCount);
		}
		packetIndex = NetRingIncrementIndex(packetRing, packetIndex);
	}
	packetRing->BeginIndex = packetIndex;
	fragmentRing->BeginIndex = fragmentIndex;
}

_Use_decl_annotations_
void
EvtTxQueueAdvance(
	_In_ NETPACKETQUEUE txQueue)
{
	DBGPRINT_ADVANCE("EvtTxQueueAdvance\n");

	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);

	IgbTransmitPackets(tx);
	IgbCompleteTransmitPackets(tx);

	DBGPRINT_ADVANCE("EvtTxQueueAdvance - Done\n");
}

void
IgbTxQueueSetInterrupt(
	_In_ IGB_TXQUEUE* tx,
	_In_ BOOLEAN notificationEnabled)
{
	WdfInterruptAcquireLock(tx->Interrupt->Handle);
	E1000_WRITE_REG(&tx->Adapter->Hw, notificationEnabled ? E1000_IMS : E1000_IMC, E1000_IMS_TXDW);
	E1000_WRITE_FLUSH(&tx->Adapter->Hw);
	WdfInterruptReleaseLock(tx->Interrupt->Handle);

	if (!notificationEnabled)
	{
		// block this thread until we're sure any outstanding DPCs are complete.
		// This is to guarantee we don't return from this function call until
		// any outstanding tx notification is complete.
		KeFlushQueuedDpcs();
	}
}

_Use_decl_annotations_
void
EvtTxQueueStart(
	_In_ NETPACKETQUEUE txQueue)
{
	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);
	IGB_ADAPTER* adapter = tx->Adapter;
	struct e1000_hw* hw = &tx->Adapter->Hw;

	DBGPRINT("EvtTxQueueStart\n");

	ASSERT(tx->QueueId < IGB_MAX_TX_QUEUES);

	RtlZeroMemory(tx->TxdBase, tx->TxSize);

	tx->TxDescIndex = 0;

	// Disable transmits
	u32 tctl = E1000_READ_REG(hw, E1000_TCTL);
	E1000_WRITE_REG(hw, E1000_TCTL, tctl & ~E1000_TCTL_EN);

	// Setup the descriptors
	PHYSICAL_ADDRESS pa = WdfCommonBufferGetAlignedLogicalAddress(tx->TxdArray);
	E1000_WRITE_REG(hw, E1000_TDLEN(tx->QueueId), tx->TxSize);
	E1000_WRITE_REG(hw, E1000_TDBAH(tx->QueueId), (u32)pa.HighPart);
	E1000_WRITE_REG(hw, E1000_TDBAL(tx->QueueId), (u32)pa.LowPart);

	// Head & tail
	E1000_WRITE_REG(hw, E1000_TDH(tx->QueueId), 0);
	E1000_WRITE_REG(hw, E1000_TDT(tx->QueueId), 0);

	// Program the Transmit Control Register
	tctl &= ~E1000_TCTL_CT;
	tctl |= (E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_TCTL_EN | (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT));

	// Enable this queue
	u32 txdctl = E1000_READ_REG(hw, E1000_TXDCTL(tx->QueueId));
	txdctl |= E1000_TXDCTL_QUEUE_ENABLE;
	txdctl &= 0xFFF00000;
	txdctl |= /*IGB_TX_PTHRESH*/8;
	txdctl |= /*IGB_TX_HTHRESH*/1 << 8;
	txdctl |= /*IGB_TX_WTHRESH*/1 << 16;
	E1000_WRITE_REG(hw, E1000_TXDCTL(tx->QueueId), txdctl);

	WdfSpinLockAcquire(adapter->Lock);

	adapter->TxQueues[tx->QueueId] = txQueue;

	WdfSpinLockRelease(adapter->Lock);

	// This write will effectively turn on the transmit unit.
	E1000_WRITE_REG(hw, E1000_TCTL, tctl);
}

_Use_decl_annotations_
void
EvtTxQueueStop(
	NETPACKETQUEUE txQueue)
{
	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);
	struct e1000_hw* hw = &tx->Adapter->Hw;

	DBGPRINT("EvtTxQueueStop\n");

	ASSERT(tx->QueueId < IGB_MAX_TX_QUEUES);

	WdfSpinLockAcquire(tx->Adapter->Lock);

	IgbTxQueueSetInterrupt(tx, false);

	tx->Adapter->TxQueues[tx->QueueId] = WDF_NO_HANDLE;

	WdfSpinLockRelease(tx->Adapter->Lock);
}

_Use_decl_annotations_
void
EvtTxQueueDestroy(
	_In_ WDFOBJECT txQueue)
{
	DBGPRINT("EvtTxQueueDestroy\n");

	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);

	if (tx->TxdArray)
		WdfObjectDelete(tx->TxdArray);
	tx->TxdArray = NULL;
}

_Use_decl_annotations_
VOID
EvtTxQueueSetNotificationEnabled(
	_In_ NETPACKETQUEUE txQueue,
	_In_ BOOLEAN notificationEnabled)
{
	DBGPRINT_ADVANCE("EvtTxQueueSetNotificationEnabled\n");

	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);
	IgbTxQueueSetInterrupt(tx, notificationEnabled);
}

_Use_decl_annotations_
void
EvtTxQueueCancel(
	_In_ NETPACKETQUEUE txQueue)
{
	DBGPRINT("EvtTxQueueCancel\n");

	IGB_TXQUEUE* tx = IgbGetTxQueueContext(txQueue);
	struct e1000_hw* hw = &tx->Adapter->Hw;

	WdfSpinLockAcquire(tx->Adapter->Lock);

	// Disable the queue
	u32 txdctl = E1000_READ_REG(hw, E1000_TXDCTL(tx->QueueId));
	txdctl &= ~E1000_TXDCTL_QUEUE_ENABLE;
	E1000_WRITE_REG(hw, E1000_TXDCTL(tx->QueueId), txdctl);

	WdfSpinLockRelease(tx->Adapter->Lock);

	NET_RING* pr = NetRingCollectionGetPacketRing(tx->Rings);

	while (pr->BeginIndex != pr->EndIndex)
	{
		NET_PACKET* packet = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
		packet->Ignore = 1;

		pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
	}

	NET_RING* fr = NetRingCollectionGetFragmentRing(tx->Rings);

	fr->BeginIndex = fr->EndIndex;
}