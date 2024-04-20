#include "precomp.h"
#include "trace.h"
#include "adapter.h"
#include "interrupt.h"
#include "link.h"

NTSTATUS
IgbInterruptCreate(
	_In_ WDFDEVICE wdfDevice,
	_In_ IGB_ADAPTER* adapter,
	_Out_ IGB_INTERRUPT** interrupt)
{
	DBGPRINT("IntelInterruptCreate");

	*interrupt = nullptr;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, IGB_INTERRUPT);

	WDF_INTERRUPT_CONFIG config;
	WDF_INTERRUPT_CONFIG_INIT(&config, EvtInterruptIsr, EvtInterruptDpc);

	NTSTATUS status = STATUS_SUCCESS;

	WDFINTERRUPT wdfInterrupt;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfInterruptCreate(wdfDevice, &config, &attributes, &wdfInterrupt));

	*interrupt = IgbGetInterruptContext(wdfInterrupt);

	(*interrupt)->Adapter = adapter;
	(*interrupt)->Handle = wdfInterrupt;

Exit:
	return status;
}

_Use_decl_annotations_
BOOLEAN
EvtInterruptIsr(
	_In_ WDFINTERRUPT wdfInterrupt,
	ULONG MessageID)
{
	UNREFERENCED_PARAMETER((MessageID));

	IGB_INTERRUPT* interrupt = IgbGetInterruptContext(wdfInterrupt);
	IGB_ADAPTER* adapter = interrupt->Adapter;
	struct e1000_hw* hw = &adapter->Hw;
	u32 reg_icr;
	BOOLEAN queueDPC = FALSE;

	reg_icr = E1000_READ_REG(hw, E1000_ICR);

	// Hot eject
	if (reg_icr == 0xffffffff)
	{
		return TRUE;
	}

	// Stray interrupts
	if (reg_icr == 0x0 || (reg_icr & E1000_ICR_INT_ASSERTED) == 0)
	{
		return TRUE;
	}

	if ((reg_icr & (E1000_ICR_FER | E1000_ICR_DOUTSYNC | E1000_ICR_DRSTA | E1000_ICR_ECCER | E1000_ICR_THS)) &&
		!InterlockedExchange8(&interrupt->PciInterrupt, TRUE))
	{
		WdfInterruptQueueDpcForIsr(wdfInterrupt);
		return TRUE;
	}

	/* Rx interrupt */
	if ((reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0 | E1000_ICR_RXCFG)) &&
		!InterlockedExchange8(&interrupt->RxInterrupt, TRUE))
	{
		queueDPC = TRUE;
	}

	/* Tx interrupt */
	if ((reg_icr & (E1000_ICR_TXDW | E1000_ICR_TXQE)) &&
		!InterlockedExchange8(&interrupt->TxInterrupt, TRUE))
	{
		queueDPC = TRUE;
	}

	if ((reg_icr & (E1000_ICR_LSC)) &&
		!InterlockedExchange8(&interrupt->LinkChange, TRUE))
	{
		queueDPC = TRUE;
	}

	if (queueDPC)
	{
		WdfInterruptQueueDpcForIsr(wdfInterrupt);
	}

	return TRUE;
}

static
void
IgbRxNotify(
	_In_ IGB_INTERRUPT* interrupt,
	_In_ ULONG queueId)
{
	if (InterlockedExchange(&interrupt->RxNotifyArmed[queueId], false))
	{
		NetRxQueueNotifyMoreReceivedPacketsAvailable(interrupt->Adapter->RxQueues[queueId]);
	}
}

_Use_decl_annotations_
VOID
EvtInterruptDpc(
	_In_ WDFINTERRUPT Interrupt,
	_In_ WDFOBJECT AssociatedObject)
{
	UNREFERENCED_PARAMETER(AssociatedObject);

	IGB_INTERRUPT* interrupt = IgbGetInterruptContext(Interrupt);
	IGB_ADAPTER* adapter = interrupt->Adapter;
	struct e1000_hw* hw = &adapter->Hw;

	if (InterlockedExchange8(&interrupt->PciInterrupt, FALSE))
	{
		DBGPRINT("PCI Interrupt!\n");
	}

	if (InterlockedExchange8(&interrupt->TxInterrupt, FALSE))
	{
		//DBGPRINT("TX Interrupt!\n");
		if (InterlockedExchange(&interrupt->TxNotifyArmed, false))
		{
			NetTxQueueNotifyMoreCompletedPacketsAvailable(adapter->TxQueues[0]);
		}
	}

	if (InterlockedExchange8(&interrupt->RxInterrupt, FALSE))
	{
		//DBGPRINT("RX Interrupt!\n");
		IgbRxNotify(interrupt, 0);
	}

	if (InterlockedExchange8(&interrupt->LinkChange, FALSE))
	{
		DBGPRINT("Link Interrupt!\n");
		hw->mac.get_link_status = 1;
		IgbCheckLinkStatus(adapter);
	}
}