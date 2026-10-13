#pragma once

#include <cstdint>

enum MsgType : uint32_t {
    MSG_PING=0x504E4750, MSG_FPS_SET=0x46505343, MSG_HOST_INFO=0x484F5354, MSG_FPS_ACK=0x46505341,
    MSG_REQUEST_KEY=0x4B455952, MSG_MONITOR_LIST=0x4D4F4E4C, MSG_MONITOR_SET=0x4D4F4E53,
    MSG_AUDIO_DATA=0x41554449, MSG_MOUSE_MOVE=0x4D4F5645, MSG_MOUSE_BTN=0x4D42544E,
    MSG_MOUSE_WHEEL=0x4D57484C, MSG_KEY=0x4B455920, MSG_CODEC_SET=0x434F4443, MSG_CODEC_ACK=0x434F4441,
    MSG_CODEC_CAPS=0x434F4350, MSG_MOUSE_MOVE_REL=0x4D4F5652, MSG_CLIPBOARD_DATA=0x434C4950,
    MSG_CLIPBOARD_GET=0x434C4754, MSG_KICKED=0x4B49434B, MSG_CURSOR_CAPTURE=0x43555243,
    MSG_CURSOR_SHAPE=0x43555253, MSG_AUDIO_ENABLE=0x41554445, MSG_MIC_DATA=0x4D494344,
    MSG_MIC_ENABLE=0x4D494345, MSG_ENCODER_INFO=0x49434E45, MSG_VERSION=0x56455253,
    MSG_STREAM_TARGET=0x56505254, MSG_DESKTOP_STATE=0x44534B54,
    // Instrumentation: client->host input-channel latency probe and host echo.
    MSG_INPUT_PING=0x49504E47, MSG_INPUT_PONG=0x49504F47
};

enum CodecType : uint8_t { CODEC_AV1=0, CODEC_H265=1, CODEC_H264=2 };
enum PacketType : uint8_t { PKT_DATA=0, PKT_FEC=1 };

// Wire protocol revision. Bump on any change an older client would misread; /api/offer rejects
// clients that don't send the matching value. History: 1 = untagged frameType (0/1),
// 2 = codec + encoder-generation tags in frameType.
constexpr int kProtocolVersion = 2;

// Video PacketHeader::frameType layout:
//   bit 0    keyframe
//   bits 1-3 encoder generation (mod 8): changes whenever a new encoder instance is installed,
//            so the client can tell a new encoder's frames from the old one's even with one codec
//   bits 4-6 codec + 1 (0 = untagged)
constexpr uint8_t kFrameTypeKeyBit = 0x01;
constexpr uint8_t kFrameTypeGenerationShift = 1;
constexpr uint8_t kFrameTypeCodecShift = 4;
constexpr uint8_t MakeVideoFrameType(bool isKey, CodecType codec, uint8_t generation) {
    return static_cast<uint8_t>((isKey ? kFrameTypeKeyBit : 0) |
        ((generation & 0x7) << kFrameTypeGenerationShift) |
        ((static_cast<uint8_t>(codec) + 1) << kFrameTypeCodecShift));
}

// MSG_DESKTOP_STATE payload byte.
enum DesktopState : uint8_t { DESKTOP_NORMAL=0, DESKTOP_SECURE=1 };

enum CursorType : uint8_t {
    CURSOR_DEFAULT=0, CURSOR_TEXT, CURSOR_POINTER, CURSOR_WAIT, CURSOR_PROGRESS, CURSOR_CROSSHAIR,
    CURSOR_MOVE, CURSOR_EW_RESIZE, CURSOR_NS_RESIZE, CURSOR_NWSE_RESIZE, CURSOR_NESW_RESIZE,
    CURSOR_NOT_ALLOWED, CURSOR_HELP, CURSOR_NONE, CURSOR_CUSTOM=255
};

#pragma pack(push,1)
struct MediaPacketHeader {
    uint32_t magic;
    int64_t timestamp;
    uint32_t packetId;
    uint16_t samples;
    uint16_t dataLength;
    uint8_t packetType;
    uint8_t fecGroupSize;
    uint16_t reserved;
};
using AudioPacketHeader = MediaPacketHeader;
using MicPacketHeader = MediaPacketHeader;
#pragma pack(pop)
