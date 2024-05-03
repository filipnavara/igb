#include "precomp.h"

#include "trace.h"
#include "power.h"
#include "device.h"
#include "adapter.h"
#include "link.h"
#include "interrupt.h"

_Use_decl_annotations_
NTSTATUS
EvtDeviceD0Entry(
    _In_ WDFDEVICE wdfDevice,
    WDF_POWER_DEVICE_STATE previousState)
{
    IGB_ADAPTER* adapter = IgbGetDeviceContext(wdfDevice)->Adapter;

    IgbInterruptInitialize(adapter);
    IgbResetLink(adapter);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
EvtDeviceD0Exit(
    _In_ WDFDEVICE wdfDevice,
    _In_ WDF_POWER_DEVICE_STATE targetState)
{
    IGB_ADAPTER* adapter = IgbGetDeviceContext(wdfDevice)->Adapter;

    return STATUS_SUCCESS;
}
