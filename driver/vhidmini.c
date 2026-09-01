/*++

Copyright (C) Microsoft Corporation, All Rights Reserved.

Module Name:

    vhidmini.cpp

Abstract:

    This module contains the implementation of the driver

Environment:

    Windows Driver Framework (WDF)

--*/
#include "vhidmini.h"

#ifdef DBG
#include "kmdf/trace.h"
#include "vhidmini.tmh"
#endif

#define GOODIX_TOUCH_EVENT 0x80
#define GOODIX_REQUEST_EVENT 0x40
#define GOODIX_GESTURE_EVENT    0x20
#define GOODIX_HOTKNOT_EVENT    0x10
#define BYTES_PER_COORD 0x8
#define BYTES_CHKSUM 0x2
#define SPB_TRANSFER_TIMEOUT_MS   100
#define SPB_CANCEL_TIMEOUT_MS     100

const BYTE gtx9886_get_pre_coor[2] = { 0x41, 0x00 };
const BYTE gtx9886_get_coor[2] = { 0x41, 0x0c };
const BYTE gtx9886_clean_coor[3] = { 0x41, 0x00, 0x00};

//
// Transform one touch point from the panel coordinate space to the
// reported HID coordinate space according to the per-instance registry
// configuration (XRevert / YRevert / XYExchange). Points are clamped to
// the configured panel bounds first.
//
static VOID
TransformPoint(
    _In_ PDEVICE_CONTEXT pDevice,
    _Inout_ int* pX,
    _Inout_ int* pY
)
{
    ULONG xMin, xMax, yMin, yMax;
    ULONG x, y, t;

    //
    // Hardware coordinates are unsigned 12-bit values. Defensively treat
    // any negative input as zero so clamping lands on the minimum edge
    // instead of wrapping around to the maximum edge.
    //
    x = (*pX < 0) ? 0 : (ULONG)*pX;
    y = (*pY < 0) ? 0 : (ULONG)*pY;

    //
    // If the axes are exchanged, the reported X axis corresponds to
    // the panel's Y axis, so the bounds must be swapped as well.
    //
    if (pDevice->XYExchange) {
        t = x; x = y; y = t;
        xMin = pDevice->YMin; xMax = pDevice->YMax;
        yMin = pDevice->XMin; yMax = pDevice->XMax;
    }
    else {
        xMin = pDevice->XMin; xMax = pDevice->XMax;
        yMin = pDevice->YMin; yMax = pDevice->YMax;
    }

    if (x < xMin) x = xMin;
    if (x > xMax) x = xMax;
    if (y < yMin) y = yMin;
    if (y > yMax) y = yMax;

    if (pDevice->XRevert) x = xMax + xMin - x;
    if (pDevice->YRevert) y = yMax + yMin - y;

    *pX = (int)x;
    *pY = (int)y;
}


typedef struct
{
    BYTE  reportId;                                 // Report ID = 0x54 (84) 'T'
                                                       // Collection: TouchScreen
    BYTE  DIG_TouchScreenContactCountMaximum;       // Usage 0x000D0055: Contact Count Maximum, Value = 0 to 8
} featureReport54_t;

static BOOLEAN
TouchReportIsActive(
    _In_ const inputReport54_t* Report
);

static VOID
CacheTouchReportLocked(
    _In_ PMANUAL_QUEUE_CONTEXT QueueContext,
    _In_ const inputReport54_t* Report
);

static VOID
CompleteTouchReport(
    _In_ PMANUAL_QUEUE_CONTEXT QueueContext,
    _In_ WDFREQUEST Request,
    _In_ const inputReport54_t* Report
);

static VOID
ClearTouchReports(
    _In_ PDEVICE_CONTEXT DeviceContext
);

//
// This is the default report descriptor for the virtual Hid device returned
// by the mini driver in response to IOCTL_HID_GET_REPORT_DESCRIPTOR.
//
/*HID_REPORT_DESCRIPTOR       G_DefaultReportDescriptor[] = {
    0x06,0x00, 0xFF,                // USAGE_PAGE (Vender Defined Usage Page)
    0x09,0x01,                      // USAGE (Vendor Usage 0x01)
    0xA1,0x01,                      // COLLECTION (Application)
    0x85,CONTROL_FEATURE_REPORT_ID,    // REPORT_ID (1)
    0x09,0x01,                         // USAGE (Vendor Usage 0x01)
    0x15,0x00,                         // LOGICAL_MINIMUM(0)
    0x26,0xff, 0x00,                   // LOGICAL_MAXIMUM(255)
    0x75,0x08,                         // REPORT_SIZE (0x08)
    0x96,(FEATURE_REPORT_SIZE_CB & 0xff), (FEATURE_REPORT_SIZE_CB >> 8), // REPORT_COUNT
    0xB1,0x00,                         // FEATURE (Data,Ary,Abs)
    0x09,0x01,                         // USAGE (Vendor Usage 0x01)
    0x75,0x08,                         // REPORT_SIZE (0x08)
    0x96,(INPUT_REPORT_SIZE_CB & 0xff), (INPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
    0x81,0x00,                         // INPUT (Data,Ary,Abs)
    0x09,0x01,                         // USAGE (Vendor Usage 0x01)
    0x75,0x08,                         // REPORT_SIZE (0x08)
    0x96,(OUTPUT_REPORT_SIZE_CB & 0xff), (OUTPUT_REPORT_SIZE_CB >> 8), // REPORT_COUNT
    0x91,0x00,                         // OUTPUT (Data,Ary,Abs)
    0xC0,                           // END_COLLECTION
};*/

const HID_REPORT_DESCRIPTOR G_DefaultReportDescriptor[] = {
    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x04,     //   (LOCAL)USAGE              0x000D0004 Touch Screen(Application Collection)
    0xA1, 0x01,     //   (MAIN)COLLECTION         0x01 Application(Usage = 0x000D0004: Page = Digitizer Device Page, Usage = Touch Screen, Type = Application Collection)
    0x85, 0x54,     //     (GLOBAL)REPORT_ID          0x54 (84) 'T'

    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x14,           //    (GLOBAL)LOGICAL_MINIMUM(0)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //46 47
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //55 56
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical
    
    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical
    
    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x22,     //     (LOCAL)USAGE              0x000D0022 Finger(Logical Collection)
    0xA1, 0x02,     //     (MAIN)COLLECTION         0x02 Logical(Usage = 0x000D0022: Page = Digitizer Device Page, Usage = Finger, Type = Logical Collection)
    0x09, 0x42,     //       (LOCAL)USAGE              0x000D0042 Tip Switch(Momentary Control)
    0x25, 0x01,     //       (GLOBAL)LOGICAL_MAXIMUM    0x01 (1)
    0x75, 0x01,     //       (GLOBAL)REPORT_SIZE        0x01 (1) Number of bits per field
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x32,     //       (LOCAL)USAGE              0x000D0032 In Range(Momentary Control)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x47,     //       (LOCAL)USAGE              0x000D0047 Confidence(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 1 bit) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x95, 0x05,     //       (GLOBAL)REPORT_COUNT       0x05 (5) Number of fields
    0x81, 0x03,     //       (MAIN)INPUT              0x00000003 (5 fields x 1 bit) 1 = Constant 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x75, 0x08,     //       (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x95, 0x01,     //       (GLOBAL)REPORT_COUNT       0x01 (1) Number of fields
    0x09, 0x51,     //       (LOCAL)USAGE              0x000D0051 Contact Identifier(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x05, 0x01,     //       (GLOBAL)USAGE_PAGE         0x0001 Generic Desktop Page
    0x26, 0x38, 0x04,   // (GLOBAL) LOGICAL_MAXIMUM    0x0438 (1080)    //99 100
    0x75, 0x10,     //       (GLOBAL)REPORT_SIZE        0x10 (16) Number of bits per field
    0x09, 0x30,     //       (LOCAL)USAGE              0x00010030 X(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x26, 0x24, 0x09,   // (GLOBAL) LOGICAL_MAXIMUM    0x0924 (2340)    //108 109
    0x09, 0x31,     //       (LOCAL)USAGE              0x00010031 Y(Dynamic Value)
    0x81, 0x02,     //       (MAIN)INPUT              0x00000002 (1 field x 16 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Logical

    0x05, 0x0D,     // (GLOBAL) USAGE_PAGE         0x000D Digitizer Device Page
    0x09, 0x54,     //     (LOCAL)USAGE              0x000D0054 Contact Count(Dynamic Value)
    0x75, 0x08,     //     (GLOBAL)REPORT_SIZE        0x08 (8) Number of bits per field
    0x25, 0x0A,     //     (GLOBAL)LOGICAL_MAXIMUM    0x0A (10)
    0x81, 0x02,     //     (MAIN)INPUT              0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0x09, 0x55,     //     (LOCAL)USAGE              0x000D0055 Contact Count Maximum(Static Value)
    0xB1, 0x02,     //     (MAIN)FEATURE            0x00000002 (1 field x 8 bits) 0 = Data 1 = Variable 0 = Absolute 0 = NoWrap 0 = Linear 0 = PrefState 0 = NoNull 0 = NonVolatile 0 = Bitmap
    0xC0,           // (MAIN)   END_COLLECTION     Application

};

C_ASSERT(sizeof(G_DefaultReportDescriptor) == TOUCH_REPORT_DESCRIPTOR_SIZE);

static const featureReport54_t features = {0x54,10};
//
// This is the default HID descriptor returned by the mini driver
// in response to IOCTL_HID_GET_DEVICE_DESCRIPTOR. The size
// of report descriptor is currently the size of G_DefaultReportDescriptor.
//

HID_DESCRIPTOR              G_DefaultHidDescriptor = {
    0x09,   // length of HID descriptor
    0x21,   // descriptor type == HID  0x21
    0x0100, // hid spec release
    0x00,   // country code == Not Specified
    0x01,   // number of HID class descriptors
    {                                       //DescriptorList[0]
        0x22,                               //report descriptor type 0x22
        sizeof(G_DefaultReportDescriptor)   //total length of report descriptor
    }
};

NTSTATUS
DriverEntry(
    _In_  PDRIVER_OBJECT    DriverObject,
    _In_  PUNICODE_STRING   RegistryPath
    )
/*++

Routine Description:
    DriverEntry initializes the driver and is the first routine called by the
    system after the driver is loaded. DriverEntry specifies the other entry
    points in the function driver, such as EvtDevice and DriverUnload.

Parameters Description:

    DriverObject - represents the instance of the function driver that is loaded
    into memory. DriverEntry must initialize members of DriverObject before it
    returns to the caller. DriverObject is allocated by the system before the
    driver is loaded, and it is released by the system after the system unloads
    the function driver from memory.

    RegistryPath - represents the driver specific path in the Registry.
    The function driver can use the path to store driver related data between
    reboots. The path does not store hardware instance specific data.

Return Value:

    STATUS_SUCCESS, or another status value for which NT_SUCCESS(status) equals
                    TRUE if successful,

    STATUS_UNSUCCESSFUL, or another status for which NT_SUCCESS(status) equals
                    FALSE otherwise.

--*/
{
    WDF_DRIVER_CONFIG       config;
    WDF_OBJECT_ATTRIBUTES driverAttributes;
    NTSTATUS                status;
#ifdef DBG
    WPP_INIT_TRACING(DriverObject, RegistryPath);
#endif
#ifdef _KERNEL_MODE
    //
    // Opt-in to using non-executable pool memory on Windows 8 and later.
    // https://msdn.microsoft.com/en-us/library/windows/hardware/hh920402(v=vs.85).aspx
    //
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
#endif

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    WDF_OBJECT_ATTRIBUTES_INIT(&driverAttributes);
    driverAttributes.EvtCleanupCallback = EvtDriverCleanup;

    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            &driverAttributes,
                            &config,
                            WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {

        goto Exit;
    }
Exit:
    return status;
}
VOID
EvtDriverCleanup(
    _In_ WDFOBJECT Object
)
{
#ifdef DBG
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)Object));
#else
    UNREFERENCED_PARAMETER(Object);
#endif
}

NTSTATUS
EvtDeviceAdd(
    _In_  WDFDRIVER         Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
/*++
Routine Description:

    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent a new instance of the device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    WDFDEVICE               device;
    PDEVICE_CONTEXT         deviceContext;
    PHID_DEVICE_ATTRIBUTES  hidAttributes;
    UNREFERENCED_PARAMETER  (Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

    pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;


    //
    // Mark ourselves as a filter, which also relinquishes power policy ownership
    //
    WdfFdoInitSetFilter(DeviceInit);

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                            &deviceAttributes,
                            DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit,
                            &deviceAttributes,
                            &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceContext = GetDeviceContext(device);
    deviceContext->Device       = device;
    deviceContext->OnClose = TRUE;
    deviceContext->Interrupt = WDF_NO_HANDLE;
    deviceContext->SpbController = WDF_NO_HANDLE;
    deviceContext->LastActiveReportValid = FALSE;
    deviceContext->ActiveCount = 0;
    for (ULONG i = 0; i < SPB_MAX_INFLIGHT; i++) {
        KeInitializeEvent(&deviceContext->SpbSlots[i].Event, NotificationEvent, FALSE);
        deviceContext->SpbSlots[i].InUse = FALSE;
    }

    //
    // Default coordinate configuration; the device registry key may
    // override these in ReadCoordinateConfigFromRegistry.
    //
    deviceContext->XRevert = 0;
    deviceContext->YRevert = 0;
    deviceContext->XYExchange = 0;
    deviceContext->XMin = 0;
    deviceContext->XMax = 1080;
    deviceContext->YMin = 0;
    deviceContext->YMax = 2340;

    hidAttributes = &deviceContext->HidDeviceAttributes;
    RtlZeroMemory(hidAttributes, sizeof(HID_DEVICE_ATTRIBUTES));
    hidAttributes->Size         = sizeof(HID_DEVICE_ATTRIBUTES);
    hidAttributes->VendorID     = HIDMINI_VID;
    hidAttributes->ProductID    = HIDMINI_PID;
    hidAttributes->VersionNumber = HIDMINI_VERSION;

    status = QueueCreate(device,
                         &deviceContext->DefaultQueue);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    status = ManualQueueCreate(device,
                               &deviceContext->ManualQueue);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    //
    // Use default "HID Descriptor" (hardcoded). We will set the
    // wReportLength memeber of HID descriptor when we read the
    // the report descriptor either from registry or the hard-coded
    // one.
    //
    deviceContext->HidDescriptor = G_DefaultHidDescriptor;

    //
    // Read the coordinate configuration (XRevert/YRevert/XYExchange and
    // the panel bounds) from the device registry key.
    //
    status = ReadCoordinateConfigFromRegistry(device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("GTX9886: coordinate config read failed 0x%08X, using defaults\n",
            status));
    }

    //
    // Copy the template descriptor into per-instance storage and patch the
    // X/Y logical maximum of every finger collection. The first finger
    // collection stores its X max value bytes at offset 46/47 and Y max at
    // 55/56; each subsequent collection starts 53 bytes later. When the
    // axes are exchanged, the maxima are swapped so the descriptor matches
    // the transformed report data.
    //
    RtlCopyMemory(
        deviceContext->ReportDescriptorStorage,
        G_DefaultReportDescriptor,
        sizeof(deviceContext->ReportDescriptorStorage));

    ULONG descXMax = deviceContext->XYExchange ? deviceContext->YMax : deviceContext->XMax;
    ULONG descYMax = deviceContext->XYExchange ? deviceContext->XMax : deviceContext->YMax;

    for (ULONG i = 0; i < 10; i++) {
        deviceContext->ReportDescriptorStorage[46 + 53 * i] = (BYTE)(descXMax & 0xFF);
        deviceContext->ReportDescriptorStorage[47 + 53 * i] = (BYTE)((descXMax >> 8) & 0x0F);
        deviceContext->ReportDescriptorStorage[55 + 53 * i] = (BYTE)(descYMax & 0xFF);
        deviceContext->ReportDescriptorStorage[56 + 53 * i] = (BYTE)((descYMax >> 8) & 0x0F);
    }

    deviceContext->ReportDescriptor = deviceContext->ReportDescriptorStorage;
    status = STATUS_SUCCESS;

    return status;
}

NTSTATUS
    OnPrepareHardware(
        _In_  WDFDEVICE     FxDevice,
        _In_  WDFCMRESLIST  FxResourcesRaw,
        _In_  WDFCMRESLIST  FxResourcesTranslated
    )
    /*++

        Routine Description:

        This routine caches the SPB resource connection ID.

        Arguments:

        FxDevice - a handle to the framework device object
        FxResourcesRaw - list of translated hardware resources that
            the PnP manager has assigned to the device
        FxResourcesTranslated - list of raw hardware resources that
            the PnP manager has assigned to the device

        Return Value:

        Status

    --*/
{
    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    BOOLEAN fSpbResourceFound = FALSE;
    BOOLEAN fInterruptResourceFound = FALSE;
    ULONG interruptIndex = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(FxResourcesRaw);

    //
    // Parse the peripheral's resources.
    //

    ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

    for (ULONG i = 0; i < resourceCount; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
        UCHAR Class;
        UCHAR Type;

        pDescriptor = WdfCmResourceListGetDescriptor(
            FxResourcesTranslated, i);

        switch (pDescriptor->Type)
        {
        case CmResourceTypeConnection:

            //
            // Look for I2C or SPI resource and save connection ID.
            //

            Class = pDescriptor->u.Connection.Class;
            Type = pDescriptor->u.Connection.Type;

            if ((Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL) &&
                ((Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)))
            {
                if (fSpbResourceFound == FALSE)
                {
                    pDevice->PeripheralId.LowPart =
                        pDescriptor->u.Connection.IdLowPart;
                    pDevice->PeripheralId.HighPart =
                        pDescriptor->u.Connection.IdHighPart;

                    fSpbResourceFound = TRUE;
                }
            }

            break;

        case CmResourceTypeInterrupt:

            if (fInterruptResourceFound == FALSE)
            {
                fInterruptResourceFound = TRUE;
                interruptIndex = i;
            }
            break;

        default:

            //
            // Ignoring all other resource types.
            //

            break;
        }
    }

    //
    // An SPB resource is required.
    //

    if (fSpbResourceFound == FALSE)
    {
        status = STATUS_NOT_FOUND;
    }

    if (fInterruptResourceFound == FALSE)
    {
        //
        // Not fatal (unlike a missing SPB resource), but the touchscreen
        // will be silent: report it so the misconfiguration is obvious.
        //
        KdPrint(("GTX9886: no interrupt resource found, touch input will be silent\n"));
    }

    //
    // Create the interrupt if an interrupt
    // resource was found.
    //

    if (NT_SUCCESS(status))
    {
        if (fInterruptResourceFound == TRUE)
        {
            WDF_INTERRUPT_CONFIG interruptConfig;
            
            WDF_INTERRUPT_CONFIG_INIT(
                &interruptConfig,
                OnInterruptIsr,
                NULL);
            interruptConfig.ReportInactiveOnPowerDown = TRUE;
            interruptConfig.PassiveHandling = TRUE;
            interruptConfig.InterruptTranslated = WdfCmResourceListGetDescriptor(
                FxResourcesTranslated,
                interruptIndex);
            interruptConfig.InterruptRaw = WdfCmResourceListGetDescriptor(
                FxResourcesRaw,
                interruptIndex);

            status = WdfInterruptCreate(
                pDevice->Device,
                &interruptConfig,
                WDF_NO_OBJECT_ATTRIBUTES,
                &pDevice->Interrupt);

            if (!NT_SUCCESS(status))
            {
                KdPrint(("GTX9886: WdfInterruptCreate failed 0x%08X\n", status));
            }

            if (NT_SUCCESS(status))
            {
                WdfInterruptDisable(pDevice->Interrupt);
            }
        }
    }

    return status;
}

NTSTATUS
    OnReleaseHardware(
        _In_  WDFDEVICE     FxDevice,
        _In_  WDFCMRESLIST  FxResourcesTranslated
    )
    /*++

        Routine Description:

        Arguments:

        FxDevice - a handle to the framework device object
        FxResourcesTranslated - list of raw hardware resources that
            the PnP manager has assigned to the device

        Return Value:

        Status

    --*/
{
    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    UNREFERENCED_PARAMETER(FxResourcesTranslated);
    if (pDevice->Interrupt != NULL)
    {
        WdfObjectDelete(pDevice->Interrupt);
        pDevice->Interrupt = WDF_NO_HANDLE;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
OnD0Entry(
    _In_  WDFDEVICE               FxDevice,
    _In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

    Routine Description:

    This routine allocates objects needed by the driver.

    Arguments:

    FxDevice - a handle to the framework device object
    FxPreviousState - previous power state

    Return Value:

    Status

--*/
{
    UNREFERENCED_PARAMETER(FxPreviousState);

    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    NTSTATUS status;

    //
    // Drop anything left over from the previous D0 residency.
    //
    ClearTouchReports(pDevice);

    //
    // Create the SPB target.
    //

    WDF_OBJECT_ATTRIBUTES targetAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&targetAttributes);

    status = WdfIoTargetCreate(
        pDevice->Device,
        &targetAttributes,
        &pDevice->SpbController);

    if (NT_SUCCESS(status)) {
        status = SpbDeviceOpen(pDevice);
    }

    if (!NT_SUCCESS(status)) {
        //
        // Never fail D0 entry: a filter that returns an error from
        // EvtDeviceD0Entry breaks the whole device stack (the device
        // shows up as Code 10 / fails to start). Instead keep OnClose
        // = TRUE and the interrupt disabled; the device simply stays
        // silent with no input.
        //
        KdPrint(("GTX9886: OnD0Entry setup failed 0x%08X, staying silent\n",
            status));
        if (pDevice->SpbController != WDF_NO_HANDLE) {
            WdfObjectDelete(pDevice->SpbController);
            pDevice->SpbController = WDF_NO_HANDLE;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
OnD0Exit(
    _In_  WDFDEVICE               FxDevice,
    _In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

    Routine Description:

    This routine destroys objects needed by the driver.

    Arguments:

    FxDevice - a handle to the framework device object
    FxPreviousState - previous power state

    Return Value:

    Status

--*/
{
    NTSTATUS status;
    LARGE_INTEGER timeout;
    UNREFERENCED_PARAMETER(FxPreviousState);

    PDEVICE_CONTEXT pDevice = GetDeviceContext(FxDevice);
    PMANUAL_QUEUE_CONTEXT queueContext =
        GetManualQueueContext(pDevice->ManualQueue);

    SpbDeviceClose(pDevice);
    //
    // Wait until any in-flight delivery loop has exited before deleting
    // objects it may still be using. Bounded wait: a delivery loop never
    // blocks, so a timeout here indicates a bug; continue rather than
    // hang the PnP stack forever.
    //
    timeout.QuadPart = -5 * 1000 * 1000 * 10;   // 5 seconds

    status = KeWaitForSingleObject(
        &queueContext->DeliveryIdleEvent,
        Executive,
        KernelMode,
        FALSE,
        &timeout);
    if (status == STATUS_TIMEOUT) {
        //
        // The delivery loop is wedged. Ask the framework to fail the
        // device instead of tearing down objects a live loop may still
        // be using; the framework completes pending requests and walks
        // the normal removal path, which reclaims the hung state safely.
        //
        KdPrint(("GTX9886: delivery loop did not drain within 5s\n"));
        WdfDeviceSetFailed(pDevice->Device, WdfDeviceFailedNoRestart);
        return STATUS_SUCCESS;
    }
    ClearTouchReports(pDevice);
    if (pDevice->SpbController != WDF_NO_HANDLE)
    {
        WdfObjectDelete(pDevice->SpbController);
        pDevice->SpbController = WDF_NO_HANDLE;
    }

    return STATUS_SUCCESS;
}


#ifdef _KERNEL_MODE
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoDeviceControl;
#else
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL          EvtIoDeviceControl;
#endif

NTSTATUS
QueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    )
/*++
Routine Description:

    This function creates a default, parallel I/O queue to proces IOCTLs
    from hidclass.sys.

Arguments:

    Device - Handle to a framework device object.

    Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDF_OBJECT_ATTRIBUTES   queueAttributes;
    WDFQUEUE                queue;
    PQUEUE_CONTEXT          queueContext;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
                            &queueConfig,
                            WdfIoQueueDispatchParallel);

#ifdef _KERNEL_MODE
    queueConfig.EvtIoInternalDeviceControl  = EvtIoDeviceControl;
#else
    //
    // HIDclass uses INTERNAL_IOCTL which is not supported by UMDF. Therefore
    // the hidumdf.sys changes the IOCTL type to DEVICE_CONTROL for next stack
    // and sends it down
    //
    queueConfig.EvtIoDeviceControl          = EvtIoDeviceControl;
#endif

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                            &queueAttributes,
                            QUEUE_CONTEXT);

    status = WdfIoQueueCreate(
                            Device,
                            &queueConfig,
                            &queueAttributes,
                            &queue);

    if( !NT_SUCCESS(status) ) {
        return status;
    }

    queueContext = GetQueueContext(queue);
    queueContext->Queue         = queue;
    queueContext->DeviceContext = GetDeviceContext(Device);

    *Queue = queue;
    
    return status;
}

VOID
EvtIoDeviceControl(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            OutputBufferLength,
    _In_  size_t            InputBufferLength,
    _In_  ULONG             IoControlCode
    )
/*++
Routine Description:

    This event callback function is called when the driver receives an

    (KMDF) IOCTL_HID_Xxx code when handlng IRP_MJ_INTERNAL_DEVICE_CONTROL
    (UMDF) IOCTL_HID_Xxx, IOCTL_UMDF_HID_Xxx when handling IRP_MJ_DEVICE_CONTROL

Arguments:

    Queue - A handle to the queue object that is associated with the I/O request

    Request - A handle to a framework request object.

    OutputBufferLength - The length, in bytes, of the request's output buffer,
            if an output buffer is available.

    InputBufferLength - The length, in bytes, of the request's input buffer, if
            an input buffer is available.

    IoControlCode - The driver or system defined IOCTL associated with the request

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    BOOLEAN                 completeRequest = TRUE;
    WDFDEVICE               device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT         deviceContext = NULL;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    UNREFERENCED_PARAMETER  (OutputBufferLength);
    UNREFERENCED_PARAMETER  (InputBufferLength);

    deviceContext = GetDeviceContext(device);

    switch (IoControlCode)
    {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:   // METHOD_NEITHER
        //
        // Retrieves the device's HID descriptor.
        //
        _Analysis_assume_(deviceContext->HidDescriptor.bLength != 0);
        status = RequestCopyFromBuffer(Request,
                            &deviceContext->HidDescriptor,
                            deviceContext->HidDescriptor.bLength);
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:   // METHOD_NEITHER
        //
        //Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
        //
        status = RequestCopyFromBuffer(Request,
                            &queueContext->DeviceContext->HidDeviceAttributes,
                            sizeof(HID_DEVICE_ATTRIBUTES));
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:   // METHOD_NEITHER
        //
        //Obtains the report descriptor for the HID device.
        //
        status = RequestCopyFromBuffer(Request,
                            deviceContext->ReportDescriptor,
                            deviceContext->HidDescriptor.DescriptorList[0].wReportLength);
        break;

    case IOCTL_HID_READ_REPORT:             // METHOD_NEITHER
        //
        // Returns a report from the device into a class driver-supplied
        // buffer.
        //
        status = ReadReport(queueContext, Request, &completeRequest);
        break;

    case IOCTL_HID_WRITE_REPORT:            // METHOD_NEITHER
        //
        // Transmits a class driver-supplied report to the device.
        //
        status = WriteReport(queueContext, Request);
        break;

#ifdef _KERNEL_MODE

    case IOCTL_HID_GET_FEATURE:             // METHOD_OUT_DIRECT

        status = GetFeature(queueContext, Request);
        break;

    case IOCTL_HID_SET_FEATURE:             // METHOD_IN_DIRECT

        status = SetFeature(queueContext, Request);
        break;

    case IOCTL_HID_GET_INPUT_REPORT:        // METHOD_OUT_DIRECT

        status = GetInputReport(queueContext, Request);
        break;

    case IOCTL_HID_SET_OUTPUT_REPORT:       // METHOD_IN_DIRECT

        status = SetOutputReport(queueContext, Request);
        break;

#else // UMDF specific

    //
    // HID minidriver IOCTL uses HID_XFER_PACKET which contains an embedded pointer.
    //
    //   typedef struct _HID_XFER_PACKET {
    //     PUCHAR reportBuffer;
    //     ULONG  reportBufferLen;
    //     UCHAR  reportId;
    //   } HID_XFER_PACKET, *PHID_XFER_PACKET;
    //
    // UMDF cannot handle embedded pointers when marshalling buffers between processes.
    // Therefore a special driver mshidumdf.sys is introduced to convert such IRPs to
    // new IRPs (with new IOCTL name like IOCTL_UMDF_HID_Xxxx) where:
    //
    //   reportBuffer - passed as one buffer inside the IRP
    //   reportId     - passed as a second buffer inside the IRP
    //
    // The new IRP is then passed to UMDF host and driver for further processing.
    //

    case IOCTL_UMDF_HID_GET_FEATURE:        // METHOD_NEITHER

        status = GetFeature(queueContext, Request);
        break;

    case IOCTL_UMDF_HID_SET_FEATURE:        // METHOD_NEITHER

        status = SetFeature(queueContext, Request);
        break;

    case IOCTL_UMDF_HID_GET_INPUT_REPORT:  // METHOD_NEITHER

        status = GetInputReport(queueContext, Request);
        break;

    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT: // METHOD_NEITHER

        status = SetOutputReport(queueContext, Request);
        break;

#endif // _KERNEL_MODE

    case IOCTL_HID_GET_STRING:                      // METHOD_NEITHER

        status = GetString(Request);
        break;

    case IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT

        status = GetIndexedString(Request);
        break;

    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:  // METHOD_NEITHER
        //
        // This has the USBSS Idle notification callback. If the lower driver
        // can handle it (e.g. USB stack can handle it) then pass it down
        // otherwise complete it here as not inplemented. For a virtual
        // device, idling is not needed.
        //
        // Not implemented. fall through...
        //
    case IOCTL_HID_ACTIVATE_DEVICE:                 // METHOD_NEITHER
    case IOCTL_HID_DEACTIVATE_DEVICE:               // METHOD_NEITHER
    case IOCTL_GET_PHYSICAL_DESCRIPTOR:             // METHOD_OUT_DIRECT
        //
        // We don't do anything for these IOCTLs but some minidrivers might.
        //
        // Not implemented. fall through...
        //
    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    //
    // Complete the request. Information value has already been set by request
    // handlers.
    //
    if (completeRequest) {
        WdfRequestComplete(Request, status);
    }
}

NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _When_(NumBytesToCopyFrom == 0, __drv_reportError(NumBytesToCopyFrom cannot be zero))
    _In_  size_t            NumBytesToCopyFrom
    )
/*++

Routine Description:

    A helper function to copy specified bytes to the request's output memory

Arguments:

    Request - A handle to a framework request object.

    SourceBuffer - The buffer to copy data from.

    NumBytesToCopyFrom - The length, in bytes, of data to be copied.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDFMEMORY               memory;
    size_t                  outputBufferLength;

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    WdfMemoryGetBuffer(memory, &outputBufferLength);
    if (outputBufferLength < NumBytesToCopyFrom) {
        status = STATUS_INVALID_BUFFER_SIZE;
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory,
                                    0,
                                    SourceBuffer,
                                    NumBytesToCopyFrom);
    if( !NT_SUCCESS(status) ) {
        
        return status;
    }
    WdfRequestSetInformation(Request, NumBytesToCopyFrom);
    return status;
}

static BOOLEAN
TouchReportIsActive(
    _In_ const inputReport54_t* Report
)
{
    BYTE contactCount = Report->DIG_TouchScreenContactCount;

    if ((contactCount == 0) || (contactCount > MAX_POINT_NUM)) {
        return FALSE;
    }

    //
    // A frame counts as "active" (fingers down) when any reported contact
    // has its tip switch set. A frame with only tip-up slots is a lift
    // edge. Partial-lift frames (down contacts plus explicit tip-up slots)
    // therefore stay "active".
    //
    for (BYTE index = 0; index < contactCount; index++) {
        if ((Report->points[index * 6] & 0x01) != 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOLEAN
TouchReportHasLifted(
    _In_ const inputReport54_t* Report
)
{
    BYTE contactCount = Report->DIG_TouchScreenContactCount;

    if ((contactCount == 0) || (contactCount > MAX_POINT_NUM)) {
        return FALSE;
    }

    //
    // True when the frame carries at least one explicit tip-up slot.
    // Such frames must never be coalesced away, or the UP signal for the
    // lifted contact would be lost and the HID layer would fall back to
    // its slower contact-disappearance detection.
    //
    for (BYTE index = 0; index < contactCount; index++) {
        if ((Report->points[index * 6] & 0x01) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static VOID
CacheTouchReportLocked(
    _In_ PMANUAL_QUEUE_CONTEXT QueueContext,
    _In_ const inputReport54_t* Report
)
/*++
  Cache a report into the ring buffer. Caller holds ReportLock.

  Invariants:
    - Frames carrying tip-up slots (partial or full lifts) are never
      dropped: they may only be overwritten by a newer lift when every
      slot already holds a lift frame (practically unreachable).
    - Consecutive frames in the same state are coalesced only when
      neither frame carries tip-up slots.
    - Pure down frames are droppable: when the ring is full the newest
      one is discarded, because the next frame carries fresh coordinates
      and the pending lift frame is guaranteed to be delivered first.
--*/
{
    BOOLEAN reportActive = TouchReportIsActive(Report);
    BOOLEAN reportHasLifted = TouchReportHasLifted(Report);
    UCHAR reportIndex;

    if (QueueContext->PendingCount != 0) {
        reportIndex = (QueueContext->PendingHead + QueueContext->PendingCount - 1) %
            TOUCH_REPORT_SLOT_COUNT;

        if ((TouchReportIsActive(&QueueContext->PendingReports[reportIndex]) == reportActive) &&
            !TouchReportHasLifted(&QueueContext->PendingReports[reportIndex]) &&
            !reportHasLifted) {
            //
            // Same state and no tip-up slots on either side: coalesce,
            // keep only the newest frame. Never coalesce when either frame
            // carries tip-up slots, or a lifted contact's UP would vanish.
            //
            RtlCopyMemory(&QueueContext->PendingReports[reportIndex], Report, sizeof(*Report));
            return;
        }
    }

    if (QueueContext->PendingCount < TOUCH_REPORT_SLOT_COUNT) {
        //
        // State transition (or first report): append.
        //
        reportIndex = (QueueContext->PendingHead + QueueContext->PendingCount) %
            TOUCH_REPORT_SLOT_COUNT;
        RtlCopyMemory(&QueueContext->PendingReports[reportIndex], Report, sizeof(*Report));
        QueueContext->PendingCount++;
        return;
    }

    //
    // Ring is full.
    //
    if (!reportHasLifted) {
        //
        // Pure down frame: drop it. Fresh coordinates arrive in the next
        // frame, and the lift frame ahead of it stays queued so it is
        // delivered first.
        //
        return;
    }

    //
    // Lift frame: never drop it. Overwrite the most recent pure down slot,
    // scanning from the tail towards (but never touching) the head. If the
    // tail and middle slots are both lift frames, the head is the last
    // candidate: sacrifice it when it is a pure down frame, and only fall
    // back to overwriting the tail when every slot holds a lift frame
    // (practically unreachable).
    //
    for (UCHAR back = 0; back < TOUCH_REPORT_SLOT_COUNT - 1; back++) {
        reportIndex = (QueueContext->PendingHead + TOUCH_REPORT_SLOT_COUNT - 1 - back) %
            TOUCH_REPORT_SLOT_COUNT;
        if (!TouchReportHasLifted(&QueueContext->PendingReports[reportIndex])) {
            RtlCopyMemory(&QueueContext->PendingReports[reportIndex], Report, sizeof(*Report));
            return;
        }
    }
    if (!TouchReportHasLifted(&QueueContext->PendingReports[QueueContext->PendingHead])) {
        //
        // The head is a pure down frame and therefore droppable: replace it
        // with the new lift instead of losing another lift frame.
        //
        RtlCopyMemory(&QueueContext->PendingReports[QueueContext->PendingHead], Report, sizeof(*Report));
        return;
    }
    reportIndex = (QueueContext->PendingHead + TOUCH_REPORT_SLOT_COUNT - 1) %
        TOUCH_REPORT_SLOT_COUNT;
    RtlCopyMemory(&QueueContext->PendingReports[reportIndex], Report, sizeof(*Report));
}

static VOID
CompleteTouchReport(
    _In_ PMANUAL_QUEUE_CONTEXT QueueContext,
    _In_ WDFREQUEST Request,
    _In_ const inputReport54_t* Report
)
/*++
  Delivery loop: complete the request with Report, then keep pairing queued
  read requests with cached reports until either side runs dry or the device
  is closing. Exactly one loop runs at a time (DeliveryInProgress guards it,
  set by ReadReport or the ISR before calling this).
--*/
{
    inputReport54_t deliveryReport;
    NTSTATUS status;

    RtlCopyMemory(&deliveryReport, Report, sizeof(deliveryReport));

    for (;;) {
        status = RequestCopyFromBuffer(Request, &deliveryReport, sizeof(deliveryReport));
        WdfRequestComplete(Request, status);

        WdfSpinLockAcquire(QueueContext->ReportLock);
        if (QueueContext->DeviceContext->OnClose ||
            (QueueContext->PendingCount == 0)) {
            if (QueueContext->DeviceContext->OnClose) {
                QueueContext->PendingHead = 0;
                QueueContext->PendingCount = 0;
            }
            QueueContext->DeliveryInProgress = FALSE;
            KeSetEvent(&QueueContext->DeliveryIdleEvent, IO_NO_INCREMENT, FALSE);
            WdfSpinLockRelease(QueueContext->ReportLock);
            return;
        }

        status = WdfIoQueueRetrieveNextRequest(QueueContext->Queue, &Request);
        if (!NT_SUCCESS(status)) {
            //
            // No read request waiting; the next ISR/ReadReport starts a new
            // delivery loop.
            //
            QueueContext->DeliveryInProgress = FALSE;
            KeSetEvent(&QueueContext->DeliveryIdleEvent, IO_NO_INCREMENT, FALSE);
            WdfSpinLockRelease(QueueContext->ReportLock);
            return;
        }

        RtlCopyMemory(
            &deliveryReport,
            &QueueContext->PendingReports[QueueContext->PendingHead],
            sizeof(deliveryReport));
        QueueContext->PendingHead =
            (QueueContext->PendingHead + 1) % TOUCH_REPORT_SLOT_COUNT;
        QueueContext->PendingCount--;
        WdfSpinLockRelease(QueueContext->ReportLock);
    }
}

static VOID
ClearTouchReports(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    PMANUAL_QUEUE_CONTEXT queueContext = GetManualQueueContext(DeviceContext->ManualQueue);

    WdfSpinLockAcquire(queueContext->ReportLock);
    queueContext->PendingHead = 0;
    queueContext->PendingCount = 0;
    queueContext->DeliveryInProgress = FALSE;
    //
    // Reset the touch snapshot under the same lock: GetInputReport reads
    // it while holding ReportLock, so the clear must not race with it.
    //
    DeviceContext->LastActiveReportValid = FALSE;
    DeviceContext->ActiveCount = 0;
    KeSetEvent(&queueContext->DeliveryIdleEvent, IO_NO_INCREMENT, FALSE);
    WdfSpinLockRelease(queueContext->ReportLock);
}

NTSTATUS
ReadReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request,
    _Always_(_Out_)
          BOOLEAN*          CompleteRequest
    )
/*++

Routine Description:

    Handles IOCTL_HID_READ_REPORT for the HID collection. Normally the request
    will be forwarded to a manual queue for further process. In that case, the
    caller should not try to complete the request at this time, as the request
    will later be retrieved back from the manually queue and completed there.
    However, if for some reason the forwarding fails, the caller still need
    to complete the request with proper error code immediately.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to  Request Packet.

    CompleteRequest - A boolean output value, indicating whether the caller
            should complete the request or not

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    PMANUAL_QUEUE_CONTEXT   queueContext;
    inputReport54_t         pendingReport;
    BOOLEAN                 reportPending = FALSE;

    queueContext = GetManualQueueContext(QueueContext->DeviceContext->ManualQueue);
    WdfSpinLockAcquire(queueContext->ReportLock);

    if (QueueContext->DeviceContext->OnClose) {
        status = STATUS_DEVICE_NOT_READY;
    }
    else if (queueContext->PendingCount != 0) {
        if (!queueContext->DeliveryInProgress) {
            //
            // A report is cached and no delivery loop is running: take over
            // and complete this request right away.
            //
            RtlCopyMemory(
                &pendingReport,
                &queueContext->PendingReports[queueContext->PendingHead],
                sizeof(pendingReport));
            queueContext->PendingHead =
                (queueContext->PendingHead + 1) % TOUCH_REPORT_SLOT_COUNT;
            queueContext->PendingCount--;
            queueContext->DeliveryInProgress = TRUE;
            KeClearEvent(&queueContext->DeliveryIdleEvent);
            reportPending = TRUE;
            status = STATUS_SUCCESS;
        }
        else {
            //
            // A delivery loop is already running; it will pick this request up.
            //
            status = WdfRequestForwardToIoQueue(Request, queueContext->Queue);
        }
    }
    else {
        //
        // No report yet; park the request and let the ISR complete it later.
        //
        status = WdfRequestForwardToIoQueue(Request, queueContext->Queue);
    }

    WdfSpinLockRelease(queueContext->ReportLock);

    if (reportPending) {
        CompleteTouchReport(queueContext, Request, &pendingReport);
        *CompleteRequest = FALSE;
    }
    else {
        *CompleteRequest = !NT_SUCCESS(status);
    }

    return status;
}

NTSTATUS
WriteReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_WRITE_REPORT all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to  Request Packet.

Return Value:

    NT status code.

--*/

{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    UNREFERENCED_PARAMETER(QueueContext);

    status = RequestGetHidXferPacket_ToWriteToDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // Return error for unknown collection
        //
        status = STATUS_INVALID_PARAMETER;
        return status;
    }

    //
    // before touching buffer make sure buffer is big enough.
    //
    reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        
        return status;
    }

    //
    // No output report state is kept; just acknowledge the transfer.
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}


NTSTATUS
GetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_FEATURE for all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to  Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    
    UNREFERENCED_PARAMETER(QueueContext);
    status = RequestGetHidXferPacket_ToReadFromDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        
        return status;
    }

    //
    // Since output buffer is for write only (no read allowed by UMDF in output
    // buffer), any read from output buffer would be reading garbage), so don't
    // let app embed custom control code in output buffer. The minidriver can
    // support multiple features using separate report ID instead of using
    // custom control code. Since this is targeted at report ID 1, we know it
    // is a request for getting attributes.
    //
    // While KMDF does not enforce the rule (disallow read from output buffer),
    // it is good practice to not do so.
    //

    reportSize = sizeof(features);
    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        
        
        return status;
    }

    //
    // Since this device has one report ID, hidclass would pass on the report
    // ID in the buffer (it wouldn't if report descriptor did not have any report
    // ID). However, since UMDF allows only writes to an output buffer, we can't
    // "read" the report ID from "output" buffer. There is no need to read the
    // report ID since we get it other way as shown above, however this is
    // something to keep in mind.
    //
    packet.reportBuffer[0] = features.reportId;
    packet.reportBuffer[1] = features.DIG_TouchScreenContactCountMaximum;
    
    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}

NTSTATUS
SetFeature(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_SET_FEATURE for the touch collection. The feature
    report (report ID 0x54) carries only the read-only Contact Count
    Maximum, so there is no writable state: accept and ignore.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    UNREFERENCED_PARAMETER(QueueContext);

    status = RequestGetHidXferPacket_ToWriteToDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        return status;
    }

    //
    // Feature payload is one byte (Contact Count Maximum) after the
    // report ID. Nothing to do with it: acknowledge and return success.
    //
    reportSize = 2;
    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        return status;
    }

    WdfRequestSetInformation(Request, reportSize);
    return status;
}

NTSTATUS
GetInputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_INPUT_REPORT for all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    inputReport54_t*        touchReport;
    PMANUAL_QUEUE_CONTEXT   manualQueueContext;

    status = RequestGetHidXferPacket_ToReadFromDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        return status;
    }

    reportSize = sizeof(inputReport54_t);
    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        
        return status;
    }

    //
    // Synchronous report read: return the last active frame (or an empty
    // report when nothing is down). Guarded by ReportLock so the frame
    // cannot tear against a concurrent ISR update.
    //
    touchReport = (inputReport54_t*)packet.reportBuffer;
    RtlZeroMemory(touchReport, reportSize);
    touchReport->reportId = CONTROL_COLLECTION_REPORT_ID;

    manualQueueContext = GetManualQueueContext(
        QueueContext->DeviceContext->ManualQueue);
    WdfSpinLockAcquire(manualQueueContext->ReportLock);
    if (QueueContext->DeviceContext->LastActiveReportValid) {
        RtlCopyMemory(
            touchReport,
            &QueueContext->DeviceContext->LastActiveReport,
            reportSize);
    }
    WdfSpinLockRelease(manualQueueContext->ReportLock);

    //
    // Report how many bytes were copied
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}


NTSTATUS
SetOutputReport(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_SET_OUTPUT_REPORT for all the collection.

Arguments:

    QueueContext - The object context associated with the queue

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    HID_XFER_PACKET         packet;
    ULONG                   reportSize;
    UNREFERENCED_PARAMETER(QueueContext);

    status = RequestGetHidXferPacket_ToWriteToDevice(
                            Request,
                            &packet);
    if( !NT_SUCCESS(status) ) {
        return status;
    }

    if (packet.reportId != CONTROL_COLLECTION_REPORT_ID) {
        //
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        //
        status = STATUS_INVALID_PARAMETER;
        
        return status;
    }

    //
    // before touching buffer make sure buffer is big enough.
    //
    reportSize = sizeof(HIDMINI_OUTPUT_REPORT);

    if (packet.reportBufferLen < reportSize) {
        status = STATUS_INVALID_BUFFER_SIZE;
        return status;
    }

    //
    // No output report state is kept; just acknowledge the transfer.
    //
    WdfRequestSetInformation(Request, reportSize);
    return status;
}


NTSTATUS
GetStringId(
    _In_  WDFREQUEST        Request,
    _Out_ ULONG            *StringId,
    _Out_ ULONG            *LanguageId
    )
/*++

Routine Description:

    Helper routine to decode IOCTL_HID_GET_INDEXED_STRING and IOCTL_HID_GET_STRING.

Arguments:

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    ULONG                   inputValue;

#ifdef _KERNEL_MODE

    WDF_REQUEST_PARAMETERS  requestParameters;

    //
    // IOCTL_HID_GET_STRING:                      // METHOD_NEITHER
    // IOCTL_HID_GET_INDEXED_STRING:              // METHOD_OUT_DIRECT
    //
    // The string id (or string index) is passed in Parameters.DeviceIoControl.
    // Type3InputBuffer. However, Parameters.DeviceIoControl.InputBufferLength
    // was not initialized by hidclass.sys, therefore trying to access the
    // buffer with WdfRequestRetrieveInputMemory will fail
    //
    // Another problem with IOCTL_HID_GET_INDEXED_STRING is that METHOD_OUT_DIRECT
    // expects the input buffer to be Irp->AssociatedIrp.SystemBuffer instead of
    // Type3InputBuffer. That will also fail WdfRequestRetrieveInputMemory.
    //
    // The solution to the above two problems is to get Type3InputBuffer directly
    //
    // Also note that instead of the buffer's content, it is the buffer address
    // that was used to store the string id (or index)
    //

    WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
    WdfRequestGetParameters(Request, &requestParameters);

    inputValue = PtrToUlong(
        requestParameters.Parameters.DeviceIoControl.Type3InputBuffer);

    status = STATUS_SUCCESS;

#else

    WDFMEMORY               inputMemory;
    size_t                  inputBufferLength;
    PVOID                   inputBuffer;

    //
    // mshidumdf.sys updates the IRP and passes the string id (or index) through
    // the input buffer correctly based on the IOCTL buffer type
    //

    status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
    if( !NT_SUCCESS(status) ) {
        KdPrint(("WdfRequestRetrieveInputMemory failed 0x%x\n",status));
        return status;
    }
    inputBuffer = WdfMemoryGetBuffer(inputMemory, &inputBufferLength);

    //
    // make sure buffer is big enough.
    //
    if (inputBufferLength < sizeof(ULONG))
    {
        status = STATUS_INVALID_BUFFER_SIZE;
        KdPrint(("GetStringId: invalid input buffer. size %d, expect %d\n",
                            (int)inputBufferLength, (int)sizeof(ULONG)));
        return status;
    }

    inputValue = (*(PULONG)inputBuffer);

#endif

    //
    // The least significant two bytes of the INT value contain the string id.
    //
    *StringId = (inputValue & 0x0ffff);

    //
    // The most significant two bytes of the INT value contain the language
    // ID (for example, a value of 1033 indicates English).
    //
    *LanguageId = (inputValue >> 16);
    return status;
}


NTSTATUS
GetIndexedString(
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_INDEXED_STRING

Arguments:

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    ULONG                   languageId, stringIndex;

    status = GetStringId(Request, &stringIndex, &languageId);

    // While we don't use the language id, some minidrivers might.
    //
    UNREFERENCED_PARAMETER(languageId);

    if (NT_SUCCESS(status)) {

        if (stringIndex != VHIDMINI_DEVICE_STRING_INDEX)
        {
            status = STATUS_INVALID_PARAMETER;
            
            return status;
        }

        status = RequestCopyFromBuffer(Request, VHIDMINI_DEVICE_STRING, sizeof(VHIDMINI_DEVICE_STRING));
    }
    return status;
}


NTSTATUS
GetString(
    _In_  WDFREQUEST        Request
    )
/*++

Routine Description:

    Handles IOCTL_HID_GET_STRING.

Arguments:

    Request - Pointer to Request Packet.

Return Value:

    NT status code.

--*/
{
    NTSTATUS                status;
    ULONG                   languageId, stringId;
    size_t                  stringSizeCb;
    PWSTR                   string;

    status = GetStringId(Request, &stringId, &languageId);

    // While we don't use the language id, some minidrivers might.
    //
    UNREFERENCED_PARAMETER(languageId);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    switch (stringId){
    case HID_STRING_ID_IMANUFACTURER:
        stringSizeCb = sizeof(VHIDMINI_MANUFACTURER_STRING);
        string = VHIDMINI_MANUFACTURER_STRING;
        break;
    case HID_STRING_ID_IPRODUCT:
        stringSizeCb = sizeof(VHIDMINI_PRODUCT_STRING);
        string = VHIDMINI_PRODUCT_STRING;
        break;
    case HID_STRING_ID_ISERIALNUMBER:
        stringSizeCb = sizeof(VHIDMINI_SERIAL_NUMBER_STRING);
        string = VHIDMINI_SERIAL_NUMBER_STRING;
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
        
        return status;
    }

    status = RequestCopyFromBuffer(Request, string, stringSizeCb);
    return status;
}


NTSTATUS
ManualQueueCreate(
    _In_  WDFDEVICE         Device,
    _Out_ WDFQUEUE          *Queue
    )
/*++
Routine Description:

    This function creates a manual I/O queue to receive IOCTL_HID_READ_REPORT
    forwarded from the device's default queue handler.

    It also creates a periodic timer to check the queue and complete any pending
    request with data from the device. Here timer expiring is used to simulate
    a hardware event that new data is ready.

    The workflow is like this:

    - Hidclass.sys sends an ioctl to the miniport to read input report.

    - The request reaches the driver's default queue. As data may not be avaiable
      yet, the request is forwarded to a second manual queue temporarily.

    - Later when data is ready (as simulated by timer expiring), the driver
      checks for any pending request in the manual queue, and then completes it.

    - Hidclass gets notified for the read request completion and return data to
      the caller.

    On the other hand, for IOCTL_HID_WRITE_REPORT request, the driver simply
    acknowledges the request and completes it immediately. There is no need
    to use another queue for write operations.

Arguments:

    Device - Handle to a framework device object.

    Queue - Output pointer to a framework I/O queue handle, on success.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                status;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDF_OBJECT_ATTRIBUTES   queueAttributes;
    WDF_OBJECT_ATTRIBUTES   lockAttributes;
    WDFQUEUE                queue;
    PMANUAL_QUEUE_CONTEXT   queueContext;
    
    WDF_IO_QUEUE_CONFIG_INIT(
                            &queueConfig,
                            WdfIoQueueDispatchManual);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
                            &queueAttributes,
                            MANUAL_QUEUE_CONTEXT);

    status = WdfIoQueueCreate(
                            Device,
                            &queueConfig,
                            &queueAttributes,
                            &queue);

    if( !NT_SUCCESS(status) ) {
        
        
        return status;
    }

    queueContext = GetManualQueueContext(queue);
    queueContext->Queue         = queue;
    queueContext->DeviceContext = GetDeviceContext(Device);

    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = queue;
    status = WdfSpinLockCreate(&lockAttributes, &queueContext->ReportLock);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(queue);
        return status;
    }

    //
    // Signaled whenever no delivery loop is active. OnD0Exit waits on this
    // to ensure no in-flight completion touches objects it is about to
    // delete.
    //
    KeInitializeEvent(
        &queueContext->DeliveryIdleEvent,
        NotificationEvent,
        TRUE);

    *Queue = queue;
    return status;
}

BOOLEAN
OnInterruptIsr(
    _In_  WDFINTERRUPT FxInterrupt,
    _In_  ULONG        MessageID
)
/*++

  Routine Description:

    This routine responds to interrupts generated by the H/W.
    It then waits indefinitely for the user to signal that
    the interrupt has been acknowledged, allowing the ISR to
    return. This ISR is called at PASSIVE_LEVEL.

  Arguments:

    Interrupt - a handle to a framework interrupt object
    MessageID - message number identifying the device's
        hardware interrupt message (if using MSI)

  Return Value:

    TRUE if interrupt recognized.

--*/
{
    BOOLEAN fInterruptRecognized = TRUE;
    //BOOLEAN fNotificationSent;
    WDFDEVICE device;
    PDEVICE_CONTEXT pDevice;
    PMANUAL_QUEUE_CONTEXT queueContext;
    NTSTATUS status;
    WDFREQUEST request = WDF_NO_HANDLE;
    BOOLEAN requestRetrieved = FALSE;
    inputReport54_t    readReport = { 0 };
    inputReport54_t    downReport = { 0 };
    //
    // Worst case: 10 bytes copied from preBuffer plus 8 bytes per remaining
    // contact plus 2 trailing bytes = 8*MAX + 4 bytes.
    //
    UINT8 allBuf[MAX_POINT_NUM * BYTES_PER_COORD + 2 * BYTES_CHKSUM] = { 0 };

    UINT8 touchId = 0;
    int x = 0, y = 0;
    UNREFERENCED_PARAMETER(MessageID);


    device = WdfInterruptGetDevice(FxInterrupt);

    pDevice = GetDeviceContext(device);
    //
    // Lock-free read is safe here: SpbDeviceClose sets OnClose (under
    // ReportLock) *before* disabling this interrupt, and KMDF serializes
    // passive-level ISR callbacks per interrupt object.
    //
    if (pDevice->OnClose)
        return TRUE;

    //
    // Notify the app that an interrupt has occurred.
    //

    //fNotificationSent = SpbPeripheralInterruptNotify(pDevice);

    //if (fNotificationSent)
    //{
        //
        // Stall in ISR until acknowledged by user.
        //
        // Note: In a 'real' driver, the ISR should directly
        //       acknowledge the interrupt and then queue
        //       a workitem to carry out any additional
        //       processing. The ISR should never call
        //       KeWaitForSingleObject as done below.
        //

    // Get Pre Buffer
    BYTE preBuffer[12] = { 0 };
    RtlZeroMemory(preBuffer, sizeof(preBuffer));

    SpbDeviceWriteRead(pDevice, gtx9886_get_pre_coor, &preBuffer[0], sizeof(gtx9886_get_pre_coor), sizeof(preBuffer));

    //
    // Only TOUCH events carry point data; request/gesture/hotknot packets
    // must not be parsed as coordinates. NOTE: a REQUEST event (0x40) is
    // acknowledged (clean_coor below) without being serviced because the
    // firmware request protocol is unknown. Log it (rate-limited) so a
    // future firmware-protocol investigation has a starting point.
    //
    if ((preBuffer[0] & GOODIX_TOUCH_EVENT) != GOODIX_TOUCH_EVENT) {
        if ((preBuffer[0] & GOODIX_REQUEST_EVENT) &&
            ((++pDevice->RequestEventLogCount % 64) == 1)) {
            KdPrint(("GTX9886: unserviced firmware request event 0x%02X\n",
                preBuffer[0]));
        }
        goto exit;
    }

    BYTE touch_count = preBuffer[1] & 0x0F;
    if (touch_count > MAX_POINT_NUM)
        goto exit;

    //
    // allBuf layout (gtx9886 packet, verified on device):
    //   [0..7]   first contact (8 bytes), copied from the pre-read buffer
    //   [8..9]   preBuffer[10..11]: first 2 bytes of the second contact
    //   [10..]   remainder of the packet, read via gtx9886_get_coor
    //            (rest of the second contact, further contacts and the
    //            2 trailing bytes)
    //
    RtlCopyMemory(allBuf, &preBuffer[2], sizeof(preBuffer) - 2);
    if (touch_count > 1) {
        SpbDeviceWriteRead(pDevice, gtx9886_get_coor, &allBuf[10],
            sizeof(gtx9886_get_coor),
            BYTES_PER_COORD * (touch_count - 1));
    }

    readReport.DIG_TouchScreenContactCount = touch_count;
#ifdef DBG
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Point Buffer %x %x %x %x %x %x %x %x", preBuffer[0], preBuffer[1], preBuffer[2], preBuffer[3], preBuffer[4], preBuffer[5], preBuffer[6], preBuffer[7]);
#endif

    switch(touch_count){
    case 0:
        //
        // All points leave: replay the last active frame and release every
        // contact so the HID layer sees a symmetric multi-touch lift. If no
        // active frame is cached (nothing was down), drop the stray lift.
        // LastActiveReportValid is cleared later under ReportLock.
        //
        if (!pDevice->LastActiveReportValid)
            goto exit;

        RtlCopyMemory(
            &readReport,
            &pDevice->LastActiveReport,
            sizeof(readReport));
        for (BYTE index = 0;
            index < readReport.DIG_TouchScreenContactCount;
            index++) {
            readReport.points[index * 6] = 0x04;
        }
#ifdef DBG
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "All points leave");
#endif
        break;
    case 1:
        // Do not get all buffer, simply get point from preBuffer 
        x = preBuffer[3] | (preBuffer[4] << 8);
        y = preBuffer[5] | (preBuffer[6] << 8);
        touchId = preBuffer[2] & 0x0F;
        TransformPoint(pDevice, &x, &y);
        readReport.points[0] = 0x07;  // In Point
        readReport.points[1] = touchId;
        readReport.points[2] = x & 0xFF;
        readReport.points[3] = (x >> 8) & 0x0F;
        readReport.points[4] = y & 0xFF;
        readReport.points[5] = (y >> 8) & 0x0F;
        readReport.DIG_TouchScreenContactCount = 1;
#ifdef DBG
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Point Enter X:%d, Y:%d", x, y);
#endif
        break;
    default:
        // Remaining contacts were already read into allBuf above.
#ifdef DBG
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Multi Buffer %d %x %x %x %x %x %x %x", allBuf[0], allBuf[1], allBuf[2], allBuf[3], allBuf[4], allBuf[5]
            , allBuf[6], allBuf[7]);
#endif


        for (int i = 0; i < touch_count; i++)
        {
            touchId = (allBuf[0 + i * 8] & 0x0F);
            x = allBuf[1 + i * 8] | (allBuf[2 + i * 8] << 8);
            y = allBuf[3 + i * 8] | (allBuf[4 + i * 8] << 8);
            TransformPoint(pDevice, &x, &y);
            readReport.points[i * 6 + 0] = 0x07;  // In Point
            readReport.points[i * 6 + 1] = touchId;
            readReport.points[i * 6 + 2] = x & 0xFF;
            readReport.points[i * 6 + 3] = (x >> 8) & 0x0F;
            readReport.points[i * 6 + 4] = y & 0xFF;
            readReport.points[i * 6 + 5] = (y >> 8) & 0x0F;
#ifdef DBG
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Multi %d X:%d, Y:%d", touchId, x, y);
#endif
        }
    }

    readReport.reportId = CONTROL_FEATURE_REPORT_ID;

    //
    // Keep a snapshot of the pure down frame: LastActiveReport is used to
    // replay lifts on "all fingers up" and must not contain tip-up slots.
    //
    RtlCopyMemory(&downReport, &readReport, sizeof(downReport));

    //
    // Partial lift: append every contact that was down in the previous
    // frame but is absent from this one as an explicit tip-up slot, so
    // the HID layer lifts it immediately instead of waiting for its
    // slower contact-disappearance debounce. The contact's last known
    // coordinates are preserved so the UP lands at its final position.
    //
    // Lock contract: ActiveIds/ActiveCount/LastActiveReport are read here
    // without ReportLock. This is safe because they are only written by
    // this ISR (under ReportLock below, and KMDF serializes ISR callbacks
    // per interrupt object) or by ClearTouchReports, which runs only while
    // the interrupt is disabled (D0Entry) or after SpbDeviceClose has
    // disabled it (D0Exit).
    //
    if ((touch_count != 0) &&
        pDevice->LastActiveReportValid &&
        (pDevice->ActiveCount != 0)) {
        for (BYTE a = 0; a < pDevice->ActiveCount; a++) {
            UINT8 liftedId = pDevice->ActiveIds[a];
            BOOLEAN stillDown = FALSE;
            BYTE c, l;

            for (c = 0; c < touch_count; c++) {
                if (readReport.points[c * 6 + 1] == liftedId) {
                    stillDown = TRUE;
                    break;
                }
            }
            if (stillDown) {
                continue;
            }
            if (readReport.DIG_TouchScreenContactCount >= MAX_POINT_NUM) {
                //
                // No free slot left for the lift; this contact falls back
                // to disappearance detection for one frame.
                //
                continue;
            }

            c = readReport.DIG_TouchScreenContactCount;
            readReport.points[c * 6 + 0] = 0x04;    // tip up
            readReport.points[c * 6 + 1] = liftedId;
            readReport.points[c * 6 + 2] = 0;
            readReport.points[c * 6 + 3] = 0;
            readReport.points[c * 6 + 4] = 0;
            readReport.points[c * 6 + 5] = 0;
            for (l = 0;
                l < pDevice->LastActiveReport.DIG_TouchScreenContactCount;
                l++) {
                if (pDevice->LastActiveReport.points[l * 6 + 1] == liftedId) {
                    readReport.points[c * 6 + 2] =
                        pDevice->LastActiveReport.points[l * 6 + 2];
                    readReport.points[c * 6 + 3] =
                        pDevice->LastActiveReport.points[l * 6 + 3];
                    readReport.points[c * 6 + 4] =
                        pDevice->LastActiveReport.points[l * 6 + 4];
                    readReport.points[c * 6 + 5] =
                        pDevice->LastActiveReport.points[l * 6 + 5];
                    break;
                }
            }
            readReport.DIG_TouchScreenContactCount++;
        }
    }

    queueContext = GetManualQueueContext(pDevice->ManualQueue);

    //
    // Cache the report under the spin lock and start the delivery loop if it
    // is idle. Only one loop (here or in ReadReport) runs at a time; see
    // CompleteTouchReport. LastActiveReport is updated under the same lock
    // so a concurrent GetInputReport sees a consistent frame.
    //
    WdfSpinLockAcquire(queueContext->ReportLock);
    if (touch_count != 0) {
        RtlCopyMemory(
            &pDevice->LastActiveReport,
            &downReport,
            sizeof(downReport));
        pDevice->LastActiveReportValid = TRUE;
        pDevice->ActiveCount = touch_count;
        for (BYTE c = 0; c < touch_count; c++) {
            pDevice->ActiveIds[c] = downReport.points[c * 6 + 1];
        }
    }
    else {
        pDevice->LastActiveReportValid = FALSE;
        pDevice->ActiveCount = 0;
    }
    CacheTouchReportLocked(queueContext, &readReport);

    if (!queueContext->DeliveryInProgress && (queueContext->PendingCount != 0)) {
        status = WdfIoQueueRetrieveNextRequest(queueContext->Queue, &request);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(
                &readReport,
                &queueContext->PendingReports[queueContext->PendingHead],
                sizeof(readReport));
            queueContext->PendingHead =
                (queueContext->PendingHead + 1) % TOUCH_REPORT_SLOT_COUNT;
            queueContext->PendingCount--;
            queueContext->DeliveryInProgress = TRUE;
            KeClearEvent(&queueContext->DeliveryIdleEvent);
            requestRetrieved = TRUE;
        }
    }
    else {
        status = STATUS_NO_MORE_ENTRIES;
    }
    WdfSpinLockRelease(queueContext->ReportLock);

    if (requestRetrieved) {
        CompleteTouchReport(queueContext, request, &readReport);
    }

exit:
    SpbDeviceWrite(pDevice, gtx9886_clean_coor, sizeof(gtx9886_clean_coor));
    return fInterruptRecognized;
}

NTSTATUS
SpbDeviceOpen(
    _In_  PDEVICE_CONTEXT  pDevice
)
{
    PMANUAL_QUEUE_CONTEXT queueContext =
        GetManualQueueContext(pDevice->ManualQueue);
    WDF_IO_TARGET_OPEN_PARAMS  openParams;
    NTSTATUS status;

    if ((pDevice->Interrupt == WDF_NO_HANDLE) ||
        (pDevice->SpbController == WDF_NO_HANDLE)) {
        return STATUS_NOT_FOUND;
    }

    DECLARE_UNICODE_STRING_SIZE(DevicePath, RESOURCE_HUB_PATH_SIZE);
    RESOURCE_HUB_CREATE_PATH_FROM_ID(
        &DevicePath,
        pDevice->PeripheralId.LowPart,
        pDevice->PeripheralId.HighPart);

    //
    // Open a handle to the SPB controller.
    //

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &DevicePath,
        (GENERIC_READ | GENERIC_WRITE));

    openParams.ShareAccess = 0;
    openParams.CreateDisposition = FILE_OPEN;
    openParams.FileAttributes = FILE_ATTRIBUTE_NORMAL;

    status = WdfIoTargetOpen(
        pDevice->SpbController,
        &openParams);

    if (!NT_SUCCESS(status)) {
        KdPrint(("GTX9886: WdfIoTargetOpen failed 0x%08X\n", status));
        return status;
    }

    // Init 9886 here
    //WA(0x30, 0xf0, 0xaa);
    //WA(0x30, 0xf0, 0xcc);
    //WA(0x30, 0xf0, 0xaa);
    //WA(0x30, 0xf0, 0xcc);
    //WA(0x30, 0xf0, 0xaa);
    //WA(0x30, 0xf0, 0xcc);
    //WA(0x30, 0xf0, 0xaa);
    //WA(0x30, 0xf0, 0xcc);
    //WA(0x30, 0xf0, 0xaa);
    //WA(0x6f, 0x68, 0x80, 0x00, 0x80);
    //WA(0x6f, 0x78, 0x19, 0x01, 0x2b, 0xbb, 0x01, 0x1f, 0x34, 0xf1, 0x88, 0x82, 0x1e, 0x20, 0x04, 0x01, 0x21, 0x01, 0x17, 0xde, 0x08, 0x07, 0x04, 0x00, 0x00, 0x0f, 0x21, 0x00, 0x00, 0x32, 0x00, 0x00, 0x22, 0x21, 0x12, 0x00, 0x3f, 0xff, 0xff, 0x50, 0x02, 0x58, 0x28, 0x24, 0x4a, 0x27, 0x00, 0x49, 0x03, 0x01, 0x02, 0x04, 0x06, 0x07, 0x08, 0x0b, 0x09, 0x0c, 0x0d, 0x18, 0x17, 0x19, 0x1a, 0x1c, 0x1d, 0x1f, 0x21, 0x22, 0x23, 0x20, 0x1e, 0x1b, 0x0a, 0x05, 0x4b, 0xff, 0xff, 0xff, 0x32, 0x34, 0x33, 0x37, 0x36, 0x40, 0x39, 0x3e, 0x3a, 0x38, 0x3f, 0x35, 0x3b, 0x3c, 0x41, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x03, 0x58, 0x0d, 0x11, 0x13, 0x0e, 0x23, 0x14, 0x20, 0x22, 0x21, 0x1f, 0x1d, 0x1c, 0x1b, 0x18, 0x1a, 0x17, 0x16, 0x0b, 0x0c, 0x0a, 0x09, 0x07, 0x06, 0x04, 0x02, 0x01, 0x00, 0x03, 0x05, 0x08, 0x19, 0x1e, 0x12, 0xff, 0xff, 0xff, 0x1e, 0x1c, 0x1d, 0x19, 0x1a, 0x02, 0x17, 0x12, 0x16, 0x18, 0x03, 0x1b, 0x15, 0x14, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x04, 0x09, 0x01, 0x02, 0x01, 0x05, 0x19, 0x2d, 0x17, 0xde, 0x00, 0xaf, 0x05, 0x0a, 0x04, 0x38, 0x09, 0x24, 0x00, 0x3c, 0x00, 0x5a, 0x0a, 0x0f, 0xd9, 0x06, 0x02, 0x01, 0x00, 0xf7, 0x07, 0x14, 0x14, 0x16);
    //WA(0x70, 0x76, 0x1a, 0x20, 0x14, 0x1a, 0x22, 0x16, 0x1a, 0x20, 0x16, 0x1a, 0x20, 0x00, 0x00, 0x47, 0x08, 0x0c, 0x0c, 0x15, 0x15, 0x08, 0x08, 0x00, 0x40, 0x00, 0x80, 0x00, 0xc0, 0x02, 0x02, 0x6c, 0x09, 0x09, 0x19, 0x00, 0x0a, 0x00, 0x0a, 0x00, 0x0b, 0x00, 0x0a, 0xac, 0x0a, 0x13, 0x1f, 0x05, 0x03, 0x00, 0x28, 0x08, 0x00, 0x78, 0x94, 0x8c, 0x87, 0x83, 0x80, 0x7e, 0x1d, 0x20, 0x23, 0x26, 0x29, 0x3d, 0x0b, 0x02, 0x0a, 0x05, 0xe4, 0x0c, 0x05, 0x08, 0x00, 0x00, 0x00, 0x00, 0xe7, 0x0d, 0x0d, 0x41, 0x38, 0xa5, 0x0b, 0xb8, 0x0d, 0x2d, 0x04, 0x3c, 0x00, 0x1e, 0x00, 0x28, 0x45, 0x0e, 0x0c, 0x03, 0x0a, 0x01, 0x0e, 0xb4, 0x3c, 0xaa, 0x00, 0x00, 0x00, 0x32, 0x50, 0xae, 0x0f, 0x00, 0xf1, 0x10, 0x00, 0xf0, 0x11, 0x0b, 0x00, 0x01, 0x00, 0x20, 0x8d, 0x4e, 0x20, 0x64, 0x23, 0x00, 0x00, 0x41, 0x12, 0x07, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0xe5, 0x13, 0x08, 0x02, 0x58, 0x00, 0xc8, 0x00, 0x0f, 0x00, 0x00, 0xb4, 0x14, 0x00, 0xec, 0x15, 0x25, 0x00, 0x00, 0x00, 0x04, 0x00, 0x50, 0x01, 0x05, 0x02, 0x00, 0x1c, 0x00, 0x02, 0x00, 0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x05, 0x05, 0x08, 0x00, 0x01, 0x00, 0x3a, 0x98, 0x4e, 0x20, 0x14, 0x32, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x16, 0x0b, 0x15, 0x20, 0x64, 0x0b, 0xb8, 0x00, 0xa0, 0x00, 0x64, 0x00, 0x00, 0x7f, 0x17, 0x00, 0xe9, 0x18, 0x0c, 0x01, 0x32, 0x00, 0x64, 0x00, 0xc8, 0x1a, 0x1f, 0x25, 0x2b, 0x00, 0x00, 0xf4, 0x19, 0x11, 0x01, 0x0a, 0x14, 0x50, 0x1e, 0x04, 0x0a, 0x32, 0x15, 0x07, 0x00, 0x0a, 0x22, 0x22, 0x36, 0x00, 0x00, 0x69, 0x1a, 0x5e, 0x00, 0x04, 0x03, 0x35, 0x03, 0x35, 0x13, 0xb1);
    //WA(0x71, 0x74, 0x02, 0x24, 0x03, 0x35, 0x03, 0x35, 0x13, 0xb1, 0x01, 0x35, 0x1f, 0xff, 0x1f, 0xff, 0x1f, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0x06, 0x07, 0x0b, 0x18, 0x26, 0x2e, 0x02, 0x04, 0x06, 0x07, 0x0b, 0x18, 0x26, 0x2e, 0x28, 0x5a, 0x14, 0x5a, 0x14, 0x02, 0xc8, 0x32, 0x06, 0x64, 0x64, 0x00, 0x5a, 0x08, 0x00, 0x64, 0xa8, 0x02, 0x5f, 0xfe, 0x0b, 0x3c, 0x1e, 0x3c, 0x32, 0x03, 0xe8, 0x14, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x35, 0x1b, 0x00, 0xe5, 0x1c, 0x0f, 0x0a, 0x04, 0x10, 0x00, 0x1e, 0x00, 0x1e, 0x02, 0xc0, 0x00, 0x0f, 0x00, 0xc8, 0x00, 0x00, 0xe2, 0x1d, 0x00, 0xe3, 0x1e, 0x00, 0xe2, 0x1f, 0x00, 0xe1, 0x20, 0x04, 0x00, 0x00, 0x69, 0x96, 0xdd, 0x21, 0x0a, 0x01, 0x00, 0x40, 0x00, 0x01, 0x00, 0x60, 0x01, 0x00, 0x00, 0x32, 0x22, 0x13, 0x22, 0x05, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6b, 0x23, 0x04, 0x00, 0x00, 0x20, 0x00, 0xb9, 0x24, 0x00, 0xdc, 0x25, 0x20, 0x07, 0x00, 0x0c, 0x5c, 0x11, 0xa4, 0x36, 0x76, 0x3b, 0xd3, 0x06, 0x1b, 0x33, 0x63, 0x00, 0x15, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x6b, 0x0f, 0x96, 0x40, 0x35, 0x00, 0x5a, 0x00, 0x4b, 0x32, 0x18, 0x21, 0x26, 0x04, 0x27, 0x00, 0x00, 0x00, 0xaf, 0x27, 0x00, 0xd9, 0x28, 0x00, 0xd8, 0x29, 0x00, 0xd7, 0x2a, 0x00, 0xd6, 0x2b, 0x00, 0xd5);
    //WA(0x6f, 0x68, 0x83, 0x00, 0x7d);
    //WA(0x30, 0xf0, 0xaa);
    //WA(0x30, 0xf0, 0xcc);

    //
    // The device is now ready to accept reports.
    //
    WdfSpinLockAcquire(queueContext->ReportLock);
    pDevice->OnClose = FALSE;
    WdfSpinLockRelease(queueContext->ReportLock);

    //
    // Enable the interrupt. WdfInterruptEnable returns VOID, so failures
    // can only surface through the framework verifier, not here.
    //
    WdfInterruptEnable(pDevice->Interrupt);

    return STATUS_SUCCESS;
}
VOID
SpbDeviceClose(
    _In_  PDEVICE_CONTEXT  pDevice
)
{
    PMANUAL_QUEUE_CONTEXT queueContext =
        GetManualQueueContext(pDevice->ManualQueue);

    //
    // Order matters: mark the device closed first (ISR and ReadReport check
    // it), then disable the interrupt so no new ISR can start, then close the
    // SPB target.
    //
    WdfSpinLockAcquire(queueContext->ReportLock);
    pDevice->OnClose = TRUE;
    WdfSpinLockRelease(queueContext->ReportLock);

    if (pDevice->Interrupt != WDF_NO_HANDLE) {
        WdfInterruptDisable(pDevice->Interrupt);
    }
    if (pDevice->SpbController != WDF_NO_HANDLE) {
        WdfIoTargetClose(pDevice->SpbController);
    }
}
EVT_WDF_REQUEST_COMPLETION_ROUTINE SpbSyncCompletion;

VOID
SpbSyncCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
    _In_ WDFCONTEXT Context
)
{
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(CompletionParams);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
}

static SPB_SYNC_SLOT*
SpbDeviceAcquireSlot(
    _In_ PDEVICE_CONTEXT pDevice
)
/*++
  Allocate a sync slot. Only the serialized passive-level ISR calls this,
  so no locking is needed. If every slot is held by a hung request, try to
  reclaim one by cancelling its request with a bounded wait; if that also
  fails, refuse the transfer.
--*/
{
    LARGE_INTEGER timeout;
    ULONG i;

    for (i = 0; i < SPB_MAX_INFLIGHT; i++) {
        if (!pDevice->SpbSlots[i].InUse) {
            pDevice->SpbSlots[i].InUse = TRUE;
            return &pDevice->SpbSlots[i];
        }
    }

    //
    // Every slot is held by a hung request. Attempt to cancel each one in
    // turn; a request whose event fires within the cancel window has
    // terminated and its slot can be reused.
    //
    for (i = 0; i < SPB_MAX_INFLIGHT; i++) {
        SPB_SYNC_SLOT* slot = &pDevice->SpbSlots[i];
        if (slot->Request == WDF_NO_HANDLE) {
            continue;
        }
        (VOID)WdfRequestCancelSentRequest(slot->Request);
        timeout.QuadPart = -(LONGLONG)SPB_CANCEL_TIMEOUT_MS * 1000 * 10;
        if (KeWaitForSingleObject(&slot->Event, Executive, KernelMode, FALSE,
                &timeout) != STATUS_TIMEOUT) {
            //
            // The hung request has terminated; its slot is reusable.
            //
            slot->Request = WDF_NO_HANDLE;
            slot->InUse = TRUE;
            return slot;
        }
    }

    KdPrint(("GTX9886: all SPB sync slots hung, dropping transfer\n"));
    return NULL;
}

static NTSTATUS
SpbDeviceSendAndWait(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ SPB_SYNC_SLOT* slot,
    _In_ WDFREQUEST Request,
    _In_ ULONG TimeoutMs
)
/*++
  Send an already-formatted request to the SPB target and wait for its
  completion with a bounded timeout. The request buffer lives in the slot,
  so a request that stays in flight after a timeout can never touch freed
  stack memory. The slot is released (and the completed request object
  deleted) on every path except the one where cancellation also hangs;
  there the slot stays occupied and the request is reclaimed when the SPB
  target is closed at device removal.
--*/
{
    LARGE_INTEGER timeout;
    LARGE_INTEGER cancelTimeout;
    NTSTATUS status;
    BOOLEAN sendStatus;

    slot->Request = Request;
    KeClearEvent(&slot->Event);
    WdfRequestSetCompletionRoutine(Request, SpbSyncCompletion, &slot->Event);

    sendStatus = WdfRequestSend(Request, pDevice->SpbController, NULL);
    if (!sendStatus) {
        //
        // Completed synchronously; the completion routine has already
        // signaled the event.
        //
        slot->Request = WDF_NO_HANDLE;
        slot->InUse = FALSE;
        status = WdfRequestGetStatus(Request);
        WdfObjectDelete(Request);
        return status;
    }

    timeout.QuadPart = -(LONGLONG)TimeoutMs * 1000 * 10;
    status = KeWaitForSingleObject(
        &slot->Event, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        KdPrint(("GTX9886: SPB transfer timed out after %lu ms, cancelling\n",
            TimeoutMs));
        (VOID)WdfRequestCancelSentRequest(Request);
        cancelTimeout.QuadPart = -(LONGLONG)SPB_CANCEL_TIMEOUT_MS * 1000 * 10;
        status = KeWaitForSingleObject(
            &slot->Event, Executive, KernelMode, FALSE, &cancelTimeout);
        if (status == STATUS_TIMEOUT) {
            //
            // Cancellation is hung as well. Leave the slot occupied and
            // the request in flight; both the slot buffer and the event
            // live in the device context, so the late completion is safe.
            //
            return STATUS_IO_TIMEOUT;
        }
        slot->Request = WDF_NO_HANDLE;
        slot->InUse = FALSE;
        WdfObjectDelete(Request);
        return STATUS_IO_TIMEOUT;
    }

    slot->Request = WDF_NO_HANDLE;
    slot->InUse = FALSE;
    status = WdfRequestGetStatus(Request);
    WdfObjectDelete(Request);
    return status;
}

static NTSTATUS
SpbDeviceWriteWithTimeout(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ const PVOID pInputBuffer,
    _In_ size_t inputBufferLength
)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES memoryAttributes;
    WDFMEMORY memory;
    WDFREQUEST request;
    SPB_SYNC_SLOT* slot;
    NTSTATUS status;

    if (inputBufferLength > SPB_SLOT_BUFFER_SIZE) {
        return STATUS_BUFFER_OVERFLOW;
    }

    slot = SpbDeviceAcquireSlot(pDevice);
    if (slot == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Copy the payload into the slot-owned buffer so the request never
    // references caller (stack) memory beyond this function's lifetime.
    //
    RtlCopyMemory(slot->Buffer, pInputBuffer, inputBufferLength);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = pDevice->Device;

    status = WdfRequestCreate(&attributes, pDevice->SpbController, &request);
    if (!NT_SUCCESS(status)) {
        slot->InUse = FALSE;
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&memoryAttributes);
    memoryAttributes.ParentObject = request;

    status = WdfMemoryCreatePreallocated(
        &memoryAttributes,
        slot->Buffer,
        inputBufferLength,
        &memory);
    if (!NT_SUCCESS(status)) {
        slot->InUse = FALSE;
        WdfObjectDelete(request);
        return status;
    }

    status = WdfIoTargetFormatRequestForWrite(
        pDevice->SpbController,
        request,
        memory,
        NULL,
        NULL);
    if (!NT_SUCCESS(status)) {
        slot->InUse = FALSE;
        WdfObjectDelete(request);
        return status;
    }

    return SpbDeviceSendAndWait(pDevice, slot, request, SPB_TRANSFER_TIMEOUT_MS);
}

static NTSTATUS
SpbDeviceReadWithTimeout(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ PVOID pOutputBuffer,
    _In_ size_t outputBufferLength
)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES memoryAttributes;
    WDFMEMORY memory;
    WDFREQUEST request;
    SPB_SYNC_SLOT* slot;
    NTSTATUS status;

    if (outputBufferLength > SPB_SLOT_BUFFER_SIZE) {
        return STATUS_BUFFER_OVERFLOW;
    }

    slot = SpbDeviceAcquireSlot(pDevice);
    if (slot == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = pDevice->Device;

    status = WdfRequestCreate(&attributes, pDevice->SpbController, &request);
    if (!NT_SUCCESS(status)) {
        slot->InUse = FALSE;
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&memoryAttributes);
    memoryAttributes.ParentObject = request;

    status = WdfMemoryCreatePreallocated(
        &memoryAttributes,
        slot->Buffer,
        outputBufferLength,
        &memory);
    if (!NT_SUCCESS(status)) {
        slot->InUse = FALSE;
        WdfObjectDelete(request);
        return status;
    }

    status = WdfIoTargetFormatRequestForRead(
        pDevice->SpbController,
        request,
        memory,
        NULL,
        NULL);
    if (!NT_SUCCESS(status)) {
        slot->InUse = FALSE;
        WdfObjectDelete(request);
        return status;
    }

    status = SpbDeviceSendAndWait(pDevice, slot, request, SPB_TRANSFER_TIMEOUT_MS);
    if (!NT_SUCCESS(status)) {
        //
        // Timed out (data invalid) or failed; on success the slot buffer
        // holds the payload.
        //
        return status;
    }

    //
    // Copy the payload out of the slot buffer. The request has already
    // been completed and deleted by SpbDeviceSendAndWait.
    //
    RtlCopyMemory(pOutputBuffer, slot->Buffer, outputBufferLength);
    return status;
}

VOID
SpbDeviceWrite(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ const PVOID pInputBuffer,
    _In_ size_t inputBufferLength
)
{
    NTSTATUS status;

    status = SpbDeviceWriteWithTimeout(pDevice, pInputBuffer, inputBufferLength);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("GTX9886: SpbDeviceWrite failed 0x%08X\n", status));
    }
}

VOID
SpbDeviceWriteRead(
    _In_ PDEVICE_CONTEXT pDevice,
    _In_ const PVOID pInputBuffer,
    _In_ PVOID pOutputBuffer,
    _In_ size_t inputBufferLength,
    _In_ size_t outputBufferLength
)
{
    NTSTATUS status;

    status = SpbDeviceWriteWithTimeout(pDevice, pInputBuffer, inputBufferLength);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("GTX9886: SpbDeviceWriteRead (write) failed 0x%08X\n", status));
        return;
    }

    status = SpbDeviceReadWithTimeout(pDevice, pOutputBuffer, outputBufferLength);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("GTX9886: SpbDeviceWriteRead (read) failed 0x%08X\n", status));
    }
}

NTSTATUS
ReadCoordinateConfigFromRegistry(
    WDFDEVICE Device
)
/*++
Routine Description:
    Read the coordinate transform configuration (XRevert/YRevert/XYExchange
    and the panel bounds XMin/XMax/YMin/YMax) from the device registry key.
    Missing values fall back to the compiled-in defaults; out-of-range
    values are rejected because report coordinates are encoded in 12 bits.
Arguments:
    device - pointer to a device object.
Return Value:
    NT status code.
--*/
{
    WDFKEY          hKey = NULL;
    NTSTATUS        status;
    UNICODE_STRING  xRevertName;
    UNICODE_STRING  yRevertName;
    UNICODE_STRING  xYExchangeName;
    UNICODE_STRING  xMinName;
    UNICODE_STRING  xMaxName;
    UNICODE_STRING  yMinName;
    UNICODE_STRING  yMaxName;
    NTSTATUS        queryStatus;
    ULONG           queryFailures = 0;
    PDEVICE_CONTEXT deviceContext = GetDeviceContext(Device);

    status = WdfDeviceOpenRegistryKey(Device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_READ,
        WDF_NO_OBJECT_ATTRIBUTES,
        &hKey);

    if (NT_SUCCESS(status)) {

        RtlInitUnicodeString(&xRevertName, L"XRevert");
        RtlInitUnicodeString(&yRevertName, L"YRevert");
        RtlInitUnicodeString(&xYExchangeName, L"XYExchange");
        RtlInitUnicodeString(&xMinName, L"XMin");
        RtlInitUnicodeString(&xMaxName, L"XMax");
        RtlInitUnicodeString(&yMinName, L"YMin");
        RtlInitUnicodeString(&yMaxName, L"YMax");

        //
        // Query every value independently; missing values fall back to the
        // defaults set in EvtDeviceAdd. Count the failures so a partially
        // populated registry does not go unnoticed.
        //
        queryStatus = WdfRegistryQueryULong(hKey, &xRevertName, &deviceContext->XRevert);
        if (!NT_SUCCESS(queryStatus)) queryFailures++;
        queryStatus = WdfRegistryQueryULong(hKey, &yRevertName, &deviceContext->YRevert);
        if (!NT_SUCCESS(queryStatus)) queryFailures++;
        queryStatus = WdfRegistryQueryULong(hKey, &xYExchangeName, &deviceContext->XYExchange);
        if (!NT_SUCCESS(queryStatus)) queryFailures++;
        queryStatus = WdfRegistryQueryULong(hKey, &xMinName, &deviceContext->XMin);
        if (!NT_SUCCESS(queryStatus)) queryFailures++;
        queryStatus = WdfRegistryQueryULong(hKey, &xMaxName, &deviceContext->XMax);
        if (!NT_SUCCESS(queryStatus)) queryFailures++;
        queryStatus = WdfRegistryQueryULong(hKey, &yMinName, &deviceContext->YMin);
        if (!NT_SUCCESS(queryStatus)) queryFailures++;
        queryStatus = WdfRegistryQueryULong(hKey, &yMaxName, &deviceContext->YMax);
        if (!NT_SUCCESS(queryStatus)) queryFailures++;

        if (queryFailures != 0) {
            KdPrint(("GTX9886: %lu coordinate registry value(s) missing, using defaults\n",
                queryFailures));
            status = queryStatus;
        }

        //
        // Report coordinates are encoded in 12 bits (see the descriptor
        // patch and report packing), so reject bounds outside 1..4095
        // before they wrap around at runtime.
        //
        if ((deviceContext->XMax < 1) || (deviceContext->XMax > 0x0FFF) ||
            (deviceContext->YMax < 1) || (deviceContext->YMax > 0x0FFF) ||
            (deviceContext->XMin > deviceContext->XMax) ||
            (deviceContext->YMin > deviceContext->YMax)) {
            KdPrint(("GTX9886: invalid coordinate range (%lu..%lu, %lu..%lu), using defaults\n",
                deviceContext->XMin, deviceContext->XMax,
                deviceContext->YMin, deviceContext->YMax));
            deviceContext->XMin = 0;
            deviceContext->XMax = 1080;
            deviceContext->YMin = 0;
            deviceContext->YMax = 2340;
            status = STATUS_INVALID_PARAMETER;
        }

        WdfRegistryClose(hKey);
    }

    return status;
}

