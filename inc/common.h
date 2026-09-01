/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    common.h

Environment:

    User mode

--*/

#ifndef __VHIDMINI_COMMON_H__
#define __VHIDMINI_COMMON_H__

//
// Report ID of the touch collection (see the HID report descriptor).
//
#define CONTROL_COLLECTION_REPORT_ID                      0x54

#define VHIDMINI_MANUFACTURER_STRING    L"Goodix"
#define VHIDMINI_PRODUCT_STRING         L"GTX9886 Touchscreen Controller"
#define VHIDMINI_SERIAL_NUMBER_STRING   L"0001"
#define VHIDMINI_DEVICE_STRING          L"GTX9886 Touchscreen Controller Device"
#define VHIDMINI_DEVICE_STRING_INDEX    5
#include <pshpack1.h>

//
// Output report from system to device. The touch report descriptor has
// no writable output data, but this legacy sample structure's size is
// still used to validate and acknowledge IOCTL_HID_WRITE_REPORT and
// IOCTL_HID_SET_OUTPUT_REPORT.
//
typedef struct _HIDMINI_OUTPUT_REPORT {

    UCHAR ReportId;

    UCHAR Data;

    USHORT Pad1;

    ULONG Pad2;

} HIDMINI_OUTPUT_REPORT, *PHIDMINI_OUTPUT_REPORT;

#include <poppack.h>

#endif //__VHIDMINI_COMMON_H__
