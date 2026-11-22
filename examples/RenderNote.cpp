// RenderNote: plays one note of a SoundFont into a WAV file.
//
//   RenderNote <font.sf2> <out.wav> [bank] [program] [key] [velocity] [seconds]
//
//   RenderNote GeneralUser-GS.sf2 piano.wav            middle C on the piano
//   RenderNote GeneralUser-GS.sf2 snare.wav 128 0 38   a snare from the drum kit
//
// It shows what to do with the regions havSF2 gives you: find the ones for
// the note, work out each one's pitch and loudness from its generators, and
// play its sample points with the loop and the volume envelope. The note is
// held for `seconds` (default 2) and then released.
//
// It is an example, not a synthesizer. Left out on purpose: the low-pass
// filter, the modulation envelope, both LFOs, modulators that depend on MIDI
// controllers or the pitch wheel (a fresh channel would have them at rest
// anyway), chorus, reverb, and anything better than linear interpolation.

#include "havSF2.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  constexpr int OutputRate = 44100;

  // How long a release may ring on at most in seconds
  constexpr double LongestRelease = 10.0;

  // The volume envelope runs in decibels: the decay and release times are
  // what it takes to fall from full volume by 100 dB (1000 centibels),
  // which the specification takes as silence. The attack rises linearly
  // in amplitude.
  constexpr double Silence = 1000.0;

  // ---- MODULATORS: only those driven by the note itself ----

  // The concave curve: 0 at 0, 1 at 1, rising slowly at first, the way
  // loudness is heard. The specification describes its shape only roughly,
  // so this is the form synthesizers commonly use.
  double Concave(double x)
  {
    if (x >= 1.0)
    {
      return 1.0;
    }

    return std::clamp(-20.0 / 96.0 * std::log10((1.0 - x) * (1.0 - x)), 0.0, 1.0);
  }

  double Curve(havSF2::SourceType type, double x)
  {
    switch (type)
    {
    case havSF2::SourceType::Concave:
      return Concave(x);
    case havSF2::SourceType::Convex:
      return 1.0 - Concave(1.0 - x);
    case havSF2::SourceType::Switch:
      return x >= 0.5 ? 1.0 : 0.0;
    default:
      return x;
    }
  }

  // A source's value for a note, or nothing if it depends on something
  // other than the note (a controller, the pitch wheel, ...).
  std::optional<double> SourceValue(std::uint16_t encoded, int key, int velocity)
  {
    const havSF2::Source source = havSF2::DecodeSource(encoded);

    if (source.midiController)
    {
      return std::nullopt;
    }

    // A MIDI value from 0 to 127 is taken as 0 to 127/128
    double x = 0.0;

    switch (static_cast<havSF2::Controller>(source.index))
    {
    case havSF2::Controller::NoController:
      return 1.0;
    case havSF2::Controller::NoteOnVelocity:
      x = velocity / 128.0;
      break;
    case havSF2::Controller::NoteOnKey:
      x = key / 128.0;
      break;
    default:
      return std::nullopt;
    }

    if (source.negative)
    {
      x = 1.0 - x;
    }

    if (!source.bipolar)
    {
      return Curve(source.type, x);
    }

    // Bipolar: the curve mirrored around the middle from -1 to 1
    const double centered = 2.0 * x - 1.0;

    if (source.type == havSF2::SourceType::Switch)
    {
      return centered >= 0.0 ? 1.0 : -1.0;
    }

    return std::copysign(Curve(source.type, std::abs(centered)), centered);
  }

  // The region's generators with the note's modulators added
  std::vector<double> ModulatedValues(const havSF2::Region &region, int key, int velocity)
  {
    std::vector<double> values(region.generators.begin(), region.generators.end());

    for (const havSF2::Modulator &modulator : region.modulators)
    {
      if (modulator.destination >= havSF2::GeneratorCount)
      {
        continue; // InitialPitch: the pitch wheel, which is at rest
      }

      const std::optional<double> source = SourceValue(modulator.source, key, velocity);
      const std::optional<double> amountSource = SourceValue(modulator.amountSource, key, velocity);

      if (!source || !amountSource)
      {
        continue;
      }

      double change = modulator.amount * *source * *amountSource;

      if (modulator.transform == static_cast<std::uint16_t>(havSF2::Transform::AbsoluteValue))
      {
        change = std::abs(change);
      }

      values[modulator.destination] += change;
    }

    return values;
  }

  // ---- One VOICE per region ----

  struct Voice
  {
    const havSF2::Region *region = nullptr;
    double position = 0.0; // in sample points, from SoundFont::data's start
    double step = 1.0;     // sample points per output frame
    bool loops = false;
    bool loopsInRelease = false;
    double gain = 1.0; // the initial attenuation
    double left = 0.0;
    double right = 0.0;

    // The volume envelope in seconds and centibels
    double delay = 0.0;
    double attack = 0.0;
    double hold = 0.0;
    double decay = 0.0;
    double sustain = 0.0;
    double release = 0.0;

    bool finished = false;
  };

  Voice
  StartVoice(const havSF2::SoundFont &font, const havSF2::Region &region, int key, int velocity)
  {
    using enum havSF2::Generator;

    // A zone can force the key and velocity it plays with
    if (region.Value(Keynum) >= 0)
    {
      key = region.Value(Keynum);
    }

    if (region.Value(Velocity) >= 0)
    {
      velocity = region.Value(Velocity);
    }

    const std::vector<double> values = ModulatedValues(region, key, velocity);
    const auto value = [&values](havSF2::Generator generator)
    {
      return values[static_cast<std::size_t>(generator)];
    };

    const havSF2::Sample &sample = font.samples[region.sample];

    Voice voice;
    voice.region = &region;
    voice.position = region.start;

    // Pitch: the distance from the root key in the zone's scale, plus the
    // tuning and the sample's own correction.
    const double cents = (key - region.rootKey) * value(ScaleTuning) + value(CoarseTune) * 100.0 +
                         value(FineTune) + sample.pitchCorrection;
    voice.step = havSF2::CentsToRatio(cents) * sample.sampleRate / OutputRate;

    // SampleModes: 1 loops for good, 3 loops until the key is released
    const int mode = region.Value(SampleModes) & 3;
    const bool hasLoop = region.loopStart < region.loopEnd;
    voice.loops = hasLoop && (mode == 1 || mode == 3);
    voice.loopsInRelease = hasLoop && mode == 1;

    voice.gain = havSF2::CentibelsToGain(std::max(0.0, value(InitialAttenuation)));

    // Pan from -500 (left) to 500 (right) tenths of a percent at equal power
    const double pan =
        (std::clamp(value(Pan), -500.0, 500.0) / 1000.0 + 0.5) * std::numbers::pi / 2.0;
    voice.left = std::cos(pan);
    voice.right = std::sin(pan);

    const auto seconds = [](double timecents)
    {
      return havSF2::TimecentsToSeconds(static_cast<int>(std::lround(timecents)));
    };

    voice.delay = seconds(value(DelayVolEnv));
    voice.attack = seconds(value(AttackVolEnv));
    voice.hold = seconds(value(HoldVolEnv) + value(KeynumToVolEnvHold) * (60 - key));
    voice.decay = seconds(value(DecayVolEnv) + value(KeynumToVolEnvDecay) * (60 - key));
    voice.sustain = std::clamp(value(SustainVolEnv), 0.0, Silence);
    voice.release = seconds(value(ReleaseVolEnv));

    return voice;
  }

  // The envelope while the key is held as an amplitude factor
  double HeldLevel(const Voice &voice, double time)
  {
    time -= voice.delay;

    if (time < 0.0)
    {
      return 0.0;
    }

    if (time < voice.attack)
    {
      return time / voice.attack;
    }

    time -= voice.attack + voice.hold;

    if (time < 0.0)
    {
      return 1.0;
    }

    const double fallen = voice.decay > 0.0 ? Silence * time / voice.decay : Silence;
    return havSF2::CentibelsToGain(std::min(fallen, voice.sustain));
  }

  // The envelope at a time with the key released at `releasedAt`
  double Level(const Voice &voice, double time, double releasedAt)
  {
    if (time < releasedAt)
    {
      return HeldLevel(voice, time);
    }

    const double fallen =
        voice.release > 0.0 ? Silence * (time - releasedAt) / voice.release : Silence;
    return HeldLevel(voice, releasedAt) * havSF2::CentibelsToGain(fallen);
  }

  // One sample point from -1 to 1 with the low byte if the file has one
  double Point(const havSF2::SoundFont &font, std::size_t index)
  {
    if (font.lowBytes.empty())
    {
      return font.data[index] / 32768.0;
    }

    return (font.data[index] * 256 + font.lowBytes[index]) / 8388608.0;
  }

  // The next point of a voice or 0 once it has played to the end
  double Next(const havSF2::SoundFont &font, Voice &voice, bool released)
  {
    const havSF2::Region &region = *voice.region;
    const bool looping = voice.loops && (!released || voice.loopsInRelease);

    if (looping)
    {
      while (voice.position >= region.loopEnd)
      {
        voice.position -= region.loopEnd - region.loopStart;
      }
    }

    const auto index = static_cast<std::size_t>(voice.position);
    std::size_t following = index + 1;

    if (looping && following == region.loopEnd)
    {
      following = region.loopStart;
    }
    else if (following >= region.end)
    {
      voice.finished = true;
      return 0.0;
    }

    // Linear interpolation between the two points around the position
    const double fraction = voice.position - static_cast<double>(index);
    const double point =
        Point(font, index) + (Point(font, following) - Point(font, index)) * fraction;

    voice.position += voice.step;
    return point;
  }

  void WriteLittleEndian(std::ofstream &file, std::uint32_t value, int bytes)
  {
    for (int byte = 0; byte < bytes; ++byte)
    {
      file.put(static_cast<char>((value >> (8 * byte)) & 0xFF));
    }
  }

  bool WriteWav(const std::string &path, const std::vector<double> &frames)
  {
    std::ofstream file(path, std::ios::binary);

    const auto dataSize = static_cast<std::uint32_t>(frames.size() * 2);

    file.write("RIFF", 4);
    WriteLittleEndian(file, 36 + dataSize, 4);
    file.write("WAVEfmt ", 8);
    WriteLittleEndian(file, 16, 4);                 // format chunk size
    WriteLittleEndian(file, 1, 2);                  // PCM
    WriteLittleEndian(file, 2, 2);                  // stereo
    WriteLittleEndian(file, OutputRate, 4);         // frames per second
    WriteLittleEndian(file, OutputRate * 2 * 2, 4); // bytes per second
    WriteLittleEndian(file, 4, 2);                  // bytes per frame
    WriteLittleEndian(file, 16, 2);                 // bits per sample
    file.write("data", 4);
    WriteLittleEndian(file, dataSize, 4);

    for (const double sample : frames)
    {
      const long value = std::lround(std::clamp(sample, -1.0, 1.0) * 32767.0);
      WriteLittleEndian(file, static_cast<std::uint32_t>(value) & 0xFFFF, 2);
    }

    return static_cast<bool>(file);
  }

  std::optional<int> Number(std::string_view text, int low, int high)
  {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (error != std::errc{} || end != text.data() + text.size() || value < low || value > high)
    {
      return std::nullopt;
    }

    return value;
  }
} // namespace

int main(int argc, char **argv)
{
  if (argc < 3 || argc > 8)
  {
    std::cerr
        << "Usage: RenderNote <font.sf2> <out.wav> [bank] [program] [key] [velocity] [seconds]\n";
    return 2;
  }

  // The defaults: middle C on the first piano, fairly loud, held 2 seconds
  const std::array<int, 5> defaults{0, 0, 60, 100, 2};
  const std::array<std::pair<int, int>, 5> limits{
      {{0, 128}, {0, 127}, {0, 127}, {1, 127}, {1, 60}}};
  const std::array<std::string_view, 5> names{"bank", "program", "key", "velocity", "seconds"};
  std::array<int, 5> settings = defaults;

  for (int argument = 3; argument < argc; ++argument)
  {
    const auto which = static_cast<std::size_t>(argument - 3);
    const std::optional<int> value =
        Number(argv[argument], limits[which].first, limits[which].second);

    if (!value)
    {
      std::cerr << std::format("The {} must be a whole number from {} to {}.\n", names[which],
                               limits[which].first, limits[which].second);
      return 2;
    }

    settings[which] = *value;
  }

  const auto [bank, program, key, velocity, held] = settings;

  const auto font = havSF2::Load(argv[1]);

  if (!font)
  {
    std::cerr << font.error().message << '\n';
    return 1;
  }

  for (const std::string &note : font->notes)
  {
    std::cout << "Note: " << note << '\n';
  }

  const havSF2::Preset *preset =
      font->FindPreset(static_cast<std::uint16_t>(bank), static_cast<std::uint16_t>(program));

  if (preset == nullptr)
  {
    std::cerr << std::format("The SoundFont has no preset in bank {}, program {}.\n", bank,
                             program);
    return 1;
  }

  std::vector<Voice> voices;

  for (const havSF2::Region *region :
       font->FindRegions(static_cast<std::uint16_t>(bank), static_cast<std::uint16_t>(program),
                         static_cast<std::uint8_t>(key), static_cast<std::uint8_t>(velocity)))
  {
    voices.push_back(StartVoice(*font, *region, key, velocity));
  }

  if (voices.empty())
  {
    std::cerr << std::format("\"{}\" has nothing to play for key {} at velocity {}.\n",
                             preset->name, key, velocity);
    return 1;
  }

  // Play until every voice has finished or faded out or the release has
  // rung on for long enough.
  const double releasedAt = held;
  const auto lastFrame = static_cast<std::size_t>((held + LongestRelease) * OutputRate);
  const double inaudible = havSF2::CentibelsToGain(Silence);

  std::vector<double> frames; // left, right, left, right, ...
  double peak = 0.0;

  for (std::size_t frame = 0; frame < lastFrame; ++frame)
  {
    const double time = static_cast<double>(frame) / OutputRate;
    const bool released = time >= releasedAt;
    double left = 0.0;
    double right = 0.0;
    bool sounding = false;

    for (Voice &voice : voices)
    {
      if (voice.finished)
      {
        continue;
      }

      const double level = Level(voice, time, releasedAt);

      if (released && level < inaudible)
      {
        voice.finished = true;
        continue;
      }

      const double point = Next(*font, voice, released) * level * voice.gain;
      left += point * voice.left;
      right += point * voice.right;
      sounding = true;
    }

    if (!sounding && released)
    {
      break;
    }

    frames.push_back(left);
    frames.push_back(right);
    peak = std::max({peak, std::abs(left), std::abs(right)});
  }

  if (!WriteWav(argv[2], frames))
  {
    std::cerr << std::format("{} could not be written.\n", argv[2]);
    return 1;
  }

  std::cout << std::format(
      "\"{}\", key {} at velocity {}: {} regions, {:.2f} seconds, peak {:.2f}{}\n", preset->name,
      key, velocity, voices.size(), static_cast<double>(frames.size() / 2) / OutputRate, peak,
      peak > 1.0 ? " (clipped)" : "");

  return 0;
}
