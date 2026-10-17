#pragma once

#include <stdint.h>

#define LTL_PROTOCOL_VERSION    1u
#define LTL_VIDEO_TCP_PORT      27183u
#define LTL_INPUT_TCP_PORT      27184u
#define LTL_MAX_INPUT_POINTERS  10u

#define LTL_MAGIC_0 0x4Cu
#define LTL_MAGIC_1 0x54u

typedef enum LtlPacketType : uint8_t
{
    LtlPacketHello = 0x01,
    LtlPacketConfig = 0x02,
    LtlPacketVideo = 0x03,
    LtlPacketIdrRequest = 0x04,
    LtlPacketInput = 0x05,
    LtlPacketBye = 0x06,
} LtlPacketType;

typedef enum LtlVideoCodec : uint8_t
{
    LtlVideoCodecHevc = 0x01,
    LtlVideoCodecAvc = 0x02,
} LtlVideoCodec;

typedef enum LtlInputDeviceKind : uint8_t
{
    LtlInputDevicePen = 0x01,
    LtlInputDeviceTouch = 0x02,
} LtlInputDeviceKind;

typedef enum LtlPointerAction : uint8_t
{
    LtlActionDown = 0x00,
    LtlActionUp = 0x01,
    LtlActionMove = 0x02,
    LtlActionCancel = 0x03,
    LtlActionPointerDown = 0x05,
    LtlActionPointerUp = 0x06,
    LtlActionHoverMove = 0x07,
    LtlActionHoverEnter = 0x09,
    LtlActionHoverExit = 0x0A,
} LtlPointerAction;

typedef enum LtlPointerState : uint8_t
{
    LtlPointerIdle = 0x00,
    LtlPointerHovering = 0x01,
    LtlPointerContact = 0x02,
} LtlPointerState;

typedef enum LtlPointerButton : uint8_t
{
    LtlButtonNone = 0x00,
    LtlButtonStylusPrimary = 0x01,
    LtlButtonStylusSecondary = 0x02,
} LtlPointerButton;

typedef enum LtlPointerTool : uint8_t
{
    LtlToolUnknown = 0x00,
    LtlToolPen = 0x01,
    LtlToolEraser = 0x02,
    LtlToolFinger = 0x03,
    LtlToolMouse = 0x04,
} LtlPointerTool;

#pragma pack(push, 1)

typedef struct LtlPacketHeader
{
    uint8_t     magic0;
    uint8_t     magic1;
    uint8_t     packetType;
    uint8_t     protocolVersion;
    uint32_t    payloadSize;
} LtlPacketHeader;

typedef struct LtlHelloPayload
{
    uint16_t    maxWidth;
    uint16_t    maxHeight;
    uint8_t     preferredCodec;
    uint8_t     reserved0;
    uint8_t     reserved1;
    uint8_t     reserved2;
} LtlHelloPayload;

typedef struct LtlConfigPayload
{
    uint16_t    width;
    uint16_t    height;
    uint16_t    fps;
    uint16_t    bitrateMbps;
    uint8_t     codec;
    uint8_t     reserved0;
    uint8_t     reserved1;
    uint8_t     reserved2;
} LtlConfigPayload;

typedef struct LtlVideoFrameHeader
{
    uint8_t     keyframe;
    uint8_t     reserved0;
    uint8_t     reserved1;
    uint8_t     reserved2;
} LtlVideoFrameHeader;

typedef struct LtlPointerPayload
{
    float       x;
    float       y;
    float       pressure;
    int16_t     tiltXDegrees;
    int16_t     tiltYDegrees;
    uint8_t     pointerId;
    uint8_t     pointerState;
    uint8_t     buttons;
    uint8_t     tool;
} LtlPointerPayload;

typedef struct LtlInputEventPayload
{
    uint8_t     deviceKind;
    uint8_t     action;
    uint8_t     pointerCount;
    uint8_t     actionIndex;
    LtlPointerPayload pointers[LTL_MAX_INPUT_POINTERS];
} LtlInputEventPayload;

#pragma pack(pop)
