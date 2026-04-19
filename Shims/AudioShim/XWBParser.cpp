#include "XWBParser.h"
#include <fstream>
#include <algorithm>

bool XWBParser::Load(const wchar_t* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    auto fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(fileSize);
    f.read(reinterpret_cast<char*>(buf.data()), fileSize);

    if (fileSize < sizeof(XWBFileHeader)) return false;

    auto* hdr = reinterpret_cast<XWBFileHeader*>(buf.data());
    if (strncmp(hdr->dwSignature, "WBND", 4) != 0 &&
        strncmp(hdr->dwSignature, "DNBW", 4) != 0) // little/big endian variants
        return false;

    auto& bankSeg  = hdr->Segments[XWBSEG_BANKDATA];
    auto& entrySeg = hdr->Segments[XWBSEG_ENTRYMETADATA];
    auto& nameSeg  = hdr->Segments[XWBSEG_ENTRYNAMES];
    auto& waveSeg  = hdr->Segments[XWBSEG_ENTRYWAVEDATA];

    if (bankSeg.dwOffset + sizeof(XWBBankData) > fileSize) return false;
    auto* bank = reinterpret_cast<XWBBankData*>(buf.data() + bankSeg.dwOffset);

    uint32_t entryCount = bank->dwEntryCount;
    uint32_t entrySize  = bank->dwEntryMetaDataElementSize;
    uint32_t nameSize   = bank->dwEntryNameElementSize;

    m_sounds.resize(entryCount);

    for (uint32_t i = 0; i < entryCount; ++i)
    {
        auto& sound = m_sounds[i];

        // Name
        if (nameSeg.dwLength > 0 && nameSize > 0)
        {
            const char* namePtr = reinterpret_cast<const char*>(
                buf.data() + nameSeg.dwOffset + i * nameSize);
            sound.name = std::string(namePtr, strnlen(namePtr, nameSize));
        }
        else
        {
            sound.name = "sound_" + std::to_string(i);
        }

        // Entry metadata
        auto* entry = reinterpret_cast<XWBEntry*>(
            buf.data() + entrySeg.dwOffset + i * entrySize);

        sound.channels    = entry->Format.nChannels;
        sound.sampleRate  = entry->Format.nSamplesPerSec;
        sound.bitsPerSample = entry->Format.wBitsPerSample;
        sound.blockAlign  = entry->Format.nBlockAlign;
        sound.isADPCM     = (entry->Format.wFormatTag == 2); // WAVE_FORMAT_ADPCM

        // Wave data
        uint32_t offset = waveSeg.dwOffset + entry->PlayRegion.dwOffset;
        uint32_t length = entry->PlayRegion.dwLength;
        if (offset + length <= fileSize)
        {
            sound.data.assign(
                buf.data() + offset,
                buf.data() + offset + length);
        }
    }

    return true;
}

const XWBSound* XWBParser::GetSound(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_sounds.size())) return nullptr;
    return &m_sounds[index];
}

const XWBSound* XWBParser::FindSound(const std::string& name) const
{
    for (auto& s : m_sounds)
        if (s.name == name) return &s;
    return nullptr;
}
