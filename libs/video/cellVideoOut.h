/*
 * ps3recomp - cellVideoOut HLE
 *
 * Video output configuration: resolution, display mode, device info.
 */

#ifndef PS3RECOMP_CELL_VIDEOOUT_H
#define PS3RECOMP_CELL_VIDEOOUT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_VIDEO_OUT_PRIMARY      0
#define CELL_VIDEO_OUT_SECONDARY    1

/* Resolution IDs */
#define CELL_VIDEO_OUT_RESOLUTION_UNDEFINED  0
#define CELL_VIDEO_OUT_RESOLUTION_1080       1
#define CELL_VIDEO_OUT_RESOLUTION_720        2
#define CELL_VIDEO_OUT_RESOLUTION_480        4
#define CELL_VIDEO_OUT_RESOLUTION_576        5
#define CELL_VIDEO_OUT_RESOLUTION_1600x1080  10
#define CELL_VIDEO_OUT_RESOLUTION_1440x1080  11
#define CELL_VIDEO_OUT_RESOLUTION_1280x1080  12
#define CELL_VIDEO_OUT_RESOLUTION_960x1080   13

/* Scan mode */
#define CELL_VIDEO_OUT_SCAN_MODE_INTERLACE   0
#define CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE 1

/* Aspect ratio */
#define CELL_VIDEO_OUT_ASPECT_AUTO   0
#define CELL_VIDEO_OUT_ASPECT_4_3    1
#define CELL_VIDEO_OUT_ASPECT_16_9   2

/* Output port */
#define CELL_VIDEO_OUT_OUTPUT_HDMI   1

/* Color space */
#define CELL_VIDEO_OUT_COLOR_SPACE_RGB   0x01
#define CELL_VIDEO_OUT_COLOR_SPACE_YUV   0x02

/* Buffer color format */
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8  0
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8  1
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_R16G16B16X16_FLOAT  2

/* Refresh-rate flags */
#define CELL_VIDEO_OUT_REFRESH_RATE_AUTO       0x0000
#define CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ    0x0001
#define CELL_VIDEO_OUT_REFRESH_RATE_50HZ       0x0002
#define CELL_VIDEO_OUT_REFRESH_RATE_60HZ       0x0004
#define CELL_VIDEO_OUT_REFRESH_RATE_30HZ       0x0008

/* Output state */
#define CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED    0
#define CELL_VIDEO_OUT_OUTPUT_STATE_DISABLED   1
#define CELL_VIDEO_OUT_OUTPUT_STATE_PREPARING  2

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_VIDEO_OUT_ERROR_NOT_IMPLEMENTED          ((s32)0x8002B220)
#define CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION    ((s32)0x8002B221)
#define CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER        ((s32)0x8002B222)
#define CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE   ((s32)0x8002B223)
#define CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND         ((s32)0x8002B224)
#define CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT    ((s32)0x8002B225)
#define CELL_VIDEO_OUT_ERROR_UNSUPPORTED_DISPLAY_MODE ((s32)0x8002B226)
#define CELL_VIDEO_OUT_ERROR_CONDITION_BUSY           ((s32)0x8002B227)

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellVideoOutResolution {
    u16 width;
    u16 height;
} CellVideoOutResolution;

typedef struct CellVideoOutConfiguration {
    u8  resolutionId;
    u8  format;
    u8  aspect;
    u8  reserved[9];
    u32 pitch;
} CellVideoOutConfiguration;

typedef struct CellVideoOutDisplayMode {
    u8  resolutionId;
    u8  scanMode;
    u8  conversion;
    u8  aspect;
    u8  reserved[2];
    u16 refreshRates;
} CellVideoOutDisplayMode;

typedef struct CellVideoOutState {
    u8  state;
    u8  colorSpace;
    u8  reserved[6];
    CellVideoOutDisplayMode displayMode;
} CellVideoOutState;

typedef struct CellVideoOutDeviceInfo {
    u8  portType;
    u8  colorSpace;
    u16 latency;
    u8  availableModeCount;
    u8  state;
    u8  rgbOutputRange;
    u8  reserved[5];
    CellVideoOutDisplayMode availableModes[32];
} CellVideoOutDeviceInfo;

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

/* Set the default resolution (call before game boots) */
void cellVideoOut_set_resolution(u8 resolutionId);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellVideoOutGetState(u32 videoOut, u32 deviceIndex, CellVideoOutState* state);

s32 cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* resolution);

s32 cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config,
                            void* option, u32 waitForEvent);

s32 cellVideoOutGetConfiguration(u32 videoOut, CellVideoOutConfiguration* config,
                                   void* option);

s32 cellVideoOutGetDeviceInfo(u32 videoOut, u32 deviceIndex,
                               CellVideoOutDeviceInfo* info);

s32 cellVideoOutGetNumberOfDevice(u32 videoOut);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VIDEOOUT_H */
