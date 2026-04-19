// XSBParser.h
// Parses Xbox Sound Bank (.xsb) files to map cue names to wave bank entries.

#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct XSBCue
{
    std::string name;
    int         waveBankIndex; // index into the referenced wave bank
    int         waveIndex;     // index of the wave within that bank
    float       volume;        // 0.0 - 1.0
    bool        isLooping;
};

class XSBParser
{
public:
    bool Load(const wchar_t* path);
    const XSBCue* FindCue(const std::string& name) const;
    int GetCueCount() const { return static_cast<int>(m_cues.size()); }

private:
    std::vector<XSBCue> m_cues;
    std::unordered_map<std::string, int> m_index;
};
