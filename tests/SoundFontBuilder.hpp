#ifndef HAVSF2_TESTS_SOUNDFONTBUILDER_HPP
#define HAVSF2_TESTS_SOUNDFONTBUILDER_HPP

#include "havSF2.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Writes SoundFont files for the tests byte by byte and independently of
// the reader: a test says which samples, instruments, and presets it wants
// and gets the file a SoundFont editor would have saved. Tables can be
// replaced or left out to make damaged files.

namespace test
{
  // A generator and its amount as the file stores it: a signed number, an
  // index, or a range packed with RangeAmount.
  using GeneratorList = std::vector<std::pair<havSF2::Generator, int>>;

  inline int RangeAmount(int low, int high)
  {
    return low | (high << 8);
  }

  struct BuilderSample
  {
    std::string name;
    std::vector<std::int16_t> points;
    std::uint32_t loopStart = 0; // counted from the sample's first point
    std::uint32_t loopEnd = 0;
    std::uint32_t sampleRate = 44100;
    std::uint8_t originalKey = 60;
    std::int8_t pitchCorrection = 0;
    std::uint16_t type = 1; // mono (0x8001 is a mono sample in ROM)
  };

  // The modulators of a zone, as the file stores them.
  using ModulatorList = std::vector<havSF2::Modulator>;

  // A zone ends on SampleId (in an instrument) or Instrument (in a
  // preset). One without is a global zone. `modulators[n]` belongs to
  // zone n, and may be shorter than `zones`.
  struct BuilderInstrument
  {
    std::string name;
    std::vector<GeneratorList> zones;
    std::vector<ModulatorList> modulators = {};
  };

  struct BuilderPreset
  {
    std::string name;
    std::uint16_t program = 0;
    std::uint16_t bank = 0;
    std::vector<GeneratorList> zones;
    std::vector<ModulatorList> modulators = {};
  };

  struct SoundFontBuilder
  {
    std::uint16_t versionMajor = 2;
    std::uint16_t versionMinor = 1;
    std::string name = "Test Font";
    std::string engine = "EMU8000";
    std::vector<std::pair<std::string, std::string>> moreInfo; // chunk id and text
    std::vector<BuilderSample> samples;
    std::vector<BuilderInstrument> instruments;
    std::vector<BuilderPreset> presets;
    bool with24Bit =
        false; // an sm24 chunk whose byte n is n % 251, the right size for the 16-bit data

    // For damaged files: the body to use for a chunk instead of the right
    // one, and chunks to leave out. Ids as in the file: "phdr", "smpl", ...
    std::map<std::string, std::string> replace;
    std::set<std::string> omit;

    [[nodiscard]] std::string build() const
    {
      // Sample data: every sample followed by the 46 zero points the
      // format asks for.
      std::string pool;
      std::string sampleHeaders;
      std::uint32_t position = 0;

      for (const BuilderSample &sample : samples)
      {
        for (const std::int16_t point : sample.points)
        {
          U16(pool, static_cast<std::uint16_t>(point));
        }

        for (int padding = 0; padding < 46; ++padding)
        {
          U16(pool, 0);
        }

        const std::uint32_t length = static_cast<std::uint32_t>(sample.points.size());
        Name(sampleHeaders, sample.name);
        U32(sampleHeaders, position);
        U32(sampleHeaders, position + length);
        U32(sampleHeaders, position + sample.loopStart);
        U32(sampleHeaders, position + sample.loopEnd);
        U32(sampleHeaders, sample.sampleRate);
        sampleHeaders.push_back(static_cast<char>(sample.originalKey));
        sampleHeaders.push_back(static_cast<char>(sample.pitchCorrection));
        U16(sampleHeaders, 0);
        U16(sampleHeaders, sample.type);
        position += length + 46;
      }

      Name(sampleHeaders, "EOS");
      sampleHeaders.append(26, '\0');

      // Instruments and presets share their layout: headers that point
      // into a table of zones that point into a table of generators.
      std::string instrumentHeaders;
      std::string instrumentBags;
      std::string instrumentModulators;
      std::string instrumentGenerators;
      std::uint16_t bag = 0;
      std::uint16_t modulator = 0;
      std::uint16_t generator = 0;

      const auto zones = [&](const std::vector<GeneratorList> &list,
                             const std::vector<ModulatorList> &modulatorLists, std::string &bags,
                             std::string &modulatorTable, std::string &generators)
      {
        for (std::size_t index = 0; index < list.size(); ++index)
        {
          U16(bags, generator);
          U16(bags, modulator);
          ++bag;

          for (const auto &[which, amount] : list[index])
          {
            U16(generators, static_cast<std::uint16_t>(which));
            U16(generators, static_cast<std::uint16_t>(amount));
            ++generator;
          }

          if (index < modulatorLists.size())
          {
            for (const havSF2::Modulator &record : modulatorLists[index])
            {
              U16(modulatorTable, record.source);
              U16(modulatorTable, record.destination);
              U16(modulatorTable, static_cast<std::uint16_t>(record.amount));
              U16(modulatorTable, record.amountSource);
              U16(modulatorTable, record.transform);
              ++modulator;
            }
          }
        }
      };

      for (const BuilderInstrument &instrument : instruments)
      {
        Name(instrumentHeaders, instrument.name);
        U16(instrumentHeaders, bag);
        zones(instrument.zones, instrument.modulators, instrumentBags, instrumentModulators,
              instrumentGenerators);
      }

      Name(instrumentHeaders, "EOI");
      U16(instrumentHeaders, bag);
      U16(instrumentBags, generator);
      U16(instrumentBags, modulator);
      instrumentModulators.append(10, '\0');
      instrumentGenerators.append(4, '\0');

      std::string presetHeaders;
      std::string presetBags;
      std::string presetModulators;
      std::string presetGenerators;
      bag = 0;
      modulator = 0;
      generator = 0;

      for (const BuilderPreset &preset : presets)
      {
        Name(presetHeaders, preset.name);
        U16(presetHeaders, preset.program);
        U16(presetHeaders, preset.bank);
        U16(presetHeaders, bag);
        presetHeaders.append(12, '\0');
        zones(preset.zones, preset.modulators, presetBags, presetModulators, presetGenerators);
      }

      Name(presetHeaders, "EOP");
      U16(presetHeaders, 0);
      U16(presetHeaders, 0);
      U16(presetHeaders, bag);
      presetHeaders.append(12, '\0');
      U16(presetBags, generator);
      U16(presetBags, modulator);
      presetModulators.append(10, '\0');
      presetGenerators.append(4, '\0');

      // The three lists
      std::string version;
      U16(version, versionMajor);
      U16(version, versionMinor);

      std::string info = "INFO";
      info += chunk("ifil", version);
      info += chunk("isng", engine + '\0');
      info += chunk("INAM", name + '\0');

      for (const auto &[id, text] : moreInfo)
      {
        info += chunk(id, text + '\0');
      }

      std::string data = "sdta";
      data += chunk("smpl", pool);

      if (with24Bit)
      {
        // One byte per point padded to an even size as the format asks
        std::string lowBytes;

        for (std::size_t point = 0; point < pool.size() / 2; ++point)
        {
          lowBytes.push_back(static_cast<char>(point % 251));
        }

        if (lowBytes.size() % 2 != 0)
        {
          lowBytes.push_back('\0');
        }

        data += chunk("sm24", lowBytes);
      }

      std::string tables = "pdta";
      tables += chunk("phdr", presetHeaders);
      tables += chunk("pbag", presetBags);
      tables += chunk("pmod", presetModulators);
      tables += chunk("pgen", presetGenerators);
      tables += chunk("inst", instrumentHeaders);
      tables += chunk("ibag", instrumentBags);
      tables += chunk("imod", instrumentModulators);
      tables += chunk("igen", instrumentGenerators);
      tables += chunk("shdr", sampleHeaders);

      std::string body = "sfbk";
      body += chunk("LIST", info, true);
      body += chunk("LIST", data, true);
      body += chunk("LIST", tables, true);

      return chunk("RIFF", body, true);
    }

  private:
    static void U16(std::string &to, std::uint16_t value)
    {
      to.push_back(static_cast<char>(value & 0xFF));
      to.push_back(static_cast<char>(value >> 8));
    }

    static void U32(std::string &to, std::uint32_t value)
    {
      U16(to, static_cast<std::uint16_t>(value & 0xFFFF));
      U16(to, static_cast<std::uint16_t>(value >> 16));
    }

    // A name field: 20 bytes filled with zeros
    static void Name(std::string &to, std::string_view name)
    {
      std::string field(name.substr(0, 20));
      field.resize(20, '\0');
      to += field;
    }

    // A chunk with its header and padding. Replaced or left out if the test
    // asked for that, but `structural` chunks (RIFF and the lists) never are.
    [[nodiscard]] std::string
    chunk(std::string_view id, std::string body, bool structural = false) const
    {
      if (!structural)
      {
        if (omit.contains(std::string(id)))
        {
          return {};
        }

        if (const auto replacement = replace.find(std::string(id)); replacement != replace.end())
        {
          body = replacement->second;
        }
      }

      std::string result(id);
      U32(result, static_cast<std::uint32_t>(body.size()));
      result += body;

      if (body.size() % 2 != 0)
      {
        result.push_back('\0');
      }

      return result;
    }
  };
} // namespace test

#endif
