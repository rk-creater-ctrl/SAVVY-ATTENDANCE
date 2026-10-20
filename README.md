# C++ Attendance App

This is an attendance-only C++ migration path for the current Python project.

It is designed to:

- reuse `trainer.yml`
- reuse `labels.txt`
- reuse `attendance.db`
- start quickly
- avoid dataset scanning at startup
- avoid Python runtime overhead

## Current scope

This version focuses on the main deployment flow:

- load existing LBPH trainer
- load existing label mapping
- open camera
- recognize faces
- mark attendance in SQLite

It does not yet implement:

- student registration
- model training
- WhatsApp sending
- unknown-face storage
- full Tkinter-style admin UI

See [RUNTIME_FILES.md](./RUNTIME_FILES.md) for the deployment file list.

## Prerequisites

- Visual Studio 2022 with C++ desktop workload
- CMake 3.20+
- OpenCV C++ SDK with `opencv_world`/`OpenCVConfig.cmake`
- SQLite3 development package

If CMake cannot find OpenCV, pass `OpenCV_DIR` explicitly.

## Build

Example with CMake:

```powershell
cmake -S cpp_attendance -B cpp_attendance\build -DOpenCV_DIR="C:\path\to\opencv\build"
cmake --build cpp_attendance\build --config Release
```

Executable output:

```text
cpp_attendance\build\bin\Release\SavvyAttendanceCpp.exe
```

The executable should run from a folder that contains the runtime files.

## Controls

- `ESC` or `Q`: quit

## Notes

This app keeps the same recognition direction as the Python project by using:

- OpenCV Haar cascade for detection
- OpenCV LBPH for recognition
- the existing `trainer.yml`
- the existing `labels.txt`

That makes migration safer because you can keep using the current trained data while moving away from Python for production deployment.

## Recommended deployment flow

1. Train on the source machine.
2. Copy updated `trainer.yml`, `labels.txt`, and optional `trainer_meta.json`.
3. Deploy the C++ exe with `attendance.db` and the Haar cascade file.
4. Use the target system mainly for attendance, not training.
