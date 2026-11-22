# havSF2

Havoc's single-file SoundFont 2 (.sf2) loader for C++.

## Table of Contents

- [Features](#features)
- [Getting Started](#getting-started)
- [Usage](#usage)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [License](#license)

## Features

- Single-header, dependency-free (just include `havSF2.hpp`)
- Reads SoundFont 2 files from a path or any seekable stream
- Resolves the preset, instrument, and global zones into flat regions: for each preset, which sample plays for which keys and velocities, and the value of every generator
  - Instrument values override the instrument's global zone and the defaults, and preset values add to them, as the specification says
  - Key and velocity ranges of preset and instrument narrow each other
  - Sample offsets are applied, and loops are checked against what is played
  - Modulators are resolved the same way: the specification's default modulators, replaced by the instrument's and added to by the preset's
- Reads 24-bit samples (`sm24`) where the specification allows them
- Finds the regions to sound for a note: `FindRegions(bank, program, key, velocity)`
- Checks the whole file and trusts nothing in it: damaged, truncated, or foreign files are refused with a message fit to show a user, and every table index is checked before it is followed
- Lists what it left out and why in `notes` instead of failing a whole file over one bad zone
- Reads large sample data in blocks straight into its final place, and reports running out of memory instead of crashing
- Converts the specification's units: `TimecentsToSeconds`, `CentibelsToGain`, and `CentsToRatio`
- Comes with an example that renders a note into a WAV file to show how regions become sound
- Compile-time version constants (`VersionMajor`, `VersionMinor`, `VersionPatch`, and `VersionString`)

It is a loader, not a synthesizer: units stay as the specification defines them (timecents, centibels, cents), and turning regions into sound is up to you. The example shows where to start.

## Getting Started

This library requires C++23. It has been tested under Windows and Linux.

### Installation

Copy the header file into your project folder and include it:

```cpp
#include "havSF2.hpp"
```

Or add the folder to your CMake project and link the target:

```cmake
add_subdirectory(path/to/havSF2 ${CMAKE_BINARY_DIR}/havSF2)
target_link_libraries(your_target PRIVATE havSF2::havSF2)
```

The tests and the example build when havSF2 is the top-level project (`HAVSF2_BUILD_TESTS`, `HAVSF2_BUILD_EXAMPLES`). Presets are provided for Visual Studio 2022 (`msvc` with `msvc-debug` / `msvc-release`), MinGW (`mingw-debug` / `mingw-release`), and Linux (`linux-debug` / `linux-release`):

```sh
cmake --preset msvc
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

The `.vscode` folder has matching build, test, and debug configurations for VS Code with the CMake Tools and C/C++ extensions.

## Usage

```cpp
#include "havSF2.hpp"

#include <iostream>

int main()
{
  const auto font = havSF2::Load("GeneralUser-GS.sf2");

  if (!font)
  {
    std::cerr << font.error().message << '\n'; // "GeneralUser-GS.sf2: the file is incomplete: ..."
    return 1;
  }

  std::cout << font->info.name << ": " << font->presets.size() << " presets\n";

  for (const std::string &note : font->notes)
  {
    std::cout << "  " << note << '\n'; // what was left out, if anything
  }

  // Middle C on a piano (bank 0, program 0) at velocity 100
  for (const havSF2::Region *region : font->FindRegions(0, 0, 60, 100))
  {
    const havSF2::Sample &sample = font->samples[region->sample];

    // The points to play are font->data[region->start .. region->end),
    // looping over [region->loopStart, region->loopEnd) if those differ.
    // Pitch: the key against region->rootKey, plus tuning and correction.
    std::cout << sample.name << " at " << sample.sampleRate << " Hz, root key " << int(region->rootKey)
              << ", attack " << region->Value(havSF2::Generator::AttackVolEnv) << " timecents, "
              << region->modulators.size() << " modulators\n";
  }

  return 0;
}
```

Drum kits are in bank `havSF2::PercussionBank` (128). Programs are 0-based. `havSF2::Read(stream, name)` reads from a seekable stream opened in binary mode, and `name` stands in for the file name in messages.

### Example: from regions to sound

[examples/RenderNote.cpp](examples/RenderNote.cpp) plays one note into a WAV file:

```sh
RenderNote GeneralUser-GS.sf2 piano.wav            # Middle C on the piano
RenderNote GeneralUser-GS.sf2 snare.wav 128 0 38   # A snare from the drum kit
```

The arguments after the file names are bank, program, key, velocity, and how many seconds the note is held. For each region the note plays, the example works out:

- the pitch, from the key, the root key, the tuning, and the sample's correction (`CentsToRatio`)
- the loudness, from the initial attenuation and the modulators that depend on the note's key and velocity (`CentibelsToGain`)
- the pan, the loop mode, and the volume envelope (`TimecentsToSeconds`)

It then plays the sample points with linear interpolation, looping where the region says. It is an example, not a synthesizer: it leaves out the filter, the modulation envelope, the LFOs, modulators driven by MIDI controllers or the pitch wheel, and effects.

## Documentation

- [What the loader produces, how modulators are resolved, what it checks, and what it does not read](docs/overview.md)

## Contributing

Thank you for your interest! Suggestions for features and bug reports are always welcome via [issues](https://github.com/Havoc7891/havSF2/issues).

To maintain a consistent design and quality for this library, changes are implemented by the maintainer rather than via direct pull requests.

## License

Copyright © 2026 René Nicolaus

This library is released under the [MIT license](/LICENSE).
