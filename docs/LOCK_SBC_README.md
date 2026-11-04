# Jupy Lock SBC

## Introduction

This repository covers the different interconnected systems on a Raspberry Pi 5 (RPi5) that carry out local functions for the lock.  Available on request: [@linkedin:post](https://lnkd.in/p/e4GnH59w)

## Folder overview

The top-level folders in this workspace are grouped below.
See each folder's README (when present) for more details and usage instructions.

### Tracked (repository) folders

- **audio/**: Audio assets and examples.
- **backend/**: REST API and stream management server. See [backend/README.md](backend/README.md).
- **bin/**: Utility scripts and compiled binaries (startup, capture, helpers).
- **esp32/**: ESP32 firmware and front-end code.
- **face_recog/**: Face recognition scripts, models, and wrappers.
- **node-red/**: Node-RED flows and service configuration.
- **signalling_server/**: WebRTC signaling server and related tooling.
- **webrtc_video/**: WebRTC camera implementations and play-from-disk examples. See [webrtc_video/README.md](webrtc_video/README.md).
- **website/**: Frontend web application and build configuration. See [website/README.md](website/README.md).

---

### Untracked Folders :

Folders listed below match entries in `.gitignore` and contains runtime, generated, or dataset files that are not committed.

- **config/**: Commissioning and runtime configuration files.
- **dataset/**: Training and dataset artifacts (facedb, ygg_face2 samples).
- **facedb/**: Local face database and identity directories.
- **intruder/**: Unknown face captures and intrusion-related snippets.
- **logs/**: CSV and runtime logs.
- **tmp/**: Temporary runtime files (pids, upload staging).

---

Maintainer: repository owner ([@Github/Leospars](https://github.com/Leospars))
