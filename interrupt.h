#pragma once

typedef struct _IGB_INTERRUPT
{
	IGB_ADAPTER* Adapter;
	WDFINTERRUPT Handle;

	// Armed Notifications
	LONG RxNotifyArmed[IGB_MAX_RX_QUEUES];
	LONG TxNotifyArmed;

	char PciInterrupt;
	char RxInterrupt;
	char TxInterrupt;
	char LinkChange;
} IGB_INTERRUPT, *PIGB_INTERRUPT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IGB_INTERRUPT, IgbGetInterruptContext);

NTSTATUS
IgbInterruptCreate(
	_In_ WDFDEVICE wdfDevice,
	_In_ IGB_ADAPTER* adapter,
	_Out_ IGB_INTERRUPT** interrupt);

EVT_WDF_INTERRUPT_ISR EvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC EvtInterruptDpc;