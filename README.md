# RAINBOHz
## Project Summary
Cloud-scale audio synthesis and audio processing.

This is a research project that aims to combine additive synthesis with cloud scalability.

## Programming Langauge
C++23 using gcc.
Coding conventions according the C++ Core Guidelines.

## Target Platforms
This application is intended as a server-side application running on UNIX-like operating systems.

Currently the application has only been compiled, run and tested on macOS / Apple Silicon. This is part of the early proof-of-concept phase. It will not demonstrate horizontal scalability.

The goal is to target cloud platforms. This is likely to target AWS Graviton, it may (also) target NVIDIA CUDA for some processing. Other platforms are also under consideration. This will demonstrate horizontal scalability, and will also leverage SIMD and similar to optimize vertical scalability.

## Project Status
This project is currently in an early proof-of-concept stage. The repository will generally build correctly but it's likely not to do anything very useful yet. This situation is expected to improve...


