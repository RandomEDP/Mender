#include "WavChunks.h"

namespace wavchunks
{
juce::MemoryBlock read (const juce::File& wav, const char* id)
{
    juce::FileInputStream in (wav);
    if (! in.openedOk())
        return {};

    char riff[4], wave[4];
    in.read (riff, 4);
    in.readInt();
    in.read (wave, 4);
    const bool isRf64 = memcmp (riff, "RF64", 4) == 0;
    if ((memcmp (riff, "RIFF", 4) != 0 && ! isRf64) || memcmp (wave, "WAVE", 4) != 0)
        return {};

    juce::int64 rf64DataSize = -1;
    while (! in.isExhausted())
    {
        char chunk[4];
        if (in.read (chunk, 4) != 4)
            break;

        juce::int64 size = (juce::uint32) in.readInt();
        const auto start = in.getPosition();

        if (memcmp (chunk, "ds64", 4) == 0)
        {
            in.readInt64();                // RIFF size
            rf64DataSize = in.readInt64(); // data size
        }
        else if (memcmp (chunk, "data", 4) == 0 && isRf64 && size == 0xffffffff)
        {
            size = rf64DataSize;
        }

        if (memcmp (chunk, id, 4) == 0)
        {
            juce::MemoryBlock data;
            in.readIntoMemoryBlock (data, (juce::pointer_sized_int) size);
            return data;
        }

        if (size < 0 || ! in.setPosition (start + size + (size & 1)))
            break;
    }

    return {};
}

bool append (const juce::File& wav, const char* id, const juce::MemoryBlock& data)
{
    {
        juce::FileInputStream in (wav);
        char riff[4];
        if (! in.openedOk() || in.read (riff, 4) != 4 || memcmp (riff, "RIFF", 4) != 0)
            return false;
    }

    const auto padded = (juce::int64) data.getSize() + (juce::int64) (data.getSize() & 1);
    const auto newRiffSize = wav.getSize() - 8 + 8 + padded;
    if (newRiffSize > 0xffffffffLL)
        return false;

    juce::FileOutputStream out (wav); // opens at the end of the file
    if (! out.openedOk())
        return false;

    if (wav.getSize() & 1)
        out.writeByte (0);
    out.write (id, 4);
    out.writeInt ((int) data.getSize());
    out.write (data.getData(), data.getSize());
    if (data.getSize() & 1)
        out.writeByte (0);
    out.flush();

    // Patch the RIFF size in the header.
    if (! out.setPosition (4))
        return false;
    out.writeInt ((int) (juce::uint32) (out.getFile().getSize() - 8));
    out.flush();
    return out.getStatus().wasOk();
}

juce::StringArray trackNames (const juce::String& ixml, int numChannels)
{
    juce::StringArray names;
    for (int i = 0; i < numChannels; ++i)
        names.add ({});

    const auto xml = juce::parseXML (ixml);
    if (xml == nullptr)
        return names;

    if (const auto* list = xml->getChildByName ("TRACK_LIST"))
    {
        for (const auto* track : list->getChildWithTagNameIterator ("TRACK"))
        {
            // INTERLEAVE_INDEX is the position in the file; CHANNEL_INDEX is the recorder input.
            auto index = track->getChildElementAllSubText ("INTERLEAVE_INDEX", {}).getIntValue();
            if (index <= 0)
                index = track->getChildElementAllSubText ("CHANNEL_INDEX", {}).getIntValue();

            if (index >= 1 && index <= numChannels)
                names.set (index - 1, track->getChildElementAllSubText ("NAME", {}).trim());
        }
    }

    return names;
}
} // namespace wavchunks
