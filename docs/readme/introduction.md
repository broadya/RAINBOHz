# RAINBOHz

<img alt="RAINBOHz" src="docs/readme/rainbohz-logo.png">

## Project Summary
RAINBOHz is a research project exploring additive synthesis at large scale, referred to as __macro-scale additive synthesis__. It exists at the "macro-scale" from two perspectives. It allows large numbers of additive partials to be used, and it allows control over the macro timescale i.e. over a complete musical work (or a significant part thereof) defined by a score.

Scores are written in a domain-specific, declarative language called __HzL__. This language and its compiler are part of this project. It is expected that practitioners will use a combination of
hand-coding, assembly from pre-built component libraries, and LLMs to generate HzL scores.

Some key concepts and goals of the project are listed below. Only a limited number of
these goals have been implemented to date.

From an audio / music perspective:

- Provide an interactive environment for sound design, composition and live coding.
- Support authoring based on a language for describing scores.
- Scale to large numbers of additive partials.
- Support polyrhythms / polytempo.
- Support spatial audio.
- Allow manipulation of scores at different levels of abstraction.
- Integrate with DAWs for workflow.
- Do not provide a playable "instrument".

From a computer science perspective:

- Provide a domain-specific, declarative language describing a score.
- Allow composition with reusable components and version control through git.
- Support deployment on single-user workstations, LANs, and public cloud platforms.
- Render large numbers of partials with acceptable latency making use of vertical and horizontal scalability.
- Provide rapid rendering and random-access playback based on mathematical integration of state.

The project is currently in an early stage, moving from a proof-of-concept to the target architecture. The current version should be considered as an emerging research project. Much testing, validation and additional features still remain to be added. Here be dragons. It's not only that "your mileage may vary", it's that the wheels might fall off, or pressing the brake pedal might make the doors open. You have been warned!

In particular the following are large gaps in the current implementation.

- Anti-aliasing
- API / network approach (currently some parts are implemented)
- Horizontal scalability
- Build using GitHub actions

The architecture takes all of these issues into account, but they are simply not yet implemented.

## Audio Warning
Turn down your headphone / speaker level before trying experiments with RAINBOHz. This software can generate unusual audio signals that could damage your speakers or headphones, and more importantly could damage your hearing. Keep those levels down whilst experimenting!

## AI Disclosure

This project was initiated prior to the widespread use of LLMs for code generation. The first proof-of-concept was written following a largely conventional approach.

The current source code and most of the documentation is primarily AI-generated. The use of LLMs has accelerated development of RAINBOHz, but in its current form not all errors have been found, algorithms may be suboptimal, and code style may be inappropriate. As the project matures, code reviews and refactoring will gradually improve this situation.

Some key artefacts such as the graph database schema, and most of the core HzL language have been coded by hand.

## Architecture

RAINBOHz is designed so that it can be deployed and scaled using a distributed architecture.
This has not yet been demonstrated. It is a design concept - albeit
one that has been the subject of considerable effort, supported by performance and capacity calculations. The current codebase includes some of the initial components that will make this possible, such as an early version of one of the REST APIs.

RAINBOHz can be deployed on a single computing node, which may be sufficient depending upon the required throughput and the acceptable use of local resources. In its current state, this is the only possible deployment model.

Horizontal scalability is based on sharding of partials over multiple nodes. The sharding
is in turn based on dividing the domain-specific language source over the nodes. The exact
mechanism for this is not yet designed, but there are some clear approaches such as division
by sound entity.

This can be performed by placing all HzL code in a git repository, then using partial clones to divide the model between the nodes. It is likely that the sharding approach will evolve from there.

Each processing node is an independent end-to-end synthesis node. Each node performs the full
end-to-end process of compiling HzL to the graph database model, performing the database
transformations, performing precomputation, and finally rendering frames of audio. When deployed on a distributed system, this audio may then be written to object storage. A more performant option may be to stream the audio to the next stage in the pipeline.

The next stage makes audio available to clients. It performs summing and anti-aliasing, then makes audio available to clients via a REST API.

The pipeline can be viewed as a chain of components as follows:

**Compilation** — essentially a software build system.

- The Hertz Language, HzL
- Hertz Language Compiler
- Hertz Language Build System
- `canonical-form` — the compiled, loadable score

**Rendering** — transformation of the score specification to audio

- `rendering-engine` — loads, transforms, precomputes and renders
- `engine-db` — the one graph database, hosted in process, sharded by partial set
- Object storage — WAV objects addressed by URL

**Playback** — merging the shards on demand.

- `playback-server` — sums per-shard objects and serves audio
- Playback Component
- Playback Plug-in

**Orchestration** — a message bus connecting the components. Kafka is the
current target. This exists currently only as a design idea.

See [`architecture/overview.md`](architecture/overview.md) for the full
description, and §11 there for why an earlier pipelined design was collapsed
into this one.

## Programming Languages
C++20 using clang / Coding conventions according the C++ Core Guidelines.
Python 3.12

## Target Platforms
This application is intended as a server-side application running on UNIX-like operating systems and/or cloud computing platforms.

The primary goal is to target cloud platforms. This is likely to target AWS Graviton. Other platforms are also under consideration. This will demonstrate horizontal scalability, and will also leverage SIMD and similar to optimize vertical scalability. There has been some consideration as to the use of GPUs, but the current viewpoint is that the ARM architecture will offer the best cost / performance tradeoff and is easier to work with.

## Project Status
This project is currently moving from its proof-of-concept phase towards a scalable architecture.

