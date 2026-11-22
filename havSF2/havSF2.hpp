/*
havSF2.hpp

ABOUT

Havoc's single-file SoundFont 2 (.sf2) loader for C++.

It turns a file into presets (one per instrument of the sound set), each a
list of regions: which sample to play for which keys and velocities, and
with which settings. Every setting of a region, generators and modulators
alike, is resolved, so nothing has to be looked up again to play it. 24-bit
sample data is read where the file has it. Playing it is left to your
synthesizer.

REVISION HISTORY

v0.1.0 (2026-11-22)
- First release.

LICENSE

MIT License

Copyright (c) 2026 René Nicolaus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef HAVSF2_HPP
#define HAVSF2_HPP

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <istream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A SoundFont file is a RIFF file of three lists:
//
//   INFO  what the file is: version, name, author
//   sdta  the sample data: one long run of 16-bit points
//   pdta  nine tables that say how the samples make up instruments and
//         the instruments presets
//
// The tables refer to each other by index: a preset to its zones, a zone
// to its generators, the last generator of a zone to an instrument, and
// the same again from instruments down to samples. The reader checks every
// such reference before it follows it, and then flattens the whole
// structure into regions, so that nothing has to be looked up again.
//
// What is read and what is not is written down in docs/overview.md. Section
// numbers in the comments are those of the SoundFont 2.04 specification.

namespace havSF2
{
  inline constexpr std::uint32_t VersionMajor = 0;
  inline constexpr std::uint32_t VersionMinor = 1;
  inline constexpr std::uint32_t VersionPatch = 0;
  inline constexpr std::string_view VersionString = "0.1.0";

  // The settings of a region, named and numbered as in the SoundFont
  // specification. Their units are the specification's: timecents,
  // centibels, cents, tenths of a percent.
  enum class Generator : std::uint16_t
  {
    StartAddrsOffset = 0,
    EndAddrsOffset = 1,
    StartloopAddrsOffset = 2,
    EndloopAddrsOffset = 3,
    StartAddrsCoarseOffset = 4,
    ModLfoToPitch = 5,
    VibLfoToPitch = 6,
    ModEnvToPitch = 7,
    InitialFilterFc = 8,
    InitialFilterQ = 9,
    ModLfoToFilterFc = 10,
    ModEnvToFilterFc = 11,
    EndAddrsCoarseOffset = 12,
    ModLfoToVolume = 13,
    ChorusEffectsSend = 15,
    ReverbEffectsSend = 16,
    Pan = 17,
    DelayModLfo = 21,
    FreqModLfo = 22,
    DelayVibLfo = 23,
    FreqVibLfo = 24,
    DelayModEnv = 25,
    AttackModEnv = 26,
    HoldModEnv = 27,
    DecayModEnv = 28,
    SustainModEnv = 29,
    ReleaseModEnv = 30,
    KeynumToModEnvHold = 31,
    KeynumToModEnvDecay = 32,
    DelayVolEnv = 33,
    AttackVolEnv = 34,
    HoldVolEnv = 35,
    DecayVolEnv = 36,
    SustainVolEnv = 37,
    ReleaseVolEnv = 38,
    KeynumToVolEnvHold = 39,
    KeynumToVolEnvDecay = 40,
    Instrument = 41,
    KeyRange = 43,
    VelRange = 44,
    StartloopAddrsCoarseOffset = 45,
    Keynum = 46,
    Velocity = 47,
    InitialAttenuation = 48,
    EndloopAddrsCoarseOffset = 50,
    CoarseTune = 51,
    FineTune = 52,
    SampleId = 53,
    SampleModes = 54,
    ScaleTuning = 56,
    ExclusiveClass = 57,
    OverridingRootKey = 58,
  };

  inline constexpr std::size_t GeneratorCount = 61;

  // The MIDI bank that holds the percussion kits of a SoundFont
  inline constexpr std::uint16_t PercussionBank = 128;

  // The key a sample is taken to be recorded at if the file does not say
  inline constexpr std::uint8_t DefaultRootKey = 60;

  // The value a generator has when no zone sets it
  [[nodiscard]] inline std::int16_t DefaultValue(Generator generator) noexcept
  {
    switch (generator)
    {
    // An open filter: 13500 cents is 20 kHz
    case Generator::InitialFilterFc:
      return 13500;

    // The shortest time there is about a millisecond: -12000 timecents
    case Generator::DelayModLfo:
    case Generator::DelayVibLfo:
    case Generator::DelayModEnv:
    case Generator::AttackModEnv:
    case Generator::HoldModEnv:
    case Generator::DecayModEnv:
    case Generator::ReleaseModEnv:
    case Generator::DelayVolEnv:
    case Generator::AttackVolEnv:
    case Generator::HoldVolEnv:
    case Generator::DecayVolEnv:
    case Generator::ReleaseVolEnv:
      return -12000;

    // "Not set": the key and velocity that were played are used
    case Generator::Keynum:
    case Generator::Velocity:
    case Generator::OverridingRootKey:
      return -1;

    // A semitone per key
    case Generator::ScaleTuning:
      return 100;

    // Every key and every velocity
    case Generator::KeyRange:
    case Generator::VelRange:
      return 0x7F00;

    default:
      return 0;
    }
  }

  // UNITS
  // -----
  // Conversions from the specification's units to the ones a synthesizer
  // computes with.

  // A time in timecents (1200 per doubling) in seconds: 0 is one second,
  // -12000 about a millisecond. The specification treats -32768 as no time
  // at all, so that is 0.
  [[nodiscard]] inline double TimecentsToSeconds(int timecents) noexcept
  {
    return timecents <= -32768 ? 0.0 : std::exp2(timecents / 1200.0);
  }

  // An attenuation in centibels (positive is quieter) as the factor to
  // multiply an amplitude by: 0 is 1, 60 about a half, 200 a tenth.
  [[nodiscard]] inline double CentibelsToGain(double centibels) noexcept
  {
    return std::pow(10.0, -centibels / 200.0);
  }

  // A pitch change in cents (100 per semitone) as the factor to multiply a
  // frequency or playback rate by: 1200 is 2.
  [[nodiscard]] inline double CentsToRatio(double cents) noexcept
  {
    return std::exp2(cents / 1200.0);
  }

  // MODULATORS
  // ----------
  // A modulator routes a controller (the note's velocity, the mod wheel, the
  // pitch wheel, ...) to a generator: the generator's value moves by
  // amount * source * amount source, as the specification's chapter 9.5
  // describes. Sources and the transform are kept as the file stores them,
  // and DecodeSource takes a source apart.

  // The controllers of the general palette (a source whose midiController
  // bit is clear). With the bit set, the index is a MIDI controller number.
  enum class Controller : std::uint8_t
  {
    NoController = 0, // the source counts as 1
    NoteOnVelocity = 2,
    NoteOnKey = 3,
    PolyPressure = 10,
    ChannelPressure = 13,
    PitchWheel = 14,
    PitchWheelSensitivity = 16,
    Link = 127, // the output of another modulator
  };

  // How a source moves from its minimum to its maximum
  enum class SourceType : std::uint8_t
  {
    Linear = 0,
    Concave = 1,
    Convex = 2,
    Switch = 3,
  };

  enum class Transform : std::uint16_t
  {
    Linear = 0,
    AbsoluteValue = 2,
  };

  // A source operand taken apart: bits 0-6 index, 7 palette, 8 direction,
  // 9 polarity, 10-15 type
  struct Source
  {
    std::uint8_t index = 0;      // a Controller, or a MIDI controller number
    bool midiController = false; // which palette the index is from
    bool negative = false;       // runs from maximum to minimum
    bool bipolar = false;        // -1..1 instead of 0..1
    SourceType type = SourceType::Linear;

    friend bool operator==(const Source &, const Source &) = default;
  };

  [[nodiscard]] constexpr Source DecodeSource(std::uint16_t source) noexcept
  {
    return Source{static_cast<std::uint8_t>(source & 0x7F), (source & 0x80) != 0,
                  (source & 0x100) != 0, (source & 0x200) != 0,
                  static_cast<SourceType>(source >> 10)};
  }

  // The destination of the default pitch wheel modulator. The specification
  // calls it "Initial Pitch" and gives it no generator number. 59 is the
  // number after the last generator, unused by the format.
  inline constexpr std::uint16_t InitialPitch = 59;

  struct Modulator
  {
    std::uint16_t source = 0;       // an encoded source operand
    std::uint16_t destination = 0;  // a Generator, or InitialPitch
    std::int16_t amount = 0;        // in the destination's units
    std::uint16_t amountSource = 0; // an encoded source operand that scales the amount
    std::uint16_t transform = 0;    // a Transform

    // The specification's "identical": everything but the amount. An
    // identical modulator further down the hierarchy replaces (instrument
    // level) or adds its amount to (preset level) this one.
    [[nodiscard]] constexpr bool IsIdenticalTo(const Modulator &other) const noexcept
    {
      return source == other.source && destination == other.destination &&
             amountSource == other.amountSource && transform == other.transform;
    }

    friend bool operator==(const Modulator &, const Modulator &) = default;
  };

  // The modulators every instrument zone has unless the file replaces them
  // (specification 8.4). Two values differ from the specification's table,
  // which misprints them: controller 7 is source 0x0587 (printed 0x0582,
  // which is controller 2, while the text says index 7), and controller 10
  // goes to Pan (printed as Initial Attenuation, while the text says pan).
  inline constexpr std::array<Modulator, 10> DefaultModulators{{
      {0x0502, static_cast<std::uint16_t>(Generator::InitialAttenuation), 960, 0,
       0}, // velocity, concave, falling
      {0x0102, static_cast<std::uint16_t>(Generator::InitialFilterFc), -2400, 0,
       0}, // velocity, linear, falling
      {0x000D, static_cast<std::uint16_t>(Generator::VibLfoToPitch), 50, 0, 0}, // channel pressure
      {0x0081, static_cast<std::uint16_t>(Generator::VibLfoToPitch), 50, 0,
       0}, // controller 1, the mod wheel
      {0x0587, static_cast<std::uint16_t>(Generator::InitialAttenuation), 960, 0,
       0},                                                              // controller 7, volume
      {0x028A, static_cast<std::uint16_t>(Generator::Pan), 1000, 0, 0}, // controller 10, pan
      {0x058B, static_cast<std::uint16_t>(Generator::InitialAttenuation), 960, 0,
       0}, // controller 11, expression
      {0x00DB, static_cast<std::uint16_t>(Generator::ReverbEffectsSend), 200, 0,
       0}, // controller 91, reverb
      {0x00DD, static_cast<std::uint16_t>(Generator::ChorusEffectsSend), 200, 0,
       0},                                      // controller 93, chorus
      {0x020E, InitialPitch, 12700, 0x0010, 0}, // pitch wheel, scaled by pitch wheel sensitivity
  }};

  // One recording. Positions are indices into SoundFont::data.
  struct Sample
  {
    std::string name;
    std::uint32_t start = 0;     // first sample point
    std::uint32_t end = 0;       // one past the last
    std::uint32_t loopStart = 0; // first point of the loop
    std::uint32_t loopEnd = 0;   // one past its last: playback jumps from here to loopStart
    std::uint32_t sampleRate = 44100;
    std::uint8_t originalKey = DefaultRootKey; // the MIDI key the recording sounds at
    std::int8_t pitchCorrection = 0;           // cents to add to that
    bool isRom = false; // its data is in a sound card's memory, not in the file

    friend bool operator==(const Sample &, const Sample &) = default;
  };

  // One sample of a preset and when and how to play it. Every setting is
  // resolved: the instrument's zone over the instrument's global zone over
  // the default, plus what the preset adds. Nothing further has to be
  // looked up to play it.
  struct Region
  {
    std::uint8_t lowKey = 0;
    std::uint8_t highKey = 127;
    std::uint8_t lowVelocity = 0;
    std::uint8_t highVelocity = 127;

    std::uint32_t sample = 0;     // index into SoundFont::samples
    std::uint32_t instrument = 0; // index into SoundFont::instrumentNames for display

    // Where to play in SoundFont::data with the zone's offsets applied.
    // loopStart == loopEnd means the region does not loop, whatever
    // SampleModes says.
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t loopStart = 0;
    std::uint32_t loopEnd = 0;

    // The key at which the sample plays at its recorded pitch
    std::uint8_t rootKey = DefaultRootKey;

    std::array<std::int16_t, GeneratorCount> generators{};

    // The modulators that apply, resolved like the generators: the default
    // modulators, replaced by identical ones of the instrument's global zone
    // and then of its zone, plus the preset's (its zone's over its global
    // zone's), whose amounts add to identical ones. In that order: the
    // defaults first, then what each level adds.
    std::vector<Modulator> modulators;

    [[nodiscard]] std::int16_t Value(Generator generator) const noexcept
    {
      return generators[static_cast<std::size_t>(generator)];
    }

    [[nodiscard]] bool Covers(std::uint8_t key, std::uint8_t velocity) const noexcept
    {
      return key >= lowKey && key <= highKey && velocity >= lowVelocity && velocity <= highVelocity;
    }

    friend bool operator==(const Region &, const Region &) = default;
  };

  // One instrument of the sound set, selected by a MIDI bank and program.
  struct Preset
  {
    std::string name;
    std::uint16_t bank = 0;    // 128 for percussion kits
    std::uint16_t program = 0; // 0-based
    std::vector<Region> regions;

    friend bool operator==(const Preset &, const Preset &) = default;
  };

  struct Info
  {
    std::uint16_t versionMajor = 0;
    std::uint16_t versionMinor = 0;
    std::string name;      // INAM
    std::string engine;    // isng
    std::string date;      // ICRD
    std::string author;    // IENG
    std::string product;   // IPRD
    std::string copyright; // ICOP
    std::string comment;   // ICMT
    std::string software;  // ISFT

    friend bool operator==(const Info &, const Info &) = default;
  };

  struct SoundFont
  {
    Info info;
    std::vector<Sample> samples;
    std::vector<std::int16_t> data; // every sample's points, 16-bit mono

    // For a 24-bit file, the low 8 bits of every point of `data`: the
    // 24-bit point i is data[i] * 256 + lowBytes[i]. Empty for a 16-bit
    // file, and when the file's 24-bit data is unusable (see `notes`).
    std::vector<std::uint8_t> lowBytes;

    std::vector<std::string> instrumentNames; // the building blocks the presets were made from
    std::vector<Preset> presets;              // by bank, then program

    // What the reader left out and why, one line each: zones that could not
    // be used, parts of the format it does not use. Empty for a file that
    // was used in full.
    std::vector<std::string> notes;

    // The preset for a bank and program, or nullptr
    [[nodiscard]] const Preset *FindPreset(std::uint16_t bank, std::uint16_t program) const noexcept
    {
      const auto found =
          std::find_if(presets.begin(), presets.end(), [bank, program](const Preset &preset)
      { return preset.bank == bank && preset.program == program; });

      return found == presets.end() ? nullptr : &*found;
    }

    // The regions of a preset that play for a key struck at a velocity,
    // both 0..127.
    [[nodiscard]] std::vector<const Region *> FindRegions(std::uint16_t bank,
                                                          std::uint16_t program,
                                                          std::uint8_t key,
                                                          std::uint8_t velocity) const
    {
      std::vector<const Region *> matches;

      if (const Preset *preset = FindPreset(bank, program))
      {
        for (const Region &region : preset->regions)
        {
          if (region.Covers(key, velocity))
          {
            matches.push_back(&region);
          }
        }
      }

      return matches;
    }
  };

  // A message fit to show a user naming the file
  struct Error
  {
    std::string message;
  };

  namespace detail
  {
    using Bytes = std::span<const std::uint8_t>;
    using Id = std::array<char, 4>;

    inline constexpr std::size_t PresetHeaderSize = 38;
    inline constexpr std::size_t InstrumentHeaderSize = 22;
    inline constexpr std::size_t SampleHeaderSize = 46;
    inline constexpr std::size_t BagSize = 4;
    inline constexpr std::size_t ModulatorSize = 10;
    inline constexpr std::size_t GeneratorSize = 4;
    inline constexpr std::size_t NameSize = 20;

    // The INFO list and the tables are read into memory whole. Even the
    // largest sound sets keep them to a few megabytes, so a size far beyond
    // that is a damaged length field, not a table.
    inline constexpr std::uint64_t MaxTableBytes = 256ull * 1024 * 1024;

    inline constexpr std::uint16_t RomSampleFlag = 0x8000;
    inline constexpr int CoarseOffsetUnit = 32768;

    inline std::unexpected<Error> Fail(std::string_view name, std::string_view what)
    {
      return std::unexpected(Error{std::format("{}: {}", name, what)});
    }

    inline std::uint16_t U16(Bytes bytes, std::size_t at)
    {
      return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
    }

    inline std::uint32_t U32(Bytes bytes, std::size_t at)
    {
      return static_cast<std::uint32_t>(bytes[at]) |
             (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
             (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
             (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
    }

    inline Id IdAt(Bytes bytes, std::size_t at)
    {
      return Id{static_cast<char>(bytes[at]), static_cast<char>(bytes[at + 1]),
                static_cast<char>(bytes[at + 2]), static_cast<char>(bytes[at + 3])};
    }

    inline bool Is(const Id &id, std::string_view text)
    {
      return std::string_view(id.data(), id.size()) == text;
    }

    // Text of a fixed-size field or a string chunk: up to the first zero
    // byte. The format has no character set, so anything that is not
    // printable ASCII is shown as a question mark.
    inline std::string Text(Bytes bytes)
    {
      std::string text;

      for (const std::uint8_t byte : bytes)
      {
        if (byte == 0)
        {
          break;
        }

        text.push_back(byte >= 0x20 && byte < 0x7F ? static_cast<char>(byte) : '?');
      }

      while (!text.empty() && text.back() == ' ')
      {
        text.pop_back();
      }

      return text;
    }

    inline bool ReadExact(std::istream &input, std::uint8_t *to, std::size_t count)
    {
      input.read(reinterpret_cast<char *>(to), static_cast<std::streamsize>(count));

      return static_cast<std::size_t>(input.gcount()) == count;
    }

    struct Chunk
    {
      Id id;
      Bytes body;
    };

    // The chunks a list consists of. False if one of them runs past the
    // end of the list or bytes are left over that are no chunk.
    inline bool SplitChunks(Bytes list, std::vector<Chunk> &chunks)
    {
      std::size_t at = 0;

      while (list.size() - at >= 8)
      {
        const Id id = IdAt(list, at);
        const std::size_t size = U32(list, at + 4);
        at += 8;

        if (size > list.size() - at)
        {
          return false;
        }

        chunks.push_back(Chunk{id, list.subspan(at, size)});

        // Chunks are padded to an even length, but the last one may lack the byte.
        at = std::min(at + size + (size & 1), list.size());
      }

      return at == list.size();
    }

    inline const Chunk *Find(const std::vector<Chunk> &chunks, std::string_view id)
    {
      const auto found = std::find_if(chunks.begin(), chunks.end(),
                                      [id](const Chunk &chunk) { return Is(chunk.id, id); });

      return found == chunks.end() ? nullptr : &*found;
    }

    // ZONES
    // -----

    // The generators of one zone as they stand in the file
    struct Zone
    {
      std::array<std::int16_t, GeneratorCount> values{};
      std::bitset<GeneratorCount> has;
      std::optional<std::uint16_t>
          target; // the instrument or sample the zone ends on (a zone without one is global)
      std::uint8_t lowKey = 0;
      std::uint8_t highKey = 127;
      std::uint8_t lowVelocity = 0;
      std::uint8_t highVelocity = 127;
      bool hasKeyRange = false;
      bool hasVelocityRange = false;
      std::vector<Modulator> modulators; // the usable ones, no two identical
    };

    // Reads the generators [first, last) of a table. `terminal` is the
    // generator that ends a zone: Instrument in a preset, SampleId in an
    // instrument. Whatever follows it in the zone is ignored, as the format
    // asks.
    inline Zone ReadZone(Bytes generators, std::size_t first, std::size_t last, Generator terminal)
    {
      Zone zone;

      for (std::size_t index = first; index < last; ++index)
      {
        const std::size_t at = index * GeneratorSize;
        const std::uint16_t generator = U16(generators, at);
        const std::uint8_t low = generators[at + 2];
        const std::uint8_t high = generators[at + 3];

        if (generator == static_cast<std::uint16_t>(Generator::KeyRange))
        {
          // Some files have the two ends the wrong way round
          zone.lowKey = std::min<std::uint8_t>(std::min(low, high), 127);
          zone.highKey = std::min<std::uint8_t>(std::max(low, high), 127);
          zone.hasKeyRange = true;
        }
        else if (generator == static_cast<std::uint16_t>(Generator::VelRange))
        {
          zone.lowVelocity = std::min<std::uint8_t>(std::min(low, high), 127);
          zone.highVelocity = std::min<std::uint8_t>(std::max(low, high), 127);
          zone.hasVelocityRange = true;
        }
        else if (generator == static_cast<std::uint16_t>(terminal))
        {
          zone.target = U16(generators, at + 2);
          break;
        }
        else if (generator < GeneratorCount &&
                 generator != static_cast<std::uint16_t>(Generator::Instrument) &&
                 generator != static_cast<std::uint16_t>(Generator::SampleId))
        {
          zone.values[generator] = static_cast<std::int16_t>(U16(generators, at + 2));
          zone.has.set(generator);
        }
      }

      return zone;
    }

    // An instrument or a preset as the file describes it: a global zone
    // with defaults for the others, and the zones that end on a target.
    struct ZoneList
    {
      std::optional<Zone> global;
      std::vector<Zone> zones;
    };

    // Whether a preset may add to an instrument's value of a generator.
    // The sample addresses, the loop mode, and the fixed key and velocity
    // belong to the instrument alone.
    inline bool IsAddedByPresets(std::size_t generator)
    {
      switch (static_cast<Generator>(generator))
      {
      case Generator::StartAddrsOffset:
      case Generator::EndAddrsOffset:
      case Generator::StartloopAddrsOffset:
      case Generator::EndloopAddrsOffset:
      case Generator::StartAddrsCoarseOffset:
      case Generator::EndAddrsCoarseOffset:
      case Generator::StartloopAddrsCoarseOffset:
      case Generator::EndloopAddrsCoarseOffset:
      case Generator::Keynum:
      case Generator::Velocity:
      case Generator::SampleModes:
      case Generator::ExclusiveClass:
      case Generator::OverridingRootKey:
      case Generator::Instrument:
      case Generator::SampleId:
      case Generator::KeyRange:
      case Generator::VelRange:
        return false;

      default:
        return true;
      }
    }

    inline std::int16_t Saturate(int value)
    {
      return static_cast<std::int16_t>(std::clamp<int>(value,
                                                       std::numeric_limits<std::int16_t>::min(),
                                                       std::numeric_limits<std::int16_t>::max()));
    }

    // What the reader counts while it leaves things out for the notes
    struct Omissions
    {
      std::size_t missingInstruments = 0;
      std::size_t unusableSamples = 0;
      std::size_t droppedLoops = 0;
      std::size_t duplicatePresets = 0;
      std::size_t invalidModulators = 0;
      std::size_t linkedModulators = 0;
      std::size_t unreachableModulators = 0; // zones whose modulator indices are damaged
    };

    // The tables of the pdta list checked for size
    struct Hydra
    {
      Bytes presetHeaders;
      Bytes presetBags;
      Bytes presetModulators;
      Bytes presetGenerators;
      Bytes instrumentHeaders;
      Bytes instrumentBags;
      Bytes instrumentModulators;
      Bytes instrumentGenerators;
      Bytes sampleHeaders;
    };

    // Whether a source operand is one the format defines (8.2). A link is
    // not: modulator chains are not supported. Neither is an index the
    // format reserves or calls illegal, nor a type beyond Switch.
    inline bool IsUsableSource(std::uint16_t operand)
    {
      const Source source = DecodeSource(operand);

      if ((operand >> 10) > static_cast<std::uint16_t>(SourceType::Switch))
      {
        return false;
      }

      if (source.midiController)
      {
        // MIDI functions rather than controllers (0, 6, 38, 98-101, 120-127)
        // and the LSB counterparts of 1-31 (32-63).
        const std::uint8_t index = source.index;
        return !(index == 0 || index == 6 || (index >= 32 && index <= 63) ||
                 (index >= 98 && index <= 101) || index >= 120);
      }

      switch (static_cast<Controller>(source.index))
      {
      case Controller::NoController:
      case Controller::NoteOnVelocity:
      case Controller::NoteOnKey:
      case Controller::PolyPressure:
      case Controller::ChannelPressure:
      case Controller::PitchWheel:
      case Controller::PitchWheelSensitivity:
        return true;

      default:
        return false;
      }
    }

    inline bool IsLink(std::uint16_t operand)
    {
      const Source source = DecodeSource(operand);
      return !source.midiController && source.index == static_cast<std::uint8_t>(Controller::Link);
    }

    // Whether a modulator may change a generator: one the format defines,
    // or the pitch, but not those that only say what a zone is (its ranges
    // and its target).
    inline bool IsUsableDestination(std::uint16_t destination)
    {
      if (destination == InitialPitch)
      {
        return true;
      }

      switch (destination)
      {
      case 14:
      case 18:
      case 19:
      case 20:
      case 42:
      case 49:
      case 55: // unused or reserved numbers
      case static_cast<std::uint16_t>(Generator::Instrument):
      case static_cast<std::uint16_t>(Generator::SampleId):
      case static_cast<std::uint16_t>(Generator::KeyRange):
      case static_cast<std::uint16_t>(Generator::VelRange):
        return false;

      default:
        return destination <= static_cast<std::uint16_t>(Generator::OverridingRootKey);
      }
    }

    // Reads the modulators [first, last) of a table into a zone. Those that
    // cannot be used are counted and left out. Of two identical ones in a
    // zone, the later is kept (7.4).
    inline void ReadModulators(
        Bytes table, std::size_t first, std::size_t last, Zone &zone, Omissions &omissions)
    {
      for (std::size_t index = first; index < last; ++index)
      {
        const std::size_t at = index * ModulatorSize;
        const Modulator modulator{U16(table, at), U16(table, at + 2),
                                  static_cast<std::int16_t>(U16(table, at + 4)), U16(table, at + 6),
                                  U16(table, at + 8)};

        if ((modulator.destination & 0x8000) != 0 || IsLink(modulator.source) ||
            IsLink(modulator.amountSource))
        {
          ++omissions.linkedModulators;
          continue;
        }

        if (!IsUsableSource(modulator.source) || !IsUsableSource(modulator.amountSource) ||
            !IsUsableDestination(modulator.destination) ||
            (modulator.transform != static_cast<std::uint16_t>(Transform::Linear) &&
             modulator.transform != static_cast<std::uint16_t>(Transform::AbsoluteValue)))
        {
          ++omissions.invalidModulators;
          continue;
        }

        const auto earlier = std::find_if(zone.modulators.begin(), zone.modulators.end(),
                                          [&modulator](const Modulator &other)
        { return other.IsIdenticalTo(modulator); });

        if (earlier != zone.modulators.end())
        {
          *earlier = modulator;
        }
        else
        {
          zone.modulators.push_back(modulator);
        }
      }
    }

    // Splits the zones [firstBag, lastBag) of a table into a global zone and
    // the zones with a target. Nullopt if a zone's generator indices run
    // backwards or past the table. Modulator indices that do so cost the
    // zone its modulators only: a damaged modulator table does not make the
    // zone unplayable.
    inline std::optional<ZoneList> ReadZones(Bytes bags,
                                             Bytes generators,
                                             Bytes modulators,
                                             std::size_t firstBag,
                                             std::size_t lastBag,
                                             Generator terminal,
                                             Omissions &omissions)
    {
      const std::size_t generatorCount = generators.size() / GeneratorSize;
      const std::size_t modulatorCount = modulators.size() / ModulatorSize;
      ZoneList list;

      for (std::size_t bag = firstBag; bag < lastBag; ++bag)
      {
        const std::size_t first = U16(bags, bag * BagSize);
        const std::size_t last = U16(bags, (bag + 1) * BagSize);

        if (first > last || last > generatorCount)
        {
          return std::nullopt;
        }

        Zone zone = ReadZone(generators, first, last, terminal);

        const std::size_t firstModulator = U16(bags, bag * BagSize + 2);
        const std::size_t lastModulator = U16(bags, (bag + 1) * BagSize + 2);

        if (firstModulator > lastModulator || lastModulator > modulatorCount)
        {
          ++omissions.unreachableModulators;
        }
        else
        {
          ReadModulators(modulators, firstModulator, lastModulator, zone, omissions);
        }

        if (zone.target)
        {
          list.zones.push_back(std::move(zone));
        }
        else if (bag == firstBag)
        {
          // Only the first zone may be global. A later zone without a
          // target has nothing to play and is ignored.
          list.global = std::move(zone);
        }
      }

      return list;
    }

    // The value a zone list gives a generator for one of its zones: the
    // zone's own, else the global zone's.
    inline std::optional<std::int16_t>
    ValueIn(const ZoneList &list, const Zone &zone, std::size_t generator)
    {
      if (zone.has.test(generator))
      {
        return zone.values[generator];
      }

      if (list.global && list.global->has.test(generator))
      {
        return list.global->values[generator];
      }

      return std::nullopt;
    }

    struct Range
    {
      std::uint8_t low = 0;
      std::uint8_t high = 127;
    };

    inline Range KeyRangeIn(const ZoneList &list, const Zone &zone)
    {
      if (zone.hasKeyRange)
      {
        return Range{zone.lowKey, zone.highKey};
      }

      return list.global && list.global->hasKeyRange
                 ? Range{list.global->lowKey, list.global->highKey}
                 : Range{};
    }

    inline Range VelocityRangeIn(const ZoneList &list, const Zone &zone)
    {
      if (zone.hasVelocityRange)
      {
        return Range{zone.lowVelocity, zone.highVelocity};
      }

      return list.global && list.global->hasVelocityRange
                 ? Range{list.global->lowVelocity, list.global->highVelocity}
                 : Range{};
    }

    // Modulators over others: an identical one replaces, another is added.
    inline void Overlay(std::vector<Modulator> &list, const std::vector<Modulator> &over)
    {
      for (const Modulator &modulator : over)
      {
        const auto identical =
            std::find_if(list.begin(), list.end(), [&modulator](const Modulator &other)
        { return other.IsIdenticalTo(modulator); });

        if (identical != list.end())
        {
          *identical = modulator;
        }
        else
        {
          list.push_back(modulator);
        }
      }
    }

    // A zone list's modulators for one of its zones (9.5.1): those of the
    // global zone, replaced by identical ones of the zone, over `base`.
    inline std::vector<Modulator>
    ModulatorsIn(const ZoneList &list, const Zone &zone, std::vector<Modulator> base)
    {
      if (list.global)
      {
        Overlay(base, list.global->modulators);
      }

      Overlay(base, zone.modulators);
      return base;
    }

    // The regions one preset zone makes of one instrument
    inline void AppendRegions(const SoundFont &font,
                              const ZoneList &preset,
                              const Zone &presetZone,
                              const ZoneList &instrument,
                              std::uint32_t instrumentIndex,
                              Omissions &omissions,
                              std::vector<Region> &regions)
    {
      const Range presetKeys = KeyRangeIn(preset, presetZone);
      const Range presetVelocities = VelocityRangeIn(preset, presetZone);

      // The defaults belong to the instrument level. At the preset level
      // there are none, and what the preset has adds to the instrument's.
      const std::vector<Modulator> presetModulators = ModulatorsIn(preset, presetZone, {});

      for (const Zone &zone : instrument.zones)
      {
        // A region plays where both the preset and the instrument allow it
        const Range keys = KeyRangeIn(instrument, zone);
        const Range velocities = VelocityRangeIn(instrument, zone);

        Region region;
        region.lowKey = std::max(keys.low, presetKeys.low);
        region.highKey = std::min(keys.high, presetKeys.high);
        region.lowVelocity = std::max(velocities.low, presetVelocities.low);
        region.highVelocity = std::min(velocities.high, presetVelocities.high);

        if (region.lowKey > region.highKey || region.lowVelocity > region.highVelocity)
        {
          continue;
        }

        // The instrument's value, or the default, plus what the preset adds
        for (std::size_t generator = 0; generator < GeneratorCount; ++generator)
        {
          int value = ValueIn(instrument, zone, generator)
                          .value_or(DefaultValue(static_cast<Generator>(generator)));

          if (IsAddedByPresets(generator))
          {
            value += ValueIn(preset, presetZone, generator).value_or(0);
          }

          region.generators[generator] = Saturate(value);
        }

        region.generators[static_cast<std::size_t>(Generator::KeyRange)] =
            static_cast<std::int16_t>(region.lowKey | (region.highKey << 8));
        region.generators[static_cast<std::size_t>(Generator::VelRange)] =
            static_cast<std::int16_t>(region.lowVelocity | (region.highVelocity << 8));
        region.generators[static_cast<std::size_t>(Generator::Instrument)] =
            Saturate(static_cast<int>(instrumentIndex));
        region.generators[static_cast<std::size_t>(Generator::SampleId)] = Saturate(*zone.target);

        // The sample, and where in the data the zone's offsets put it
        if (*zone.target >= font.samples.size() || font.samples[*zone.target].isRom)
        {
          ++omissions.unusableSamples;
          continue;
        }

        const Sample &sample = font.samples[*zone.target];
        const auto address = [&](std::uint32_t base, Generator fine, Generator coarse)
        {
          return static_cast<std::int64_t>(base) + region.Value(fine) +
                 static_cast<std::int64_t>(CoarseOffsetUnit) * region.Value(coarse);
        };

        const std::int64_t start =
            address(sample.start, Generator::StartAddrsOffset, Generator::StartAddrsCoarseOffset);
        const std::int64_t end =
            address(sample.end, Generator::EndAddrsOffset, Generator::EndAddrsCoarseOffset);

        if (start < 0 || end > static_cast<std::int64_t>(font.data.size()) || start >= end)
        {
          ++omissions.unusableSamples;
          continue;
        }

        // A loop is kept if it lies inside what is played. A loop point a
        // little outside is pulled in, and one that leaves no loop drops it.
        const std::int64_t loopStart =
            std::max(start, address(sample.loopStart, Generator::StartloopAddrsOffset,
                                    Generator::StartloopAddrsCoarseOffset));
        const std::int64_t loopEnd =
            std::min(end, address(sample.loopEnd, Generator::EndloopAddrsOffset,
                                  Generator::EndloopAddrsCoarseOffset));

        region.sample = *zone.target;
        region.instrument = instrumentIndex;
        region.start = static_cast<std::uint32_t>(start);
        region.end = static_cast<std::uint32_t>(end);

        if (loopStart < loopEnd)
        {
          region.loopStart = static_cast<std::uint32_t>(loopStart);
          region.loopEnd = static_cast<std::uint32_t>(loopEnd);
        }
        else if ((region.Value(Generator::SampleModes) & 1) != 0)
        {
          ++omissions.droppedLoops;
        }

        // The key the recording sounds at: the zone's, else the sample's.
        // The format marks "no pitch" with 255. Such samples, drums mostly,
        // play at their recorded pitch on the middle key.
        const std::int16_t overridingKey = region.Value(Generator::OverridingRootKey);

        if (overridingKey >= 0 && overridingKey <= 127)
        {
          region.rootKey = static_cast<std::uint8_t>(overridingKey);
        }
        else
        {
          region.rootKey = sample.originalKey <= 127 ? sample.originalKey : DefaultRootKey;
        }

        region.modulators = ModulatorsIn(
            instrument, zone,
            std::vector<Modulator>(DefaultModulators.begin(), DefaultModulators.end()));

        for (const Modulator &modulator : presetModulators)
        {
          const auto identical = std::find_if(region.modulators.begin(), region.modulators.end(),
                                              [&modulator](const Modulator &other)
          { return other.IsIdenticalTo(modulator); });

          if (identical != region.modulators.end())
          {
            identical->amount = Saturate(identical->amount + modulator.amount);
          }
          else
          {
            region.modulators.push_back(modulator);
          }
        }

        regions.push_back(std::move(region));
      }
    }

    // THE LISTS
    // ---------

    inline std::optional<std::string> ReadInfo(Bytes list, Info &info)
    {
      std::vector<Chunk> chunks;

      if (!SplitChunks(list, chunks))
      {
        return "its information block is damaged";
      }

      const Chunk *version = Find(chunks, "ifil");

      if (version == nullptr || version->body.size() < 4)
      {
        return "it does not say which version of the format it is";
      }

      info.versionMajor = U16(version->body, 0);
      info.versionMinor = U16(version->body, 2);

      if (info.versionMajor != 2)
      {
        return std::format("it is a SoundFont of version {}.{:02}, but only version 2 (.sf2) can "
                           "be read, not compressed or newer formats",
                           info.versionMajor, info.versionMinor);
      }

      const auto text = [&chunks](std::string_view id)
      {
        const Chunk *chunk = Find(chunks, id);
        return chunk != nullptr ? Text(chunk->body) : std::string();
      };

      info.name = text("INAM");
      info.engine = text("isng");
      info.date = text("ICRD");
      info.author = text("IENG");
      info.product = text("IPRD");
      info.copyright = text("ICOP");
      info.comment = text("ICMT");
      info.software = text("ISFT");

      return std::nullopt;
    }

    inline std::optional<std::string> ReadHydra(Bytes list, Hydra &hydra)
    {
      std::vector<Chunk> chunks;

      if (!SplitChunks(list, chunks))
      {
        return "its preset tables are damaged";
      }

      struct Table
      {
        std::string_view id;
        std::size_t recordSize;
        Bytes *into;
      };

      const std::array<Table, 9> tables{{
          {"phdr", PresetHeaderSize, &hydra.presetHeaders},
          {"pbag", BagSize, &hydra.presetBags},
          {"pmod", ModulatorSize, &hydra.presetModulators},
          {"pgen", GeneratorSize, &hydra.presetGenerators},
          {"inst", InstrumentHeaderSize, &hydra.instrumentHeaders},
          {"ibag", BagSize, &hydra.instrumentBags},
          {"imod", ModulatorSize, &hydra.instrumentModulators},
          {"igen", GeneratorSize, &hydra.instrumentGenerators},
          {"shdr", SampleHeaderSize, &hydra.sampleHeaders},
      }};

      for (const Table &table : tables)
      {
        const Chunk *chunk = Find(chunks, table.id);

        if (chunk == nullptr)
        {
          return std::format("its preset tables are incomplete: there is no '{}' table", table.id);
        }

        // Every table ends with a record that only marks its end
        if (chunk->body.size() % table.recordSize != 0 || chunk->body.size() < table.recordSize)
        {
          return std::format(
              "its '{}' table is damaged: {} bytes are not a whole number of records of {}",
              table.id, chunk->body.size(), table.recordSize);
        }

        *table.into = chunk->body;
      }

      return std::nullopt;
    }

    inline void ReadSampleHeaders(Bytes headers, SoundFont &font)
    {
      const std::size_t count = headers.size() / SampleHeaderSize - 1;
      font.samples.reserve(count);

      for (std::size_t index = 0; index < count; ++index)
      {
        const Bytes record = headers.subspan(index * SampleHeaderSize, SampleHeaderSize);

        Sample sample;
        sample.name = Text(record.first(NameSize));
        sample.start = U32(record, 20);
        sample.end = U32(record, 24);
        sample.loopStart = U32(record, 28);
        sample.loopEnd = U32(record, 32);
        sample.sampleRate = U32(record, 36);
        sample.originalKey = record[40];
        sample.pitchCorrection = static_cast<std::int8_t>(record[41]);
        sample.isRom = (U16(record, 44) & RomSampleFlag) != 0;

        font.samples.push_back(std::move(sample));
      }
    }

    // Instruments and presets flattened into regions
    inline std::optional<std::string>
    ReadPresets(const Hydra &hydra, SoundFont &font, Omissions &omissions)
    {
      // Instruments first: presets are made of them
      const std::size_t instrumentCount = hydra.instrumentHeaders.size() / InstrumentHeaderSize - 1;
      const std::size_t instrumentBagCount = hydra.instrumentBags.size() / BagSize;
      std::vector<ZoneList> instruments;
      instruments.reserve(instrumentCount);

      for (std::size_t index = 0; index < instrumentCount; ++index)
      {
        const Bytes record =
            hydra.instrumentHeaders.subspan(index * InstrumentHeaderSize, InstrumentHeaderSize);
        const std::size_t firstBag = U16(record, NameSize);
        const std::size_t lastBag =
            U16(hydra.instrumentHeaders, (index + 1) * InstrumentHeaderSize + NameSize);

        // The last bag of the table is its end marker, so a zone needs the
        // bag after it to exist.
        if (firstBag > lastBag || lastBag >= instrumentBagCount)
        {
          return std::format(
              "its instrument table is damaged: instrument {} points outside the zone table",
              index);
        }

        std::optional<ZoneList> zones =
            ReadZones(hydra.instrumentBags, hydra.instrumentGenerators, hydra.instrumentModulators,
                      firstBag, lastBag, Generator::SampleId, omissions);

        if (!zones)
        {
          return std::format("its instrument table is damaged: a zone of instrument {} points "
                             "outside the generator table",
                             index);
        }

        instruments.push_back(std::move(*zones));
        font.instrumentNames.push_back(Text(record.first(NameSize)));
      }

      const std::size_t presetCount = hydra.presetHeaders.size() / PresetHeaderSize - 1;
      const std::size_t presetBagCount = hydra.presetBags.size() / BagSize;

      for (std::size_t index = 0; index < presetCount; ++index)
      {
        const Bytes record =
            hydra.presetHeaders.subspan(index * PresetHeaderSize, PresetHeaderSize);
        const std::size_t firstBag = U16(record, 24);
        const std::size_t lastBag = U16(hydra.presetHeaders, (index + 1) * PresetHeaderSize + 24);

        if (firstBag > lastBag || lastBag >= presetBagCount)
        {
          return std::format("its preset table is damaged: preset {} points outside the zone table",
                             index);
        }

        const std::optional<ZoneList> zones =
            ReadZones(hydra.presetBags, hydra.presetGenerators, hydra.presetModulators, firstBag,
                      lastBag, Generator::Instrument, omissions);

        if (!zones)
        {
          return std::format(
              "its preset table is damaged: a zone of preset {} points outside the generator table",
              index);
        }

        Preset preset;
        preset.name = Text(record.first(NameSize));
        preset.program = U16(record, 20);
        preset.bank = U16(record, 22);

        for (const Zone &zone : zones->zones)
        {
          if (*zone.target >= instruments.size())
          {
            ++omissions.missingInstruments;
            continue;
          }

          AppendRegions(font, *zones, zone, instruments[*zone.target], *zone.target, omissions,
                        preset.regions);
        }

        font.presets.push_back(std::move(preset));
      }

      // By bank and program, the way they are asked for. Where two presets
      // claim the same place the first one in the file keeps it.
      std::stable_sort(font.presets.begin(), font.presets.end(),
                       [](const Preset &left, const Preset &right)
      { return std::pair(left.bank, left.program) < std::pair(right.bank, right.program); });

      const auto duplicates = std::unique(font.presets.begin(), font.presets.end(),
                                          [](const Preset &left, const Preset &right)
      { return left.bank == right.bank && left.program == right.program; });

      omissions.duplicatePresets = static_cast<std::size_t>(font.presets.end() - duplicates);
      font.presets.erase(duplicates, font.presets.end());

      return std::nullopt;
    }

    // The 16-bit sample data, straight from the stream: it can be hundreds
    // of megabytes, far more than the rest of the file.
    inline bool
    ReadSampleData(std::istream &input, std::uint64_t byteCount, std::vector<std::int16_t> &data)
    {
      // An odd last byte is half a sample point and is left out
      data.resize(static_cast<std::size_t>(byteCount / 2));

      std::vector<std::uint8_t> block(std::min<std::uint64_t>(byteCount, 1u << 20) &
                                      ~std::uint64_t{1});
      std::size_t written = 0;

      while (written < data.size())
      {
        const std::size_t bytes = std::min(block.size(), (data.size() - written) * 2);

        if (!ReadExact(input, block.data(), bytes))
        {
          return false;
        }

        for (std::size_t at = 0; at < bytes; at += 2)
        {
          data[written++] = static_cast<std::int16_t>(
              static_cast<std::uint16_t>(block[at] | (block[at + 1] << 8)));
        }
      }

      return true;
    }

    inline std::string PathForDisplay(const std::filesystem::path &path)
    {
      const std::u8string utf8 = path.filename().u8string();

      return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    }
  } // namespace detail

  // Reads a SoundFont from a stream, which must be seekable and opened in
  // binary mode. `sourceName` stands in for the file name in messages.
  //
  // The whole file is checked: a file that is not a SoundFont, that ends
  // early, or whose tables contradict each other is refused. A zone that
  // cannot be used is left out and mentioned in `notes`, and the rest of
  // the file still loads.
  [[nodiscard]] inline std::expected<SoundFont, Error> Read(std::istream &input,
                                                            std::string_view sourceName)
  {
    using namespace detail;

    input.clear();
    input.seekg(0, std::ios::end);
    const std::streamoff endOffset = input.tellg();

    if (!input || endOffset < 0)
    {
      return Fail(sourceName, "the file cannot be read");
    }

    const std::uint64_t fileSize = static_cast<std::uint64_t>(endOffset);
    std::array<std::uint8_t, 12> header{};
    input.seekg(0);

    if (fileSize < header.size() || !ReadExact(input, header.data(), header.size()))
    {
      return Fail(sourceName, "this is not a SoundFont file: it is too short");
    }

    if (!Is(IdAt(header, 0), "RIFF") || !Is(IdAt(header, 8), "sfbk"))
    {
      return Fail(sourceName, "this is not a SoundFont file");
    }

    // The length the file says it has. What follows it is ignored, and a
    // file shorter than that has lost its end.
    const std::uint64_t riffEnd = 8 + std::uint64_t{U32(header, 4)};

    if (riffEnd < header.size())
    {
      return Fail(sourceName, "this is not a SoundFont file");
    }

    if (riffEnd > fileSize)
    {
      return Fail(sourceName, std::format("the file is incomplete: it says it is {} bytes long and "
                                          "is {} (an interrupted download or copy?)",
                                          riffEnd, fileSize));
    }

    SoundFont font;
    std::vector<std::uint8_t> infoList;
    std::vector<std::uint8_t> hydraList;
    bool hasInfo = false;
    bool hasHydra = false;
    bool hasSamples = false;
    std::optional<std::pair<std::uint64_t, std::uint64_t>>
        lowByteChunk; // where the sm24 data is and its size
    std::uint64_t position = header.size();
    std::array<std::uint8_t, 12> chunkHeader{};

    // (A chunk's padding byte may be missing at the very end, which puts the
    // position one past it. Hence "position + 8 <= end", not a subtraction)
    while (position + 8 <= riffEnd)
    {
      input.seekg(static_cast<std::streamoff>(position));

      if (!ReadExact(input, chunkHeader.data(), 8))
      {
        return Fail(sourceName, "the file cannot be read");
      }

      const Id id = IdAt(chunkHeader, 0);
      const std::uint64_t size = U32(chunkHeader, 4);
      const std::uint64_t body = position + 8;

      if (size > riffEnd - body)
      {
        return Fail(sourceName, "the file is damaged: a block of it is longer than the file");
      }

      position = body + size + (size & 1);

      if (!Is(id, "LIST") || size < 4)
      {
        continue;
      }

      if (!ReadExact(input, chunkHeader.data() + 8, 4))
      {
        return Fail(sourceName, "the file cannot be read");
      }

      const Id type = IdAt(chunkHeader, 8);
      const std::uint64_t listSize = size - 4;
      const bool isInfo = Is(type, "INFO");

      if (isInfo || Is(type, "pdta"))
      {
        if (listSize > MaxTableBytes)
        {
          return Fail(sourceName, "the file is damaged: its tables are impossibly large");
        }

        std::vector<std::uint8_t> &list = isInfo ? infoList : hydraList;
        list.resize(static_cast<std::size_t>(listSize));

        if (!ReadExact(input, list.data(), list.size()))
        {
          return Fail(sourceName, "the file cannot be read");
        }

        (isInfo ? hasInfo : hasHydra) = true;
      }
      else if (Is(type, "sdta"))
      {
        // Its chunks are walked in the stream. The 16-bit data is read now,
        // and the 24-bit low bytes once the version is known, which says
        // whether they count.
        std::uint64_t at = body + 4;
        const std::uint64_t end = body + size;

        while (at + 8 <= end)
        {
          input.seekg(static_cast<std::streamoff>(at));

          if (!ReadExact(input, chunkHeader.data(), 8))
          {
            return Fail(sourceName, "the file cannot be read");
          }

          const std::uint64_t dataSize = U32(chunkHeader, 4);

          if (dataSize > end - (at + 8))
          {
            return Fail(sourceName, "the file is damaged: its sample data is longer than the file");
          }

          if (Is(IdAt(chunkHeader, 0), "smpl") && !hasSamples)
          {
            if (!ReadSampleData(input, dataSize, font.data))
            {
              return Fail(sourceName, "the file cannot be read");
            }

            hasSamples = true;
          }
          else if (Is(IdAt(chunkHeader, 0), "sm24") && !lowByteChunk)
          {
            lowByteChunk = std::pair(at + 8, dataSize);
          }

          at += 8 + dataSize + (dataSize & 1);
        }
      }
    }

    if (!hasInfo || !hasHydra)
    {
      return Fail(sourceName, std::format("this is not a complete SoundFont file: it has no {}",
                                          hasInfo ? "preset tables" : "information block"));
    }

    if (const std::optional<std::string> problem = ReadInfo(infoList, font.info))
    {
      return Fail(sourceName, *problem);
    }

    // The low bytes of 24-bit points (6.2): one for every point of the
    // 16-bit data, padded to an even size. They count only in a file of
    // version 2.04 or later and only if they fit the 16-bit data. Otherwise
    // the 16-bit data is used alone.
    std::optional<std::string> lowBytesIgnored;

    if (lowByteChunk)
    {
      const std::uint64_t points = font.data.size();

      if (!hasSamples)
      {
        lowBytesIgnored = "there is no 16-bit data to go with it";
      }
      else if (font.info.versionMinor < 4)
      {
        lowBytesIgnored =
            std::format("the file says it is of version 2.{:02}, and 24-bit data needs 2.04",
                        font.info.versionMinor);
      }
      else if (lowByteChunk->second != points + (points & 1))
      {
        lowBytesIgnored =
            std::format("it has {} bytes for {} points", lowByteChunk->second, points);
      }
      else
      {
        font.lowBytes.resize(static_cast<std::size_t>(points));
        input.clear();
        input.seekg(static_cast<std::streamoff>(lowByteChunk->first));

        if (!ReadExact(input, font.lowBytes.data(), font.lowBytes.size()))
        {
          return Fail(sourceName, "the file cannot be read");
        }
      }
    }

    Hydra hydra;

    if (const std::optional<std::string> problem = ReadHydra(hydraList, hydra))
    {
      return Fail(sourceName, *problem);
    }

    ReadSampleHeaders(hydra.sampleHeaders, font);

    Omissions omissions;

    if (const std::optional<std::string> problem = ReadPresets(hydra, font, omissions))
    {
      return Fail(sourceName, *problem);
    }

    if (font.presets.empty())
    {
      return Fail(sourceName, "it contains no presets");
    }

    const bool playable = std::any_of(font.presets.begin(), font.presets.end(),
                                      [](const Preset &preset) { return !preset.regions.empty(); });

    if (!playable)
    {
      return Fail(sourceName,
                  hasSamples ? "none of its presets can be played: their samples are missing or "
                               "lie outside the sample data"
                             : "it contains no sample data, so none of its presets can be played");
    }

    // What was left out, so that nobody has to wonder why something is silent
    if (omissions.unusableSamples > 0)
    {
      font.notes.push_back(std::format("{} zones were left out: their sample is missing, in a "
                                       "sound card's ROM, or lies outside the sample data.",
                                       omissions.unusableSamples));
    }

    if (omissions.missingInstruments > 0)
    {
      font.notes.push_back(
          std::format("{} zones were left out: they name an instrument that is not in the file.",
                      omissions.missingInstruments));
    }

    if (omissions.droppedLoops > 0)
    {
      font.notes.push_back(
          std::format("{} zones play without their loop: its points lie outside the sample.",
                      omissions.droppedLoops));
    }

    if (omissions.duplicatePresets > 0)
    {
      font.notes.push_back(
          std::format("{} presets were left out: an earlier preset has the same bank and program.",
                      omissions.duplicatePresets));
    }

    if (omissions.invalidModulators > 0)
    {
      font.notes.push_back(std::format("{} modulators were left out: their source, destination, or "
                                       "transform is not one the format defines.",
                                       omissions.invalidModulators));
    }

    if (omissions.linkedModulators > 0)
    {
      font.notes.push_back(std::format("{} modulators were left out: they are linked to other "
                                       "modulators, which this reader does not support.",
                                       omissions.linkedModulators));
    }

    if (omissions.unreachableModulators > 0)
    {
      font.notes.push_back(
          std::format("{} zones lost their modulators: they point outside the modulator table.",
                      omissions.unreachableModulators));
    }

    if (lowBytesIgnored)
    {
      font.notes.push_back(
          std::format("The 24-bit sample data was ignored, and only the 16-bit data is used: {}.",
                      *lowBytesIgnored));
    }

    return font;
  }

  // Reads a SoundFont file. See Read for what is checked. The message of an
  // error names the file without its folder.
  [[nodiscard]] inline std::expected<SoundFont, Error> Load(const std::filesystem::path &path)
  {
    using namespace detail;

    const std::string name = PathForDisplay(path);
    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
      return Fail(name, "the file cannot be opened");
    }

    // The sample data is as large as the file says, and the file may say
    // anything: running out of memory is a failure to report, not to crash on.
    try
    {
      return Read(file, name);
    }
    catch (const std::bad_alloc &)
    {
      return Fail(name, "the file is too large to load into memory");
    }
    catch (const std::length_error &)
    {
      return Fail(name, "the file is too large to load into memory");
    }
  }
} // namespace havSF2

#endif // HAVSF2_HPP
