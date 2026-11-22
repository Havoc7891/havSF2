#include "SoundFontBuilder.hpp"
#include "Test.hpp"

#include "havSF2.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// The reader: that it reads what a file says, resolves zones into
// regions the way the format defines, and refuses or works around whatever
// a damaged file can throw at it.

using namespace havSF2;
using test::BuilderInstrument;
using test::BuilderPreset;
using test::BuilderSample;
using test::GeneratorList;
using test::RangeAmount;
using test::SoundFontBuilder;
using enum havSF2::Generator;

namespace
{
  std::expected<SoundFont, Error> ReadBytes(const std::string &bytes,
                                            std::string_view name = "test.sf2")
  {
    std::istringstream stream(bytes, std::ios::binary);
    return havSF2::Read(stream, name);
  }

  SoundFont ReadOrFail(const SoundFontBuilder &builder)
  {
    auto font = ReadBytes(builder.build());

    if (!font)
    {
      test::ReportFailure(__FILE__, __LINE__, "not read: " + font.error().message);
      REQUIRE(font.has_value());
    }

    return std::move(*font);
  }

  void ExpectRefused(const std::string &bytes, std::string_view fragment)
  {
    const auto font = ReadBytes(bytes);

    if (font.has_value())
    {
      test::ReportFailure(__FILE__, __LINE__,
                          std::string("accepted, but should be refused for: ") +
                              std::string(fragment));
    }
    else if (font.error().message.find(fragment) == std::string::npos ||
             !font.error().message.starts_with("test.sf2: "))
    {
      test::ReportFailure(__FILE__, __LINE__, "unexpected message: " + font.error().message);
    }
  }

  bool HasNote(const SoundFont &font, std::string_view fragment)
  {
    for (const std::string &note : font.notes)
    {
      if (note.find(fragment) != std::string::npos)
      {
        return true;
      }
    }

    return false;
  }

  // A ramp: point n has the value first + n * step. Easy to recognize.
  std::vector<std::int16_t> Ramp(std::size_t count, int first = 0, int step = 100)
  {
    std::vector<std::int16_t> points;

    for (std::size_t index = 0; index < count; ++index)
    {
      points.push_back(static_cast<std::int16_t>(first + static_cast<int>(index) * step));
    }

    return points;
  }

  Modulator WithAmount(Modulator modulator, int amount)
  {
    modulator.amount = static_cast<std::int16_t>(amount);
    return modulator;
  }

  const Modulator *FindIdentical(const Region &region, const Modulator &modulator)
  {
    for (const Modulator &other : region.modulators)
    {
      if (other.IsIdenticalTo(modulator))
      {
        return &other;
      }
    }

    return nullptr;
  }

  // One sample, one instrument that plays it, one preset of that instrument
  SoundFontBuilder Simple()
  {
    SoundFontBuilder builder;
    builder.samples.push_back(BuilderSample{"Ramp", Ramp(100), 10, 90, 22050, 69, -3});
    builder.instruments.push_back(BuilderInstrument{"Ramp Instrument", {{{SampleId, 0}}}});
    builder.presets.push_back(BuilderPreset{"Ramp Preset", 5, 0, {{{Instrument, 0}}}});
    return builder;
  }

  // A small sound set that looks like a real one: a piano split into two
  // key ranges with a soft and a loud layer, and a drum kit.
  SoundFontBuilder Fixture()
  {
    SoundFontBuilder builder;
    builder.versionMinor = 4;
    builder.with24Bit = true;
    builder.name = "havSF2 test set";
    builder.moreInfo = {{"ICRD", "2026-11-15"},
                        {"IENG", "havSF2 tests"},
                        {"ICMT", "Three tiny ramps. Not for listening."}};
    builder.samples.push_back(
        BuilderSample{"Piano Low", Ramp(64, -3200, 100), 16, 48, 44100, 48, 0});
    builder.samples.push_back(
        BuilderSample{"Piano High", Ramp(48, -2400, 100), 8, 40, 44100, 72, 5});
    builder.samples.push_back(BuilderSample{"Kick", Ramp(32, 3100, -200), 0, 0, 22050, 255, 0});

    builder.instruments.push_back(
        BuilderInstrument{"Piano",
                          {
                              {{ReleaseVolEnv, -2000}, {SampleModes, 1}},
                              {{KeyRange, RangeAmount(0, 59)}, {SampleId, 0}},
                              {{KeyRange, RangeAmount(60, 127)}, {Pan, 100}, {SampleId, 1}},
                          }});
    builder.instruments.push_back(
        BuilderInstrument{"Kit",
                          {
                              {{KeyRange, RangeAmount(36, 36)}, {ExclusiveClass, 1}, {SampleId, 2}},
                          }});

    // The mod wheel opens the piano's filter, and the soft layer reacts
    // less to velocity than the default says.
    builder.instruments.front().modulators = {
        {Modulator{0x0081, static_cast<std::uint16_t>(InitialFilterFc), 1200, 0, 0}}};

    builder.presets.push_back(BuilderPreset{
        "Piano",
        0,
        0,
        {
            {{VelRange, RangeAmount(0, 79)}, {InitialAttenuation, 60}, {Instrument, 0}},
            {{VelRange, RangeAmount(80, 127)}, {Instrument, 0}},
        },
        {{WithAmount(DefaultModulators[0], -480)}}});
    builder.presets.push_back(
        BuilderPreset{"Standard Kit", 0, PercussionBank, {{{Instrument, 1}}}});
    return builder;
  }

  std::filesystem::path ScratchFolder()
  {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "havsf2-tests";
    std::filesystem::create_directories(folder);
    return folder;
  }

} // namespace

TEST_CASE(SoundFontIsReadWithItsSamplesAndPresets)
{
  const SoundFont font = ReadOrFail(Simple());

  CHECK_EQ(font.info.versionMajor, 2);
  CHECK_EQ(font.info.versionMinor, 1);
  CHECK_EQ(font.info.name, "Test Font");
  CHECK_EQ(font.info.engine, "EMU8000");
  CHECK(font.notes.empty());

  // The sample where the file says it is
  REQUIRE_EQ(font.samples.size(), 1);
  const Sample &sample = font.samples.front();
  CHECK_EQ(sample.name, "Ramp");
  CHECK_EQ(sample.start, 0);
  CHECK_EQ(sample.end, 100);
  CHECK_EQ(sample.loopStart, 10);
  CHECK_EQ(sample.loopEnd, 90);
  CHECK_EQ(sample.sampleRate, 22050);
  CHECK_EQ(sample.originalKey, 69);
  CHECK_EQ(sample.pitchCorrection, -3);
  CHECK(!sample.isRom);

  // Its points and the silence the format puts after every sample
  REQUIRE_EQ(font.data.size(), 146);
  CHECK_EQ(font.data[0], 0);
  CHECK_EQ(font.data[1], 100);
  CHECK_EQ(font.data[99], 9900);
  CHECK_EQ(font.data[100], 0);

  // The preset, found by bank and program, with one region for every key
  // and velocity.
  REQUIRE_EQ(font.presets.size(), 1);
  CHECK_EQ(font.instrumentNames, std::vector<std::string>{"Ramp Instrument"});

  const Preset *preset = font.FindPreset(0, 5);
  REQUIRE(preset != nullptr);
  CHECK_EQ(preset->name, "Ramp Preset");
  CHECK(font.FindPreset(0, 4) == nullptr);
  CHECK(font.FindPreset(1, 5) == nullptr);

  REQUIRE_EQ(preset->regions.size(), 1);
  const Region &region = preset->regions.front();
  CHECK_EQ(region.lowKey, 0);
  CHECK_EQ(region.highKey, 127);
  CHECK_EQ(region.lowVelocity, 0);
  CHECK_EQ(region.highVelocity, 127);
  CHECK_EQ(region.sample, 0);
  CHECK_EQ(region.instrument, 0);
  CHECK_EQ(region.start, 0);
  CHECK_EQ(region.end, 100);
  CHECK_EQ(region.loopStart, 10);
  CHECK_EQ(region.loopEnd, 90);
  CHECK_EQ(region.rootKey, 69);

  // Nothing was set, so every generator has the format's default
  for (std::size_t generator = 0; generator < GeneratorCount; ++generator)
  {
    const Generator which = static_cast<Generator>(generator);

    if (which != Instrument && which != SampleId)
    {
      CHECK_EQ(region.generators[generator], DefaultValue(which));
    }
  }

  CHECK_EQ(region.Value(InitialFilterFc), 13500);
  CHECK_EQ(region.Value(AttackVolEnv), -12000);
  CHECK_EQ(region.Value(ReleaseVolEnv), -12000);
  CHECK_EQ(region.Value(SustainVolEnv), 0);
  CHECK_EQ(region.Value(ScaleTuning), 100);
  CHECK_EQ(region.Value(OverridingRootKey), -1);
  CHECK_EQ(region.Value(Pan), 0);

  // No modulators in the file: the region has the default ones, and the
  // file has no 24-bit data.
  CHECK(region.modulators ==
        std::vector<Modulator>(DefaultModulators.begin(), DefaultModulators.end()));
  CHECK(font.lowBytes.empty());

  CHECK_EQ(font.FindRegions(0, 5, 60, 100).size(), 1);
  CHECK(font.FindRegions(0, 6, 60, 100).empty());
}

TEST_CASE(SoundFontInformationIsText)
{
  SoundFontBuilder builder = Simple();
  builder.name = "Grand  ";
  builder.moreInfo = {{"ICRD", "November 2026"},
                      {"IENG", "Somebody"},
                      {"IPRD", "A Product"},
                      {"ICOP", "(c) Somebody"},
                      {"ICMT", "A comment of odd length"},
                      {"ISFT", "An Editor 1.0:An Editor 1.0"},
                      {"XTRA", "an unknown chunk"}};

  const SoundFont font = ReadOrFail(builder);

  // Trailing spaces go, and unknown chunks are skipped
  CHECK_EQ(font.info.name, "Grand");
  CHECK_EQ(font.info.date, "November 2026");
  CHECK_EQ(font.info.author, "Somebody");
  CHECK_EQ(font.info.product, "A Product");
  CHECK_EQ(font.info.copyright, "(c) Somebody");
  CHECK_EQ(font.info.comment, "A comment of odd length");
  CHECK_EQ(font.info.software, "An Editor 1.0:An Editor 1.0");

  // The format has no character set: what is not plain ASCII is shown as
  // a question mark instead of being passed on as it is.
  builder.name = std::string("Fl") + static_cast<char>(0xFC) + "gel" + static_cast<char>(0x07);
  builder.samples.front().name = "exactly twenty chars";
  const SoundFont odd = ReadOrFail(builder);
  CHECK_EQ(odd.info.name, "Fl?gel?");
  CHECK_EQ(odd.samples.front().name, "exactly twenty chars");

  // Optional strings may be missing altogether
  builder.omit = {"INAM", "isng"};
  builder.moreInfo.clear();
  const SoundFont bare = ReadOrFail(builder);
  CHECK_EQ(bare.info.name, "");
  CHECK_EQ(bare.info.engine, "");
}

TEST_CASE(SoundFontZonesResolveIntoRegions)
{
  SoundFontBuilder builder;
  builder.samples.push_back(BuilderSample{"A", Ramp(100), 10, 90, 44100, 60, 0});
  builder.samples.push_back(BuilderSample{"B", Ramp(50), 5, 45, 44100, 72, 0});

  // An instrument with a global zone and two zones that split the keys
  builder.instruments.push_back(BuilderInstrument{
      "Split",
      {
          {{Pan, -250}, {AttackVolEnv, -7000}, {InitialAttenuation, 30}},
          {{KeyRange, RangeAmount(0, 59)}, {SampleId, 0}},
          {{KeyRange, RangeAmount(60, 127)}, {Pan, 250}, {FineTune, 12}, {SampleId, 1}},
      }});

  // A preset with a global zone of its own that adds to all of that
  builder.presets.push_back(BuilderPreset{"Split Preset",
                                          0,
                                          0,
                                          {
                                              {{InitialAttenuation, 10}, {FineTune, -2}},
                                              {{AttackVolEnv, 500}, {Instrument, 0}},
                                          }});

  const SoundFont font = ReadOrFail(builder);
  const Preset *preset = font.FindPreset(0, 0);
  REQUIRE(preset != nullptr);
  REQUIRE_EQ(preset->regions.size(), 2);

  const Region &low = preset->regions[0];
  const Region &high = preset->regions[1];

  CHECK_EQ(low.lowKey, 0);
  CHECK_EQ(low.highKey, 59);
  CHECK_EQ(low.sample, 0);
  CHECK_EQ(high.lowKey, 60);
  CHECK_EQ(high.highKey, 127);
  CHECK_EQ(high.sample, 1);

  // The instrument's global zone gives what a zone does not set itself
  CHECK_EQ(low.Value(Pan), -250);
  CHECK_EQ(high.Value(Pan), 250);

  // The preset adds to the instrument's value: its own zone's amount,
  // else its global zone's.
  CHECK_EQ(low.Value(AttackVolEnv), -7000 + 500);
  CHECK_EQ(high.Value(AttackVolEnv), -7000 + 500);
  CHECK_EQ(low.Value(InitialAttenuation), 30 + 10);
  CHECK_EQ(low.Value(FineTune), 0 - 2);
  CHECK_EQ(high.Value(FineTune), 12 - 2);

  // What nobody set is the default plus nothing
  CHECK_EQ(low.Value(ReleaseVolEnv), -12000);
  CHECK_EQ(low.Value(InitialFilterFc), 13500);

  // The sample's own root key, and the right sample for each key
  CHECK_EQ(low.rootKey, 60);
  CHECK_EQ(high.rootKey, 72);
  CHECK_EQ(high.start, 146);
  CHECK_EQ(high.end, 196);

  REQUIRE_EQ(font.FindRegions(0, 0, 59, 64).size(), 1);
  CHECK_EQ(font.FindRegions(0, 0, 59, 64).front()->sample, 0);
  CHECK_EQ(font.FindRegions(0, 0, 60, 64).front()->sample, 1);
}

TEST_CASE(SoundFontRangesNarrowEachOther)
{
  SoundFontBuilder builder;
  builder.samples.push_back(BuilderSample{"A", Ramp(100), 0, 0, 44100, 60, 0});

  builder.instruments.push_back(BuilderInstrument{
      "Layers",
      {
          {{KeyRange, RangeAmount(40, 80)}, {VelRange, RangeAmount(0, 63)}, {SampleId, 0}},
          {{KeyRange, RangeAmount(40, 80)},
           {VelRange, RangeAmount(64, 127)},
           {InitialAttenuation, -50},
           {SampleId, 0}},
          {{KeyRange, RangeAmount(100, 110)}, {SampleId, 0}},
      }});

  // The preset lets only part of the keyboard and the louder notes through
  builder.presets.push_back(BuilderPreset{
      "Narrow",
      0,
      0,
      {
          {{KeyRange, RangeAmount(60, 90)}, {VelRange, RangeAmount(32, 127)}, {Instrument, 0}},
      }});

  // Ranges the wrong way round, and a global zone that gives the ranges
  builder.instruments.push_back(
      BuilderInstrument{"Global range",
                        {
                            {{KeyRange, RangeAmount(50, 70)}},
                            {{SampleId, 0}},
                            {{KeyRange, RangeAmount(72, 71)}, {SampleId, 0}},
                        }});
  builder.presets.push_back(BuilderPreset{"Wide", 1, 0, {{{Instrument, 1}}}});

  const SoundFont font = ReadOrFail(builder);
  const Preset *narrow = font.FindPreset(0, 0);
  REQUIRE(narrow != nullptr);

  // Both ranges have to allow a note. The zone for keys 100 to 110 has no
  // key in common with the preset and makes no region at all.
  REQUIRE_EQ(narrow->regions.size(), 2);
  CHECK_EQ(narrow->regions[0].lowKey, 60);
  CHECK_EQ(narrow->regions[0].highKey, 80);
  CHECK_EQ(narrow->regions[0].lowVelocity, 32);
  CHECK_EQ(narrow->regions[0].highVelocity, 63);
  CHECK_EQ(narrow->regions[1].lowVelocity, 64);
  CHECK_EQ(narrow->regions[1].highVelocity, 127);

  CHECK(font.FindRegions(0, 0, 59, 100).empty());
  CHECK(font.FindRegions(0, 0, 70, 31).empty());
  REQUIRE_EQ(font.FindRegions(0, 0, 70, 40).size(), 1);
  CHECK_EQ(font.FindRegions(0, 0, 70, 40).front()->Value(InitialAttenuation), 0);
  CHECK_EQ(font.FindRegions(0, 0, 70, 100).front()->Value(InitialAttenuation), -50);
  CHECK(font.FindRegions(0, 0, 105, 100).empty());

  const Preset *wide = font.FindPreset(0, 1);
  REQUIRE(wide != nullptr);
  REQUIRE_EQ(wide->regions.size(), 2);
  CHECK_EQ(wide->regions[0].lowKey, 50);
  CHECK_EQ(wide->regions[0].highKey, 70);
  CHECK_EQ(wide->regions[1].lowKey, 71);
  CHECK_EQ(wide->regions[1].highKey, 72);
}

TEST_CASE(SoundFontPresetsMayNotMoveSamples)
{
  SoundFontBuilder builder;
  builder.samples.push_back(BuilderSample{"A", Ramp(200), 20, 180, 44100, 60, 0});

  builder.instruments.push_back(BuilderInstrument{"Offsets",
                                                  {
                                                      {{StartAddrsOffset, 5},
                                                       {EndAddrsOffset, -10},
                                                       {StartloopAddrsOffset, 3},
                                                       {EndloopAddrsOffset, -4},
                                                       {SampleModes, 1},
                                                       {ExclusiveClass, 7},
                                                       {OverridingRootKey, 64},
                                                       {Keynum, 50},
                                                       {Velocity, 90},
                                                       {SampleId, 0}},
                                                  }});

  // A preset may add to tuning, envelopes, and the like, but the sample's
  // addresses, its loop mode, and the fixed key and velocity belong to the
  // instrument. The format says to ignore them here.
  builder.presets.push_back(BuilderPreset{"Meddling",
                                          0,
                                          0,
                                          {
                                              {{StartAddrsOffset, 50},
                                               {EndAddrsOffset, 50},
                                               {StartloopAddrsOffset, 50},
                                               {EndloopAddrsOffset, 50},
                                               {SampleModes, 2},
                                               {ExclusiveClass, 1},
                                               {OverridingRootKey, 10},
                                               {Keynum, 1},
                                               {Velocity, 1},
                                               {CoarseTune, 12},
                                               {Instrument, 0}},
                                          }});

  const SoundFont font = ReadOrFail(builder);
  const Region &region = font.FindPreset(0, 0)->regions.front();

  CHECK_EQ(region.start, 5);
  CHECK_EQ(region.end, 190);
  CHECK_EQ(region.loopStart, 23);
  CHECK_EQ(region.loopEnd, 176);
  CHECK_EQ(region.Value(SampleModes), 1);
  CHECK_EQ(region.Value(ExclusiveClass), 7);
  CHECK_EQ(region.rootKey, 64);
  CHECK_EQ(region.Value(Keynum), 50);
  CHECK_EQ(region.Value(Velocity), 90);
  CHECK_EQ(region.Value(CoarseTune), 12);
}

TEST_CASE(SoundFontSampleOffsetsAreAppliedAndChecked)
{
  // 40000 points, so that the coarse offsets, in units of 32768, have room
  SoundFontBuilder builder;
  builder.samples.push_back(BuilderSample{"Long", Ramp(40000, 0, 0), 100, 39000, 44100, 60, 0});
  builder.samples.push_back(BuilderSample{"Short", Ramp(50), 10, 40, 44100, 255, 0});

  builder.instruments.push_back(BuilderInstrument{
      "Offsets",
      {
          // Fine and coarse together: 32768 + 100 points in
          {{KeyRange, RangeAmount(0, 9)},
           {StartAddrsOffset, 100},
           {StartAddrsCoarseOffset, 1},
           {SampleModes, 1},
           {SampleId, 0}},
          // The end pulled in by a coarse step, and the loop end follows it
          {{KeyRange, RangeAmount(10, 19)},
           {EndAddrsCoarseOffset, -1},
           {SampleModes, 1},
           {SampleId, 0}},
          // A start after the end: nothing to play
          {{KeyRange, RangeAmount(20, 29)}, {StartAddrsCoarseOffset, 2}, {SampleId, 0}},
          // An end beyond the sample data
          {{KeyRange, RangeAmount(30, 39)}, {EndAddrsCoarseOffset, 1}, {SampleId, 1}},
          // A loop that ends before it starts: the sample plays without a loop
          {{KeyRange, RangeAmount(40, 49)},
           {StartloopAddrsOffset, 35},
           {SampleModes, 1},
           {SampleId, 1}},
          // The same on a zone that never asked to loop: nothing worth a note
          {{KeyRange, RangeAmount(50, 59)}, {StartloopAddrsOffset, 35}, {SampleId, 1}},
      }});
  builder.presets.push_back(BuilderPreset{"Offsets", 0, 0, {{{Instrument, 0}}}});

  const SoundFont font = ReadOrFail(builder);
  const Preset *preset = font.FindPreset(0, 0);
  REQUIRE(preset != nullptr);
  REQUIRE_EQ(preset->regions.size(), 4);

  CHECK_EQ(preset->regions[0].start, 32868);
  CHECK_EQ(preset->regions[0].end, 40000);
  // The loop began before the new start and is pulled in to it
  CHECK_EQ(preset->regions[0].loopStart, 32868);
  CHECK_EQ(preset->regions[0].loopEnd, 39000);

  CHECK_EQ(preset->regions[1].start, 0);
  CHECK_EQ(preset->regions[1].end, 40000 - 32768);
  CHECK_EQ(preset->regions[1].loopStart, 100);
  CHECK_EQ(preset->regions[1].loopEnd, 40000 - 32768);

  // "Short" starts after "Long" and its 46 points of silence
  CHECK_EQ(preset->regions[2].lowKey, 40);
  CHECK_EQ(preset->regions[2].start, 40046);
  CHECK_EQ(preset->regions[2].loopStart, 0);
  CHECK_EQ(preset->regions[2].loopEnd, 0);
  CHECK_EQ(preset->regions[3].lowKey, 50);
  CHECK_EQ(preset->regions[3].loopEnd, 0);

  // A sample without a pitch plays at its recorded pitch on the middle key
  CHECK_EQ(preset->regions[2].rootKey, 60);

  CHECK(font.FindRegions(0, 0, 25, 100).empty());
  CHECK(font.FindRegions(0, 0, 35, 100).empty());
  CHECK(HasNote(font, "2 zones were left out: their sample"));
  CHECK(HasNote(font, "1 zones play without their loop"));
  CHECK_EQ(font.notes.size(), 2);
}

TEST_CASE(SoundFontLeavesOutWhatItCannotPlay)
{
  SoundFontBuilder builder = Simple();
  builder.samples.push_back(BuilderSample{"In ROM", {}, 0, 0, 44100, 60, 0, 0x8001});
  builder.with24Bit = true; // in a file of version 2.01, which may not have it

  builder.instruments.push_back(
      BuilderInstrument{"Mixed",
                        {
                            {{KeyRange, RangeAmount(0, 40)}, {SampleId, 0}},
                            {{KeyRange, RangeAmount(41, 80)}, {SampleId, 1}},  // in ROM
                            {{KeyRange, RangeAmount(81, 127)}, {SampleId, 9}}, // no such sample
                            {{Pan, 100}}, // a zone without a sample, not the first: ignored
                        }});

  builder.presets.push_back(BuilderPreset{"Mixed",
                                          6,
                                          0,
                                          {
                                              {{Instrument, 1}},
                                              {{Instrument, 7}}, // no such instrument
                                          }});

  // Two presets on the same bank and program: the first one stays
  builder.presets.push_back(BuilderPreset{"Twin", 5, 0, {{{Instrument, 0}}}});

  // A drum kit, and a preset in a higher bank, out of order in the file
  builder.presets.push_back(BuilderPreset{"Kit", 0, PercussionBank, {{{Instrument, 0}}}});
  builder.presets.push_back(BuilderPreset{"Variation", 5, 8, {{{Instrument, 0}}}});

  const SoundFont font = ReadOrFail(builder);

  REQUIRE_EQ(font.samples.size(), 2);
  CHECK(font.samples[1].isRom);

  // Sorted by bank, then program
  REQUIRE_EQ(font.presets.size(), 4);
  CHECK_EQ(font.presets[0].name, "Ramp Preset");
  CHECK_EQ(font.presets[1].name, "Mixed");
  CHECK_EQ(font.presets[2].name, "Variation");
  CHECK_EQ(font.presets[3].name, "Kit");
  CHECK_EQ(font.presets[3].bank, 128);
  CHECK(font.FindPreset(PercussionBank, 0) != nullptr);
  CHECK_EQ(font.FindPreset(0, 5)->name, "Ramp Preset");

  // Of "Mixed", one zone can be played
  REQUIRE_EQ(font.FindPreset(0, 6)->regions.size(), 1);
  CHECK_EQ(font.FindPreset(0, 6)->regions.front().highKey, 40);

  // And the file says what was left out, and why
  CHECK(HasNote(font, "2 zones were left out: their sample is missing, in a sound card's ROM"));
  CHECK(HasNote(font, "1 zones were left out: they name an instrument"));
  CHECK(HasNote(font, "1 presets were left out"));
  CHECK(HasNote(font, "The 24-bit sample data was ignored, and only the 16-bit data is used: the "
                      "file says it is of version 2.01"));
  CHECK(font.lowBytes.empty());
  CHECK_EQ(font.notes.size(), 4);
}

TEST_CASE(SoundFontRefusesWhatIsNotASoundFont)
{
  const std::string good = Simple().build();
  REQUIRE(ReadBytes(good).has_value());

  ExpectRefused("", "not a SoundFont file: it is too short");
  ExpectRefused("RIFF", "too short");
  ExpectRefused(std::string(200, 'x'), "this is not a SoundFont file");

  // Another kind of RIFF file, and a MIDI file
  std::string wave = good;
  wave.replace(8, 4, "WAVE");
  ExpectRefused(wave, "this is not a SoundFont file");
  ExpectRefused(std::string("MThd") + std::string(100, '\0'), "this is not a SoundFont file");

  // A length field that says less than a header
  std::string tiny = good;
  tiny.replace(4, 4, std::string(4, '\0'));
  ExpectRefused(tiny, "this is not a SoundFont file");

  // Other versions of the format
  SoundFontBuilder newer = Simple();
  newer.versionMajor = 3;
  ExpectRefused(newer.build(), "version 3.01, but only version 2 (.sf2) can be read");

  SoundFontBuilder noVersion = Simple();
  noVersion.omit = {"ifil"};
  ExpectRefused(noVersion.build(), "does not say which version");

  // Bytes after the end of the RIFF data do no harm
  CHECK(ReadBytes(good + std::string(37, '!')).has_value());

  // The message names the file it was given
  const auto named = ReadBytes("nonsense", "Strings.sf2");
  REQUIRE(!named.has_value());
  CHECK(named.error().message.starts_with("Strings.sf2: "));
}

TEST_CASE(SoundFontRefusesEveryTruncatedFile)
{
  const std::string good = Fixture().build();
  REQUIRE(ReadBytes(good).has_value());

  // Cut anywhere, the file is refused, and says that it is incomplete
  for (std::size_t length = 0; length < good.size(); ++length)
  {
    const auto font = ReadBytes(good.substr(0, length));

    if (font.has_value())
    {
      test::ReportFailure(
          __FILE__, __LINE__,
          std::format("a file cut to {} of {} bytes was accepted", length, good.size()));
      break;
    }
  }

  ExpectRefused(good.substr(0, good.size() - 1), "the file is incomplete");
  ExpectRefused(good.substr(0, good.size() / 2), "an interrupted download or copy?");
}

TEST_CASE(SoundFontRefusesDamagedTables)
{
  // Each table left out
  for (const std::string_view table :
       {"phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"})
  {
    SoundFontBuilder builder = Simple();
    builder.omit = {std::string(table)};
    ExpectRefused(builder.build(), std::format("there is no '{}' table", table));
  }

  // A table that is not a whole number of records, or lacks its end marker
  SoundFontBuilder builder = Simple();
  builder.replace["shdr"] = std::string(47, '\0');
  ExpectRefused(builder.build(),
                "its 'shdr' table is damaged: 47 bytes are not a whole number of records of 46");

  builder = Simple();
  builder.replace["pgen"] = "";
  ExpectRefused(builder.build(), "its 'pgen' table is damaged");

  // Whole lists missing
  std::string good = Simple().build();
  std::string noTables = good;
  noTables.replace(noTables.find("pdta"), 4, "xdta");
  ExpectRefused(noTables, "it has no preset tables");

  std::string noInfo = good;
  noInfo.replace(noInfo.find("INFO"), 4, "XNFO");
  ExpectRefused(noInfo, "it has no information block");

  // No presets, or presets with nothing to play
  builder = Simple();
  builder.presets.clear();
  ExpectRefused(builder.build(), "it contains no presets");

  builder = Simple();
  builder.omit = {"smpl"};
  ExpectRefused(builder.build(), "it contains no sample data");

  builder = Simple();
  builder.instruments.front().zones = {{{SampleId, 3}}};
  ExpectRefused(builder.build(), "none of its presets can be played");

  // Indices that run backwards or off the end of the next table. The
  // tables of the simple font by hand: one preset whose zones start at
  // bag 5 of 2, and one instrument whose zone starts at generator 9 of 2.
  builder = Simple();
  std::string headers = builder.build();
  const std::size_t presetBag = headers.find("phdr") + 8 + 24;
  headers[presetBag] = 5;
  ExpectRefused(headers, "preset 0 points outside the zone table");

  std::string bags = builder.build();
  const std::size_t instrumentGenerator = bags.find("ibag") + 8;
  bags[instrumentGenerator] = 9;
  ExpectRefused(bags, "a zone of instrument 0 points outside the generator table");

  std::string backwards = builder.build();
  const std::size_t endMarker = backwards.find("phdr") + 8 + 38 + 24;
  backwards[presetBag] = 1;
  backwards[endMarker] = 0;
  ExpectRefused(backwards, "preset 0 points outside the zone table");

  // A chunk inside a list that claims more bytes than the list has
  std::string overrun = builder.build();
  const std::size_t tableSize = overrun.find("shdr") + 4;
  overrun[tableSize + 1] = 0x7F;
  ExpectRefused(overrun, "its preset tables are damaged");
}

TEST_CASE(SoundFontSurvivesEveryDamagedByte)
{
  // No damage to a single byte may crash the reader or make it hang: the
  // file is refused, or what is left of it is read.
  const std::string good = Fixture().build();
  std::size_t refused = 0;
  std::size_t accepted = 0;

  for (std::size_t at = 0; at < good.size(); ++at)
  {
    for (const int value : {0x00, 0x7F, 0x80, 0xFF})
    {
      if (static_cast<unsigned char>(good[at]) == value)
      {
        continue;
      }

      std::string damaged = good;
      damaged[at] = static_cast<char>(static_cast<unsigned char>(value));
      const auto font = ReadBytes(damaged);

      if (font.has_value())
      {
        ++accepted;

        // What was read holds together: every region plays inside the data
        for (const Preset &preset : font->presets)
        {
          for (const Region &region : preset.regions)
          {
            CHECK(region.start < region.end && region.end <= font->data.size());
            CHECK(region.loopStart <= region.loopEnd && region.loopEnd <= region.end);
            CHECK(region.sample < font->samples.size());
            CHECK(region.lowKey <= region.highKey && region.highKey <= 127);
            CHECK(region.modulators.size() >= DefaultModulators.size());
          }
        }
      }
      else
      {
        ++refused;
        CHECK(font.error().message.starts_with("test.sf2: "));
      }
    }
  }

  // Both happen: damage to the structure is refused, damage to a name or
  // to a sample point is not damage the reader can know about.
  CHECK(refused > 100);
  CHECK(accepted > 100);
}

TEST_CASE(SoundFontReadsLargeSampleDataInBlocks)
{
  // More than the reader's block of a megabyte, and an odd number of
  // points, so that the last block is a short one.
  SoundFontBuilder builder = Simple();
  builder.samples.front().points = Ramp(700'001, -20000, 0);
  builder.samples.front().points[0] = 123;
  builder.samples.front().points[524'288] = -456;
  builder.samples.front().points[700'000] = 789;
  builder.samples.front().loopStart = 1000;
  builder.samples.front().loopEnd = 700'000;

  const SoundFont font = ReadOrFail(builder);

  REQUIRE_EQ(font.data.size(), 700'001 + 46);
  CHECK_EQ(font.data[0], 123);
  CHECK_EQ(font.data[1], -20000);
  CHECK_EQ(font.data[524'287], -20000);
  CHECK_EQ(font.data[524'288], -456);
  CHECK_EQ(font.data[700'000], 789);
  CHECK_EQ(font.data[700'001], 0);
  CHECK_EQ(font.FindPreset(0, 5)->regions.front().end, 700'001);

  // Sample data with half a point at its end: the half is left out
  builder = Simple();
  std::string pool(201, '\0');
  pool[0] = 0x34;
  pool[1] = 0x12;
  builder.replace["smpl"] = pool;
  const SoundFont odd = ReadOrFail(builder);
  REQUIRE_EQ(odd.data.size(), 100);
  CHECK_EQ(odd.data[0], 0x1234);
}

TEST_CASE(SoundFontLooksLikeARealSoundSet)
{
  const SoundFont font = ReadOrFail(Fixture());

  CHECK_EQ(font.info.name, "havSF2 test set");
  CHECK_EQ(font.info.author, "havSF2 tests");
  CHECK_EQ(font.samples.size(), 3);
  CHECK_EQ(font.instrumentNames, (std::vector<std::string>{"Piano", "Kit"}));
  REQUIRE_EQ(font.presets.size(), 2);

  // The piano: two key ranges, each with a soft and a loud layer
  const Preset *piano = font.FindPreset(0, 0);
  REQUIRE(piano != nullptr);
  CHECK_EQ(piano->regions.size(), 4);

  const auto soft = font.FindRegions(0, 0, 48, 64);
  const auto loud = font.FindRegions(0, 0, 48, 100);
  const auto high = font.FindRegions(0, 0, 72, 100);
  REQUIRE_EQ(soft.size(), 1);
  REQUIRE_EQ(loud.size(), 1);
  REQUIRE_EQ(high.size(), 1);

  CHECK_EQ(soft.front()->sample, 0);
  CHECK_EQ(soft.front()->Value(InitialAttenuation), 60);
  CHECK_EQ(loud.front()->Value(InitialAttenuation), 0);
  CHECK_EQ(high.front()->sample, 1);
  CHECK_EQ(high.front()->Value(Pan), 100);
  CHECK_EQ(high.front()->rootKey, 72);

  // The instrument's global zone reaches every region of it
  for (const Region &region : piano->regions)
  {
    CHECK_EQ(region.Value(ReleaseVolEnv), -2000);
    CHECK_EQ(region.Value(SampleModes), 1);
    CHECK(region.loopStart < region.loopEnd);

    // So does its modulator, after the ten defaults
    REQUIRE_EQ(region.modulators.size(), 11);
    CHECK_EQ(region.modulators.back().destination, static_cast<std::uint16_t>(InitialFilterFc));
    CHECK_EQ(region.modulators.back().amount, 1200);
  }

  // The preset makes its soft layer react less to velocity
  CHECK_EQ(soft.front()->modulators.front().amount, 960 - 480);
  CHECK_EQ(loud.front()->modulators.front().amount, 960);

  // The kit: one drum on one key, in the percussion bank
  const auto kick = font.FindRegions(PercussionBank, 0, 36, 100);
  REQUIRE_EQ(kick.size(), 1);
  CHECK_EQ(kick.front()->Value(ExclusiveClass), 1);
  CHECK_EQ(kick.front()->rootKey, 60);
  CHECK_EQ(kick.front()->loopEnd, 0);
  CHECK(kick.front()->modulators ==
        std::vector<Modulator>(DefaultModulators.begin(), DefaultModulators.end()));

  // 24 bits: a low byte for every point
  CHECK_EQ(font.lowBytes.size(), font.data.size());
  CHECK(font.FindRegions(PercussionBank, 0, 38, 100).empty());
  CHECK(font.notes.empty());
}

TEST_CASE(SoundFontSourcesDecode)
{
  // Velocity, concave, from loud to soft: the default attenuation source
  CHECK(DecodeSource(0x0502) == (Source{2, false, true, false, SourceType::Concave}));
  // Controller 10, linear, -1..1: the default pan source
  CHECK(DecodeSource(0x028A) == (Source{10, true, false, true, SourceType::Linear}));
  // The mod wheel, and a switch
  CHECK(DecodeSource(0x0081) == (Source{1, true, false, false, SourceType::Linear}));
  CHECK_EQ(DecodeSource(0x0C02).type, SourceType::Switch);
  CHECK_EQ(DecodeSource(0x020E).index, static_cast<std::uint8_t>(Controller::PitchWheel));

  // Identical means everything but the amount
  CHECK(DefaultModulators[0].IsIdenticalTo(WithAmount(DefaultModulators[0], 1)));
  CHECK(!DefaultModulators[0].IsIdenticalTo(DefaultModulators[4]));
  Modulator transformed = DefaultModulators[0];
  transformed.transform = static_cast<std::uint16_t>(Transform::AbsoluteValue);
  CHECK(!DefaultModulators[0].IsIdenticalTo(transformed));
}

TEST_CASE(SoundFontModulatorsResolveLikeTheFormatSays)
{
  const Modulator breath{0x0082, static_cast<std::uint16_t>(InitialAttenuation), 100, 0, 0};
  const Modulator keyToPan{0x0003, static_cast<std::uint16_t>(Pan), 200, 0, 0};
  const Modulator pressure{0x000A, static_cast<std::uint16_t>(VibLfoToPitch), 10, 0, 0};
  const Modulator reverb{0x0083, static_cast<std::uint16_t>(ReverbEffectsSend), 300, 0, 0};

  SoundFontBuilder builder;
  builder.samples.push_back(BuilderSample{"A", Ramp(100), 0, 0, 44100, 60, 0});

  builder.instruments.push_back(
      BuilderInstrument{"Modulated",
                        {
                            GeneratorList{},
                            {{KeyRange, RangeAmount(0, 63)}, {SampleId, 0}},
                            {{KeyRange, RangeAmount(64, 127)}, {SampleId, 0}},
                        },
                        {
                            // The global zone replaces a default and adds two of its own
                            {WithAmount(DefaultModulators[0], 480), breath, keyToPan},
                            // A zone replaces one of the global zone's and a default, and adds one.
                            // Of two identical modulators in one zone, the later counts.
                            {WithAmount(breath, 50), WithAmount(DefaultModulators[5], 500),
                             pressure, WithAmount(pressure, 20)},
                        }});

  builder.presets.push_back(
      BuilderPreset{"Modulated",
                    0,
                    0,
                    {
                        GeneratorList{},
                        {{Instrument, 0}},
                    },
                    {
                        // The preset adds to identical modulators of the instrument, and its
                        // zone replaces what its global zone says.
                        {WithAmount(DefaultModulators[0], 100), reverb},
                        {WithAmount(reverb, 50), WithAmount(DefaultModulators[3], 25)},
                    }});

  const SoundFont font = ReadOrFail(builder);
  const auto low = font.FindRegions(0, 0, 30, 100);
  const auto high = font.FindRegions(0, 0, 100, 100);
  REQUIRE_EQ(low.size(), 1);
  REQUIRE_EQ(high.size(), 1);

  const auto amount = [](const Region *region, const Modulator &modulator)
  {
    const Modulator *found = FindIdentical(*region, modulator);
    return found != nullptr ? found->amount : std::int16_t{-1};
  };

  for (const Region *region : {low.front(), high.front()})
  {
    CHECK_EQ(amount(region, DefaultModulators[0]), 480 + 100);
    CHECK_EQ(amount(region, DefaultModulators[3]), 50 + 25);
    CHECK_EQ(amount(region, DefaultModulators[4]), 960);
    CHECK_EQ(amount(region, keyToPan), 200);
    CHECK_EQ(amount(region, reverb), 50);
  }

  CHECK_EQ(amount(low.front(), breath), 50);
  CHECK_EQ(amount(high.front(), breath), 100);
  CHECK_EQ(amount(low.front(), DefaultModulators[5]), 500);
  CHECK_EQ(amount(high.front(), DefaultModulators[5]), 1000);
  CHECK_EQ(amount(low.front(), pressure), 20);
  CHECK(FindIdentical(*high.front(), pressure) == nullptr);

  // The defaults first, then what each level adds, each once
  REQUIRE_EQ(low.front()->modulators.size(), 14);
  CHECK(low.front()->modulators[10] == WithAmount(breath, 50));
  CHECK(low.front()->modulators[11] == keyToPan);
  CHECK(low.front()->modulators[12] == WithAmount(pressure, 20));
  CHECK(low.front()->modulators[13] == WithAmount(reverb, 50));
  CHECK_EQ(high.front()->modulators.size(), 13);
  CHECK(font.notes.empty());
}

TEST_CASE(SoundFontLeavesOutModulatorsItCannotUse)
{
  const auto pan = static_cast<std::uint16_t>(Pan);

  SoundFontBuilder builder = Simple();
  builder.instruments.front().modulators = {{
      // Not defined by the format: nine
      {0x0005, pan, 10, 0, 0}, // general controller 5
      {0x0080, pan, 10, 0, 0}, // MIDI controller 0, bank select
      {0x00A8, pan, 10, 0, 0}, // controller 40, the LSB of 8
      {0x00F8, pan, 10, 0, 0}, // controller 120, a channel mode message
      {0x1002, pan, 10, 0, 0}, // source type 4
      {0x0002, pan, 10, 0, 1}, // transform 1
      {0x0002, 14, 10, 0, 0},  // generator 14 is unused
      {0x0002, static_cast<std::uint16_t>(KeyRange), 10, 0, 0}, // ranges are not modulated
      {0x0002, pan, 10, 0x0086, 0}, // amount source controller 6, data entry
      // Linked: three
      {0x0002, 0x8001, 10, 0, 0},
      {0x007F, pan, 10, 0, 0},
      {0x0002, pan, 10, 0x007F, 0},
      // Usable: three
      {0x0002, pan, 10, 0, static_cast<std::uint16_t>(Transform::AbsoluteValue)},
      {0x0C02, static_cast<std::uint16_t>(InitialFilterQ), 10, 0, 0},
      {0x0081, InitialPitch, 10, 0, 0},
  }};

  const SoundFont font = ReadOrFail(builder);
  const Region &region = font.FindPreset(0, 5)->regions.front();

  CHECK_EQ(region.modulators.size(), DefaultModulators.size() + 3);
  CHECK(HasNote(font, "9 modulators were left out: their source, destination, or transform is not "
                      "one the format defines."));
  CHECK(HasNote(font, "3 modulators were left out: they are linked to other modulators"));
  CHECK_EQ(font.notes.size(), 2);
}

TEST_CASE(SoundFontZonesKeepTheirSoundWithoutTheirModulators)
{
  SoundFontBuilder builder = Simple();
  builder.instruments.front().modulators = {
      {Modulator{0x0081, static_cast<std::uint16_t>(Pan), 10, 0, 0}}};
  std::string bytes = builder.build();

  // The zone's first modulator, by hand: 9, after its last (1)
  bytes[bytes.find("ibag") + 8 + 2] = 9;

  const auto font = ReadBytes(bytes);
  REQUIRE(font.has_value());
  const Region &region = font->FindPreset(0, 5)->regions.front();
  CHECK(region.modulators ==
        std::vector<Modulator>(DefaultModulators.begin(), DefaultModulators.end()));
  CHECK(HasNote(*font, "1 zones lost their modulators: they point outside the modulator table."));
  CHECK_EQ(font->notes.size(), 1);
}

TEST_CASE(SoundFontReads24BitDataWhereItFits)
{
  SoundFontBuilder builder = Simple();
  builder.versionMinor = 4;
  builder.with24Bit = true;

  const SoundFont font = ReadOrFail(builder);
  REQUIRE_EQ(font.lowBytes.size(), font.data.size());

  for (std::size_t point = 0; point < font.lowBytes.size(); ++point)
  {
    if (font.lowBytes[point] != point % 251)
    {
      CHECK_EQ(font.lowBytes[point], point % 251);
      break;
    }
  }

  CHECK(font.notes.empty());

  // An odd number of points: the chunk is padded to an even size
  builder.samples.front().points = Ramp(101);
  const SoundFont odd = ReadOrFail(builder);
  CHECK_EQ(odd.data.size(), 147);
  CHECK_EQ(odd.lowBytes.size(), 147);
  CHECK(odd.notes.empty());

  // A chunk of the wrong size is ignored, and so is one in an older file
  builder = Simple();
  builder.versionMinor = 4;
  builder.replace["sm24"] = std::string(10, '\x01');
  builder.with24Bit = true;
  const SoundFont wrong = ReadOrFail(builder);
  CHECK(wrong.lowBytes.empty());
  CHECK(HasNote(wrong, "The 24-bit sample data was ignored, and only the 16-bit data is used: it "
                       "has 10 bytes for 146 points."));

  builder = Simple();
  builder.versionMinor = 3;
  builder.with24Bit = true;
  const SoundFont older = ReadOrFail(builder);
  CHECK(older.lowBytes.empty());
  CHECK(HasNote(older, "the file says it is of version 2.03, and 24-bit data needs 2.04"));

  // The data stays where the 16-bit data says. Either order of the two
  // chunks is read.
  builder = Simple();
  builder.versionMinor = 4;
  builder.with24Bit = true;
  const std::string normal = builder.build();
  const std::size_t smpl = normal.find("smpl");
  const std::size_t sm24 = normal.find("sm24");
  const std::size_t sm24End = sm24 + 8 + 146;
  const std::string swapped = normal.substr(0, smpl) + normal.substr(sm24, sm24End - sm24) +
                              normal.substr(smpl, sm24 - smpl) + normal.substr(sm24End);
  const auto reordered = ReadBytes(swapped);
  REQUIRE(reordered.has_value());
  CHECK(reordered->data == font.data);
  CHECK(reordered->lowBytes == font.lowBytes);
}

TEST_CASE(SoundFontLoadsFromAFile)
{
  // A name outside ASCII, to be sure paths are handled as Unicode
  const std::filesystem::path file = ScratchFolder() / std::filesystem::path(u8"Flügel ♫.sf2");
  const std::string bytes = Fixture().build();

  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
    REQUIRE(out.good());
  }

  const auto font = Load(file);
  REQUIRE(font.has_value());
  CHECK_EQ(font->presets.size(), 2);
  CHECK(font->data == ReadBytes(bytes)->data);

  // A file that is not there, and a file that is something else. The
  // message has the file's name without its folder.
  const auto missing = Load(ScratchFolder() / "no-such-file.sf2");
  REQUIRE(!missing.has_value());
  CHECK(missing.error().message.starts_with("no-such-file.sf2: "));
  CHECK(missing.error().message.find("cannot be opened") != std::string::npos);

  const std::filesystem::path text = ScratchFolder() / "notes.sf2";

  {
    std::ofstream out(text, std::ios::binary | std::ios::trunc);
    out << "these are not the samples you are looking for";
  }

  const auto wrong = Load(text);
  REQUIRE(!wrong.has_value());
  CHECK(wrong.error().message.starts_with("notes.sf2: this is not a SoundFont file"));

  std::filesystem::remove(file);
  std::filesystem::remove(text);
}

TEST_CASE(UnitsConvertToSecondsGainAndRatio)
{
  const auto near = [](double actual, double expected)
  {
    return std::abs(actual - expected) <= 1e-9 * std::max(1.0, std::abs(expected));
  };

  CHECK(near(TimecentsToSeconds(0), 1.0));
  CHECK(near(TimecentsToSeconds(1200), 2.0));
  CHECK(near(TimecentsToSeconds(-1200), 0.5));
  CHECK(near(TimecentsToSeconds(-12000), 1.0 / 1024.0));
  CHECK_EQ(TimecentsToSeconds(-32768), 0.0);

  CHECK(near(CentibelsToGain(0), 1.0));
  CHECK(near(CentibelsToGain(200), 0.1));
  CHECK(near(CentibelsToGain(-200), 10.0));
  CHECK(near(CentibelsToGain(1440), std::pow(10.0, -7.2)));

  CHECK(near(CentsToRatio(0), 1.0));
  CHECK(near(CentsToRatio(1200), 2.0));
  CHECK(near(CentsToRatio(-2400), 0.25));
  CHECK(near(CentsToRatio(700), std::exp2(7.0 / 12.0)));
}
