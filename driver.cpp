#include "precomp.h"

#include "device.h"
#include "trace.h"
#include "adapter.h"
#include "power.h"
#include "interrupt.h"

EXTERN_C __declspec(code_seg("INIT")) DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;

_Use_decl_annotations_
__declspec(code_seg("INIT"))
NTSTATUS
DriverEntry(
	_In_ PDRIVER_OBJECT driverObject,
	_In_ PUNICODE_STRING registryPath)
{
	NTSTATUS status = STATUS_SUCCESS;

	DBGPRINT("DriverEntry\n");

	WDF_DRIVER_CONFIG driverConfig;
	WDF_DRIVER_CONFIG_INIT(&driverConfig, EvtDriverDeviceAdd);

	driverConfig.DriverPoolTag = 'I1GB';
	driverConfig.EvtDriverUnload = EvtDriverUnload;

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &driverConfig, NULL));

Exit:

	return status;
}

_Use_decl_annotations_
NTSTATUS
#pragma prefast(suppress: __WARNING_EXCESSIVESTACKUSAGE, "TVS:12813961 call stack depth well-defined")
EvtDriverDeviceAdd(
	_In_ WDFDRIVER driver,
	_Inout_ PWDFDEVICE_INIT deviceInit)
{
	UNREFERENCED_PARAMETER((driver));

	DBGPRINT("EvtDriverDeviceAdd\n");

	NTSTATUS status = STATUS_SUCCESS;
	NETADAPTER_INIT* adapterInit = nullptr;

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		NetDeviceInitConfig(deviceInit));

	WDF_OBJECT_ATTRIBUTES deviceAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, IGB_DEVICE);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
		pnpPowerCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
		pnpPowerCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
		pnpPowerCallbacks.EvtDeviceD0Entry = EvtDeviceD0Entry;
		pnpPowerCallbacks.EvtDeviceD0Exit = EvtDeviceD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPowerCallbacks);
	}

	// TODO: WdfDeviceInitSetPowerPolicyEventCallbacks

	WDFDEVICE wdfDevice;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfDeviceCreate(&deviceInit, &deviceAttributes, &wdfDevice));

	WdfDeviceSetAlignmentRequirement(wdfDevice, FILE_256_BYTE_ALIGNMENT);

	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;
	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings, IdleCannotWakeFromS0);
	idleSettings.UserControlOfIdleSettings = IdleAllowUserControl;

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		WdfDeviceAssignS0IdleSettings(wdfDevice, &idleSettings));

	// TODO: WdfDeviceAssignSxWakeSettings

	adapterInit = NetAdapterInitAllocate(wdfDevice);

	GOTO_WITH_INSUFFICIENT_RESOURCES_IF_NULL(Exit, status, adapterInit);

	NET_ADAPTER_DATAPATH_CALLBACKS datapathCallbacks;
	NET_ADAPTER_DATAPATH_CALLBACKS_INIT(
		&datapathCallbacks,
		EvtAdapterCreateTxQueue,
		EvtAdapterCreateRxQueue);

	NetAdapterInitSetDatapathCallbacks(
		adapterInit,
		&datapathCallbacks);

	WDF_OBJECT_ATTRIBUTES adapterAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttributes, IGB_ADAPTER);

	NETADAPTER netAdapter;
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		NetAdapterCreate(adapterInit, &adapterAttributes, &netAdapter));

	PIGB_ADAPTER adapter = IgbGetAdapterContext(netAdapter);
	PIGB_DEVICE device = IgbGetDeviceContext(wdfDevice);

	device->Adapter = adapter;

	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbInitializeAdapterContext(adapter, wdfDevice, netAdapter));
	GOTO_IF_NOT_NT_SUCCESS(Exit, status,
		IgbInterruptCreate(wdfDevice, adapter, &adapter->Interrupt));

Exit:
	if (adapterInit != nullptr)
	{
		NetAdapterInitFree(adapterInit);
	}

	DBGPRINT("EvtDriverDeviceAdd = %x\n", status);

	return status;
}

_Use_decl_annotations_
VOID
EvtDriverUnload(
	_In_ WDFDRIVER driver)
{
	UNREFERENCED_PARAMETER((driver));
	DBGPRINT("EvtDriverUnload\n");
}