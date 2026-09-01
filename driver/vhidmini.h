/*++

Copyright (C) Microsoft Corporation, All Rights Reserved

Module Name:

    vhidmini.h

Abstract:

    This module contains the type definitions for the driver

Environment:

    Windows Driver Framework (WDF)

--*/

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#include <ntddk.h>
#include <wdm.h>
#include <ntstrsafe.h>

#include <wdf.h>

#include <hidport.h>  // located in $(DDK_INC_PATH)/wdm

#include "common.h"

#define RESHUB_USE_HELPER_ROUTINES
#include "reshub.h"

typedef UCHAR HID_REPORT_DESCRIPTOR, *PHID_REPORT_DESCRIPTOR;

//
// Touch input report (report ID 0x54) layout:
//   reportId (1) + 10 contacts * 6 bytes (state, contact id, X lo/hi, Y lo/hi)
//   + contact count (1) = 62 bytes.
// Contact state byte: bit0 = tip switch, bit1 = in range, bit2 = confidence.
//
#define TOUCH_REPORT_SLOT_COUNT 3
#define TOUCH_INPUT_REPORT_SIZE 62
#define MAX_POINT_NUM 0xA
#define TOUCH_REPORT_DESCRIPTOR_SIZE 554

//
// Per-request synchronization slot for bounded-wait SPB transfers. Each
// slot owns its event, so a request that stays in flight after a timeout
// can never signal (and thereby falsely wake) a later request's wait.
// Slots are only touched from the serialized passive-level ISR, so no
// additional locking is needed.
//
#define SPB_MAX_INFLIGHT 4

typedef struct _SPB_SYNC_SLOT {
    KEVENT                  Event;
    BOOLEAN                 InUse;
} SPB_SYNC_SLOT;

typedef struct __declspec(align(2))
{
    BYTE  reportId;
    BYTE  points[60];
    BYTE  DIG_TouchScreenContactCount;
} inputReport54_t;

C_ASSERT(sizeof(inputReport54_t) == TOUCH_INPUT_REPORT_SIZE);

DRIVER_INITIALIZE                   DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           EvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP      EvtDriverCleanup;

EVT_WDF_DEVICE_PREPARE_HARDWARE      OnPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE      OnReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY              OnD0Entry;
EVT_WDF_DEVICE_D0_EXIT               OnD0Exit;

typedef struct _DEVICE_CONTEXT
{
    WDFDEVICE               Device;
    WDFQUEUE                DefaultQueue;
    WDFQUEUE                ManualQueue;
    HID_DEVICE_ATTRIBUTES   HidDeviceAttributes;
    HID_DESCRIPTOR          HidDescriptor;
    PHID_REPORT_DESCRIPTOR  ReportDescriptor;
    HID_REPORT_DESCRIPTOR   ReportDescriptorStorage[TOUCH_REPORT_DESCRIPTOR_SIZE];

    LARGE_INTEGER           PeripheralId;
    WDFINTERRUPT            Interrupt;
    WDFIOTARGET             SpbController;
    BOOLEAN                 OnClose;
    BOOLEAN                 LastActiveReportValid;
    inputReport54_t         LastActiveReport;
    UINT8                   ActiveIds[MAX_POINT_NUM];
    UINT8                   ActiveCount;
    SPB_SYNC_SLOT           SpbSlots[SPB_MAX_INFLIGHT];
    ULONG                   RequestEventLogCount;

    //
    // Per-instance coordinate configuration (read from the device
    // registry key at EvtDeviceAdd).
    //
    ULONG                   XRevert;
    ULONG                   YRevert;
    ULONG                   XYExchange;
    ULONG                   XMin;
    ULONG                   XMax;
    ULONG                   YMin;
    ULONG                   YMax;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

typedef struct _QUEUE_CONTEXT
{
    WDFQUEUE                Queue;
    PDEVICE_CONTEXT         DeviceContext;

} QUEUE_CONTEXT, *PQUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CONTEXT, GetQueueContext);

NTSTATUS
QueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    );

typedef struct _MANUAL_QUEUE_CONTEXT
{
    WDFQUEUE                Queue;
    PDEVICE_CONTEXT         DeviceContext;

    //
    // Touch report delivery state machine. ReportLock serializes the ISR and
    // the read-report path; PendingReports is a ring buffer of reports
    // waiting for a HID read request. DeliveryInProgress guarantees only one
    // delivery loop runs at a time; DeliveryIdleEvent is signaled whenever no
    // delivery loop is active (used by OnD0Exit to wait out in-flight
    // completions before deleting objects).
    //
    WDFSPINLOCK             ReportLock;
    inputReport54_t         PendingReports[TOUCH_REPORT_SLOT_COUNT];
    UCHAR                   PendingHead;
    UCHAR                   PendingCount;
    BOOLEAN                 DeliveryInProgress;
    KEVENT                  DeliveryIdleEvent;

} MANUAL_QUEUE_CONTEXT, *PMANUAL_QUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MANUAL_QUEUE_CONTEXT, GetManualQueueContext);

NTSTATUS
ManualQueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    );

NTSTATUS
ReadReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request,
    _Always_(_Out_)
          BOOLEAN*          CompleteRequest
    );

NTSTATUS
WriteReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
SetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetInputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
SetOutputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetString(
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetIndexedString(
    _In_  WDFREQUEST        Request
    );

NTSTATUS
GetStringId(
    _In_  WDFREQUEST        Request,
    _Out_ ULONG            *StringId,
    _Out_ ULONG            *LanguageId
    );

NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _When_(NumBytesToCopyFrom == 0, __drv_reportError(NumBytesToCopyFrom cannot be zero))
    _In_  size_t            NumBytesToCopyFrom
    );

NTSTATUS
RequestGetHidXferPacket_ToReadFromDevice(
    _In_  WDFREQUEST        Request,
    _Out_ HID_XFER_PACKET  *Packet
    );

NTSTATUS
RequestGetHidXferPacket_ToWriteToDevice(
    _In_  WDFREQUEST        Request,
    _Out_ HID_XFER_PACKET  *Packet
    );

BOOLEAN
OnInterruptIsr(
    _In_  WDFINTERRUPT FxInterrupt,
    _In_  ULONG        MessageID
);

NTSTATUS
SpbDeviceOpen(
    _In_  PDEVICE_CONTEXT  pDevice
);
VOID
SpbDeviceClose(
    _In_  PDEVICE_CONTEXT  pDevice
);
VOID
SpbDeviceWrite(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ PVOID pInputBuffer,
    _In_ size_t inputBufferLength
);
VOID
SpbDeviceWriteRead(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ PVOID pInputBuffer,
    _In_ PVOID pOutputBuffer,
    _In_ size_t inputBufferLength,
    _In_ size_t outputBufferLength
);

NTSTATUS
ReadCoordinateConfigFromRegistry(
    WDFDEVICE Device
);

//
// Misc definitions
//
#define CONTROL_FEATURE_REPORT_ID   0x54

//
// These are the device attributes returned by the mini driver in response
// to IOCTL_HID_GET_DEVICE_ATTRIBUTES.
//
#define HIDMINI_PID             0x9886
#define HIDMINI_VID             0x27C6
#define HIDMINI_VERSION         0x0100
