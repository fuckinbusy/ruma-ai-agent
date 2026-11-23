# Ruma AI Agent

An AI agent for Windows 11.

## Build

Requirements:

- Windows 11 x64
- Visual Studio 2026 with the "Desktop development with C++" workload (MSVC, CMake 4.0+, Ninja)
- PowerShell 7

Steps:

1. Fetch the pinned dependencies once. They are downloaded into `.deps/`:

   ```powershell
   pwsh -File scripts/bootstrap-deps.ps1
   ```

2. Build in a "Developer PowerShell for VS". The first configure builds the libraries from
   `vcpkg.json`, which takes a while:

   ```powershell
   cmake --preset release
   cmake --build --preset release
   ```

The program is `out/release/src/app/agent.exe`. The installer is
`out/release/src/setup/WinAiAgentSetup-0.0.1.exe`.
