Xtank Windows client (Phase 2 MVP 0.1.0)
=========================================

Requirements: a 64-bit Windows 11 machine.  No compiler, X server, Cygwin or
other POSIX runtime is required; SDL3.dll is bundled next to the executable.

Unpack this directory anywhere (paths with spaces are fine).  The archive is
self-contained.

  1. Runtime self-test (recommended first run).  In PowerShell:

         .\tools\windows-smoke.ps1 -Exe .\bin\xtank-client.exe `
                                   -Fixtures .\fixtures\protocol

     Expect "WINDOWS-SMOKE PASS".  This audits the PE imports, opens a window,
     exercises keyboard/mouse and DPI handling, decodes the wire fixtures and
     runs an ENet loopback handshake.

  2. Play against a Linux server (start it first; see the server package):

         .\bin\xtank-client.exe --play HOST:PORT --assets .\assets\xtank-assets.bin

     with HOST:PORT the address the server reports (e.g. 192.168.1.20:7777).
     Use --connect HOST:PORT to run only the handshake check.

The client owns presentation and controls only; the Linux server is the sole
authority for movement, collisions, damage, RNG, bots and scoring.  Gameplay
packets are the frozen protocol v1 (docs/PROTOCOL-v1.md).
