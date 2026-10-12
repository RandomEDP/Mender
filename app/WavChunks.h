#pragma once

#include <juce_core/juce_core.h>

// Small helpers for WAV chunks that JUCE does not keep: the full iXML chunk
// written by field recorders (track names and other production metadata).
namespace wavchunks
{
// The contents of the first chunk with this four-character id, or empty.
juce::MemoryBlock read (const juce::File& wav, const char* id);

// Adds a chunk at the end of a RIFF WAV file. Returns false for RF64 (> 4 GB)
// files or on error.
bool append (const juce::File& wav, const char* id, const juce::MemoryBlock& data);

// Channel names from an iXML TRACK_LIST, in file order. Missing names are empty.
juce::StringArray trackNames (const juce::String& ixml, int numChannels);
} // namespace wavchunks
