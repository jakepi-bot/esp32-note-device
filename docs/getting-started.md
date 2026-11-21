# Getting started

The device this repository ships is a **notes-only** recorder: it records to a
microSD card, uploads over Wi-Fi, and can hand a note to a phone over Bluetooth.
It does not talk to Hermes, run turns or play replies; the host runs Hermes only
to transcribe. The rest of this repository is still the Hermes Gadget SDK, and its
core still builds, but it is no longer what the board runs.

| Your starting point | Guide | What you need |
|---|---|---|
| I have the e-paper notes device | [The README](../README.md) | PlatformIO, a USB data cable, 2.4 GHz Wi-Fi, and a host for the note endpoint |
| I want the controls and screens | [Using the notes device](notes-device.md) | nothing else |
| I want to sync from a phone | [iPhone](ios-bluetooth-sync.md) or [Android](android-bluetooth-sync.md) | the note server, and Chrome or Bluefy |
| I want to try the SDK on my computer | [Try the simulator](desktop.md) | Python 3.10+, CMake 3.16+, and a C++17 compiler |
| I have a Raspberry Pi | [Run a Linux gadget](linux.md) | Pi 4 or 5, 64-bit Raspberry Pi OS Lite Trixie, and Hermes; optional USB audio |
| I want to build with the SDK | [Development](development.md) | A repository checkout and the tools for the part you want to change |

[Features](features.md) lists what the gadget does and which boards support each part.

Board setup uses prebuilt firmware. You do not need Python, CMake, or a compiler on the computer that flashes it.

## The host

<a id="3-connect-to-hermes"></a>

The notes device needs one thing from a Hermes host: the note endpoint the plugin
serves on port 8766, which verifies the upload and transcribes it into the vault.
[Connect Hermes](connect-hermes.md) covers that install, and then the pairing and
speech steps that belong to the SDK's other boards and the simulator.

The scripted demo echoes messages. It does not run an agent or understand speech.

## Use your gadget

[Talk, type, and interrupt](using-gadget.md) covers everyday controls. For an existing device, see [change Wi-Fi or update firmware](setup-board.md#manage-an-existing-gadget).

## Find help

[Troubleshooting](troubleshooting.md) covers build failures, connection problems, pairing, and missing audio. The browser installer can save a diagnostics report without installing Python.

## Build with the SDK

- [Customize the face](faces.md).
- [Add a board, actions, or sensors](porting.md).
- [Run the tests](development.md#test-suites).
- Read the [architecture](architecture.md), [protocol](protocol.md), and [Hermes integration reference](hermes-integration.md).
