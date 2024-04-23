#pragma once

//--------------------------------------
// TCB (Transmit Control Block)
//--------------------------------------

typedef struct _IGB_TCB
{
	USHORT FirstTxDescIdx;
	ULONG NumTxDesc;
} IGB_TCB;

typedef struct _IGB_TXQUEUE {
	IGB_ADAPTER* Adapter;
	IGB_INTERRUPT* Interrupt;

	NET_RING_COLLECTION const* Rings;
	IGB_TCB* PacketContext;

	// descriptor information
	WDFCOMMONBUFFER TxdArray;
	union e1000_adv_tx_desc* TxdBase;
	size_t TxSize;

	USHORT NumTxDesc;
	USHORT TxDescIndex;

	NET_EXTENSION LogicalAddressExtension;
	NET_EXTENSION Ieee8021qExtension;
	NET_EXTENSION GsoExtension;
	NET_EXTENSION ChecksumExtension;

	ULONG QueueId;
	UINT8 Priority;
} IGB_TXQUEUE, *PIGB_TXQUEUE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IGB_TXQUEUE, IgbGetTxQueueContext);

NTSTATUS IgbTxQueueInitialize(_In_ NETPACKETQUEUE txQueue, _In_ IGB_ADAPTER* adapter);

EVT_WDF_OBJECT_CONTEXT_DESTROY EvtTxQueueDestroy;

EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtTxQueueSetNotificationEnabled;
EVT_PACKET_QUEUE_ADVANCE EvtTxQueueAdvance;
EVT_PACKET_QUEUE_CANCEL EvtTxQueueCancel;
EVT_PACKET_QUEUE_START EvtTxQueueStart;
EVT_PACKET_QUEUE_STOP EvtTxQueueStop;