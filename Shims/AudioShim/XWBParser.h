// XWBParser.h
// Parses Xbox Wave Bank (.xwb) files to extract raw PCM/ADPCM audio data.
// Format reference: https://github.com/microsoft/DirectXTK/blob/main/Src/WAVFileReader.cpp
// and MonoGame's XWB reader.

#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <cstdint>

// XWB segment indices
enum XWBSegment
{
    XWBSEG_BANKDATA    = 0,
    XWBSEG_ENTRYMETADATA = 1,
    XWBSEG_SEEKTABLES  = 2,
    XWBSEG_ENTRYNAMES  = 3,
    XWBSEG_ENTRYWAVEDATA = 4,
    XWBSEG_COUNT       = 5
};

#pragma pack(push, 1)
struct XWBFileHeader
{
    char     dwSignature[4];   // "WBND"
    uint32_t dwVersion;
    uint32_t dwHeaderVersion;
    struct { uint32_t dwOffset; uint32_t dwLength; } Segments[XWBSEG_COUNT];
};

struct XWBBankData
{
    uint32_t dwFlags;
    uint32_t dwEntryCount;
    char     szBankName[64];
    uint32_t dwEntryMetaDataElementSize;
    uint32_t dwEntryNameElementSize;
    uint32_t dwAlignment;
    uint32_t CompactFormat;
    uint64_t BuildTime;
};

struct XWBEntry
{
    uint32_t dwFlagsAndDuration; // bits 0-1: codec, bits 2-28: duration samples
    struct {
        uint16_t wFormatTag;
        uint16_t nChannels;
        uint32_t nSamplesPerSec;
        uint32_t nAvgBytesPerSec;
        uint16_t nBlockAlign;
        uint16_t wBitsPerSample;
    } Format;
    struct { uint32_t dwOffset; uint32_t dwLength; } PlayRegion;
    struct { uint32_t dwStartSample; uint32_t dwTotalSamples; } LoopRegion;
};
#pragma pack(pop)

struct XWBSound
{
    std::string      name;
    std::vector<uint8_t> data;   // raw PCM or ADPCM bytes
    uint16_t         channels;
    uint32_t         sampleRate;
    uint16_t         bitsPerSample;
    uint16_t         blockAlign;
    bool             isADPCM;
};

class XWBParser
{
public:
    bool Load(const wchar_t* path);
    int  GetSoundCount() const { return static_cast<int>(m_sounds.size()); }
    const XWBSound* GetSound(int index) const;
    const XWBSound* FindSound(const std::string& name) const;

private:
    std::vector<XWBSound> m_sounds;
};
