# Signals Library & Test Suite

This repository implements a C++ event-driven framework featuring `GObject`, `GSignal`, `GThread`, `GTimer`, and `GEvent` abstractions with cross-thread signal/slot dispatching and object lifetime tracking.

## Prerequisites

- **Python 3** (required to run the `waf` build tool)
- **C++17 Compiler** (MSVC on Windows, GCC/Clang on Linux)
- **Boost** (included via submodules or external dependencies)

## Building the Tests

The project uses `waf` as its build system. To configure, build, and install the unit test binary, execute the following command:

### Windows (Command Prompt / MINGW Bash)

```cmd
waf configure
waf install --project=Tests
```

```MINGW bash
./waf configure
```

Optionally specify the build mode (default is `debug`):

```cmd
waf install --project=Tests --mode=release
```

### Linux

```bash
python3 waf install --project=Tests
```
```WSL
wsl python3 waf install --project=Tests
```

The compiled Google Test binary will be installed to:
- **Windows**: `install\Tests\debug\Windows\x64\usr\bin\Tests.exe`
- **Linux**: `install/Tests/debug/Linux/x64/usr/bin/Tests`

## Running the Tests

Execute the test binary directly from the command line:

### Run All Unit Tests

**Windows:**
```powershell
.\install\Tests\debug\Windows\x64\usr\bin\Tests.exe
```

**Linux:**
```bash
./install/Tests/debug/Linux/x64/usr/bin/Tests
```

### Run Specific Test Cases

Filter tests by name pattern using standard Google Test flags:

- **Run all cross-thread connection tests:**
  ```powershell
  .\install\Tests\debug\Windows\x64\usr\bin\Tests.exe --gtest_filter=GObjectTest.CrossThread*
  ```

- **Run object destruction safety tests:**
  ```powershell
  .\install\Tests\debug\Windows\x64\usr\bin\Tests.exe --gtest_filter=GObjectTest.*Destroyed*
  ```

- **Run unit tests repeatedly for stress testing:**
  ```powershell
  .\install\Tests\debug\Windows\x64\usr\bin\Tests.exe --gtest_repeat=10
  ```
