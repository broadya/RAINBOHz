---
status: accepted
date: 2026-10-03
---

# Play audio with miniaudio, on the default output device

## Context and Problem Statement

`playback-client` is the first component that plays sound. The author decided
(2026-10-03) that its first version should play through the operating
system's default output device. Device selection, multi-channel output and
the more advanced audio concepts are later work. The client must build
wherever the rest of the C++ does: macOS on Apple silicon for development, and
Linux and Windows besides.

Which library should it play through?

## Decision Drivers

* **The dependency rules of AGENTS.md.** Built from pinned source via
  FetchContent; included as a SYSTEM header; never exposed in a public header
  of the component.
* **Cross-platform from one code path.** Core Audio, ALSA/PulseAudio and
  WASAPI behind one API, so the client is not three clients.
* **Small.** The need is one output stream of float samples, fed from a
  callback. Nothing about it needs a mixer, a plug-in host or a graph.
* **Real-time behaviour we control.** The callback must stay ours, so that
  what runs on the audio thread is exactly the lock-free pop the design
  requires (`playback-client/docs/design.md` §5).

## Considered Options

1. **miniaudio** (`mackron/miniaudio`): one C header, public domain or MIT-0,
   with backends for every target. It is by the author of dr_wav, which the
   project already uses (ADR 0009).
2. **PortAudio**: long-established and C, with a CMake build of its own, and
   on Linux it needs the ALSA development package to build.
3. **RtAudio**: C++, small, with a callback API, but it needs the platform's
   audio headers and libraries found at configure time, per backend.
4. **The platform APIs directly**: Core Audio's AudioQueue or AudioUnit on
   macOS, and something else on each other platform.
5. **JUCE**: the full framework, which the later DAW plug-in may well use.
   For a CLI it is a great deal of code and a licence to manage.

## Decision Outcome

**miniaudio**, pinned to a release tag (0.11.25), with its decoding, encoding,
generation, resource-manager, node-graph and engine parts compiled out, so
only the low-level device API is built. It is included by one translation
unit (`src/device_sink.cpp`), which carries the implementation.

It meets every driver at the lowest cost. It is a header with no build
system; it finds its backends at run time (it uses `dlopen` on Linux, so
building needs no audio development packages); and it hands us a plain data
callback whose content is entirely ours. Asking for a sample rate the device
does not run at is handled inside it by resampling, which the client logs.
Having the author of dr_wav behind both libraries means that one pinning
discipline and one style of API cover both, and miniaudio's own copy of
dr_wav is prefixed, so the two do not collide.

PortAudio and RtAudio would do the job, but each brings a build system or a
configure-time dependency on platform packages for what miniaudio does as a
header. The platform APIs are three implementations to keep in step. JUCE is
the right weight for the plug-in, if that is where it goes, and the wrong
weight here. Its choice for `playback-plugin` is left open.

### Consequences

* Good, because adding audio output cost one header and no system
  dependency, on every platform.
* Good, because the real-time callback is code in this repository, reviewed
  and documented, rather than a library's.
* Bad, because miniaudio is mostly one maintainer's work. It is
  public domain and a single file, so carrying a copy, or patching it, is
  easy if that ever matters.
* Neutral, because device choice, exclusive mode and multi-channel layouts
  are all within miniaudio when they are wanted, but none is used yet.

Impact on components:

* `playback-client` — plays through the default output device from
  `src/device_sink.cpp`, the only translation unit that includes miniaudio.
  Its callback does nothing but the lock-free pop, and a resampled device
  rate is logged.
* The playback library (not built yet) — miniaudio's device API is
  available to it on the same terms, but its choice of audio output is not
  made here.
* The DAW plugin (not built yet) — plays through its host, not a device, so
  this decision does not bind it. Whether it uses JUCE is left open.

## More Information

`playback-client/cmake/Dependencies.cmake` pins it, and
`playback-client/docs/design.md` §5 describes the sink built on it.
