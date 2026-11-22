# Reading a SoundFont

How havSF2 reads SoundFont 2 files (`.sf2`), what it produces, and what of
the format it leaves out.

## What a SoundFont is

A SoundFont is a set of recorded instrument sounds with the rules for
playing them: which recording to use for which key and how hard it was
struck, where it loops, how fast it fades. A General MIDI SoundFont has one
_preset_ for each of the 128 General MIDI programs and drum kits in bank
128, so that a MIDI file sounds as intended with it.

The file has three parts:

| Part   | Contains                                                                     |
| ------ | ---------------------------------------------------------------------------- |
| `INFO` | The format version, the name of the sound set, its author, and the like      |
| `sdta` | The sample data: every recording, end to end, as 16-bit mono points          |
| `pdta` | Nine tables that build presets from instruments and instruments from samples |

The tables form a hierarchy:

```text
preset ── zones ──► instrument ── zones ──► sample
          (for which keys and           (for which keys and
           velocities, and what          velocities, with which
           to add)                       settings)
```

Each zone carries _generators_: numbered settings such as the attack time
of the volume envelope, the pan position, or the filter's cutoff. An
instrument or a preset may have a _global zone_ whose generators apply to
all of its other zones.

## What the loader produces

The hierarchy is resolved once, when the file is read. The result is flat:

```text
SoundFont
  info             name, author, ...
  samples          name, position in the data, loop, sample rate, root key
  data             the 16-bit points of all samples
  lowBytes         for a 24-bit file, the low 8 bits of every point
  instrumentNames  the instruments the presets were built from, for display
  presets          by bank and program, each a list of regions
  notes            what was left out, in words
```

A **region** is one sample of a preset with everything needed to play it:
its key and velocity range, where in the data it starts, ends, and loops,
the key it is recorded at, the value of every generator, and the
modulators that apply to it.
`SoundFont::FindRegions(bank, program, key, velocity)` returns the regions
to sound for a note. Nothing has to be looked up after that.

How a generator's value is found for a region made by a preset zone from
an instrument zone:

1. The instrument zone's value, else the instrument's global zone's, else
   the format's default (`havSF2::DefaultValue`)
2. Plus the preset zone's value, else the preset's global zone's, else
   nothing

The preset adds to the instrument's values instead of replacing them. A few
generators belong to the instrument alone and are ignored in presets, as
the format asks: the sample's start, end, and loop offsets, the loop mode,
the fixed key and velocity, the exclusive class, and the root key override.
Sums beyond the range of a 16-bit value are limited to it.

Key and velocity ranges work differently: a region plays where the
preset's range and the instrument's range both allow it. A zone whose
ranges have nothing in common makes no region. Ranges given the wrong way
round (high before low) are turned round.

The root key of a region is the zone's override if it has one, else the
sample's. A sample marked as having no pitch (255, common for drums)
plays at its recorded pitch on key 60.

The sample offsets of a region (fine and coarse, the coarse ones in units
of 32768 points) are applied to its sample's start, end, and loop. A loop
point a little outside what is played is pulled in to it.

Units are left as the format defines them (timecents, centibels, cents,
tenths of a percent). `TimecentsToSeconds`, `CentibelsToGain`, and
`CentsToRatio` convert the first three, and
[examples/RenderNote.cpp](../examples/RenderNote.cpp) shows them in use.

Presets are sorted by bank, then program. Where two presets claim the same
bank and program, the first one in the file is kept.

## Modulators

A modulator routes a controller to a generator: the note's velocity to the
attenuation, the mod wheel to the vibrato depth, the pitch wheel to the
pitch. A synthesizer moves the generator's value by the modulator's amount
times its source times its amount source, each mapped as the source's
type, direction, and polarity say (chapter 9.5 of the specification).
`Modulator` keeps the values as the file stores them, and
`havSF2::DecodeSource` takes a source apart.

Every region carries its resolved list in `Region::modulators`, built the
way chapter 9.5.1 of the specification lays down:

1. The ten **default modulators** (`havSF2::DefaultModulators`, section
   8.4). They belong to the instrument level.
2. The instrument's global zone: a modulator **identical** to one in the
   list replaces it, any other is added
3. The instrument zone: the same, over that
4. The preset's modulators: its zone's, which replace identical ones of its
   global zone. Each one identical to a modulator in the list **adds its
   amount** to it, and any other is added to the list.

Two modulators are identical when they have the same source, destination,
amount source, and transform. Only the amount may differ. (The
specification's sections on the modulator tables, 7.4 and 7.8, leave the
transform out of that. Its chapter on the controller model, 9.5.1,
includes it, and so does havSF2.) Of two identical modulators in one zone,
the later counts.

The list holds the defaults first, in the specification's order, then what
each level adds, in the order of the file. A synthesizer that wants to
leave out a default, such as velocity to filter cutoff, which several
players do not apply, finds it with `Modulator::IsIdenticalTo`.

Two entries of the specification's table of defaults are misprinted, and
havSF2 follows the text instead of the table: controller 7 is source
`0x0587` (printed `0x0582`, which would be controller 2), and controller
10 goes to the pan (printed as initial attenuation). The pitch wheel
modulator goes to "initial pitch", which the specification names but gives
no generator number. havSF2 calls it `havSF2::InitialPitch` and gives it
59, a number the format leaves unused.

**Left out**, with a line in `notes`:

- A modulator whose source, amount source, destination, or transform is not
  one the format defines: a general controller it does not list, a MIDI
  controller it calls illegal or reserves (0, 6, 32 to 63, 98 to 101, 120
  to 127), a source type beyond switch, a transform other than linear or
  absolute value, or a destination that is no generator, or one that only
  says what a zone is (its key and velocity ranges, its instrument or
  sample)
- A linked modulator: one whose destination is another modulator, or whose
  source or amount source is a link. Chains of modulators, which the
  specification describes in section 9.5.4, are not supported.
- The modulators of a zone whose indices into the modulator table run
  backwards or past its end. The zone itself still plays.

## 24-bit samples

A 24-bit file keeps the upper 16 bits of every point in the usual `smpl`
chunk and the low 8 bits in an `sm24` chunk. havSF2 reads them into
`SoundFont::lowBytes`, one per point of `data`, so that the 24-bit point
`i` is `data[i] * 256 + lowBytes[i]` (the low byte counts as unsigned).
As the specification asks (section 6.2), the chunk is ignored, with a line
in `notes`, if the file says it is older than version 2.04, if there is no
16-bit data, or if its size is not one byte per point, padded to an even
number. `lowBytes` is then empty, and the 16-bit data stands alone, as it
does in every 16-bit file.

## Checking the file

A SoundFont comes from somewhere on the internet and can be hundreds of
megabytes, so the loader trusts nothing in it.

**Refused**, with a message that names the file:

- It is not a SoundFont: no RIFF header, or another kind of RIFF file
- It is not version 2: the compressed SF3 format, for one
- It is incomplete: shorter than its own header says. The message
  suggests an interrupted download.
- A block claims more bytes than its parent has
- One of the nine tables is missing, is not a whole number of records, or
  lacks its end marker
- An index points backwards or past the end of the table it refers to
- It has no presets, or no preset has anything to play

**Left out**, with a line in `notes`, while the rest of the file loads:

- A zone whose sample does not exist, is in a sound card's ROM instead of
  in the file, or lies outside the sample data after its offsets
- A zone that names an instrument that does not exist
- A loop whose points leave no loop inside what is played. The sample is
  played without it.
- A second preset on a bank and program that an earlier one has

Every length is checked against the size of the file before anything is
allocated for it, and the sample data is read in blocks of a megabyte
straight into its final place. The tests cut a file short at every
possible length and damage each of its bytes in turn. The loader must
refuse the file or read what is left, never crash. `Load` turns running
out of memory into an error, while `Read` lets `std::bad_alloc` through.

## What the loader does not read

| Part of the format                           |                                                                                                                 |
| -------------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| Linked modulators                            | Left out, with a line in `notes` (see [Modulators](#modulators))                                                |
| Samples in ROM                               | Their zones are left out: the data is not in the file                                                           |
| Stereo links between samples                 | Not resolved. A stereo instrument is two zones, one panned left and one right, and both become regions.         |
| Preset library, genre, and morphology fields | Reserved by the format and unused in practice                                                                   |
| Text in other character sets                 | The format defines none. Anything that is not printable ASCII is shown as `?`, and trailing spaces are removed. |
