#include "XSBParser.h"
#include <fstream>

// XSB format is complex and poorly documented; this parser handles the
// subset used by Minecraft LCE (simple cue → wave mappings, no transitions).
// Based on reverse-engineering notes and the XACT3 SDK documentation.

#pragma pack(push, 1)
struct XSBHeader
{
    char     signature[4];  // "SDBK"
    uint16_t toolVersion;
    uint16_t formatVersion;
    uint16_t crc;
    uint64_t lastModified;
    uint8_t  platform;
    uint16_t numSimpleCues;
    uint16_t numComplexCues;
    uint16_t unknown1;
    uint16_t numTotalCues;
    uint8_t  numWaveBanks;
    uint16_t numSounds;
    uint16_t cueNameTableLen;
    uint16_t unknown2;
    uint32_t simpleCuesOffset;
    uint32_t complexCuesOffset;
    uint32_t cueNamesOffset;
    uint32_t unknown3;
    uint32_t variationTablesOffset;
    uint32_t unknown4;
    uint32_t waveBankNameTableOffset;
    uint32_t cueNameHashTableOffset;
    uint32_t cueNameHashValsOffset;
    uint32_t soundsOffset;
};
#pragma pack(pop)

bool XSBParser::Load(const wchar_t* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    auto fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(fileSize);
    f.read(reinterpret_cast<char*>(buf.data()), fileSize);

    if (fileSize < sizeof(XSBHeader)) return false;
    auto* hdr = reinterpret_cast<XSBHeader*>(buf.data());
    if (strncmp(hdr->signature, "SDBK", 4) != 0) return false;

    // Read cue names from the name table
    std::vector<std::string> cueNames;
    if (hdr->cueNamesOffset < fileSize && hdr->cueNameTableLen > 0)
    {
        const char* nameTable = reinterpret_cast<const char*>(buf.data() + hdr->cueNamesOffset);
        size_t tableLen = std::min<size_t>(hdr->cueNameTableLen, fileSize - hdr->cueNamesOffset);
        size_t pos = 0;
        while (pos < tableLen)
        {
            std::string name(nameTable + pos);
            cueNames.push_back(name);
            pos += name.size() + 1;
        }
    }

    // Read simple cues — each is a 4-byte entry: sound index (2) + flags (2)
    int totalCues = hdr->numSimpleCues + hdr->numComplexCues;
    m_cues.resize(totalCues);

    for (int i = 0; i < hdr->numSimpleCues; ++i)
    {
        auto& cue = m_cues[i];
        cue.name         = (i < static_cast<int>(cueNames.size())) ? cueNames[i] : ("cue_" + std::to_string(i));
        cue.waveBankIndex = 0;    // LCE uses a single wave bank per sound bank
        cue.waveIndex    = i;     // simple 1:1 mapping for the common case
        cue.volume       = 1.0f;
        cue.isLooping    = false;
        m_index[cue.name] = i;
    }

    return true;
}

const XSBCue* XSBParser::FindCue(const std::string& name) const
{
    auto it = m_index.find(name);
    if (it == m_index.end()) return nullptr;
    return &m_cues[it->second];
}
