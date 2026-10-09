# Official Keil AGDI SDK — local only, never committed

The AGDI driver ABI must **not** be guessed: `BlueBridgeAGDI.dll` is written
against the official Keil header `AGDI.H` and the official sample target driver
project (`SampTargN`) that Keil ships for AppNote 173.

Those files are copyrighted by Keil / Arm. They are **not** redistributed in this
repository. `vendor/agdi_sdk/` is git-ignored; the build script only *references*
the locally extracted copy (`build_agdi.ps1 -AgdiSdkDir <dir>`).

## Where to get it (verified working, 2026-09-26)

| Artifact | URL | Size | Notes |
|---|---|---|---|
| AGDI sample package (header + template) | `https://www.keil.com/download/files/apntex_173.zip` | 155 993 B | served by keil.com today; contains `SampTargN\AGDI.H` etc. |
| AppNote 173 documentation (CHM) | `https://www.keil.com/appnotes/files/apnt_173.zip` | 116 557 B | contains only `apnt_173.chm`; the doc text of AN173 |

Upstream docs entry point: <https://www.keil.com/products/uvision/db_trg_agdi.asp>
("AGDI Drivers" — "DLLs that are created using Microsoft Visual C++ and template
files provided by Keil … download Appnote 173 for ARM-based target systems").

The Keil **installation** on this machine ships only the compiled AGDI
implementations (`ARM\BIN\CMSIS_AGDI.dll`, `ARM\BIN\UL2CM3.dll`,
`ARM\Segger\JLTAgdi.dll`) — no `AGDI.H`, no template. That is why the package
above is used. Provenance and SHA-256 digests were verified locally during
development.

## Expected local layout

```
tools/keil_agdi/vendor/agdi_sdk/
    SampTargN/
        AGDI.H          <- official ABI header (the single source of truth)
        AGDI.CPP        <- official reference implementation (read-only for us)
        COMTYP.H        <- dbgblk / MAXTDRV / message codes
        SampTarg.cpp    <- EnumUvARM7 / DllUv3Cap lifecycle reference
        SampTarg.def    <- proves "no explicit EXPORTS" (=> cdecl dllexport names)
        ... (ReadMe.txt, Dokutxt.txt, GUI sources)
```

Extraction (PowerShell):

```powershell
Invoke-WebRequest https://www.keil.com/download/files/apntex_173.zip -OutFile $env:TEMP\apntex_173.zip
Expand-Archive $env:TEMP\apntex_173.zip -DestinationPath tools\keil_agdi\vendor\agdi_sdk -Force
```

If Keil later moves the URL, the Internet Archive copy of the *documentation*
(`web.archive.org/web/20060828214319/http://www.keil.com/appnotes/files/apnt_173.zip`)
is byte-identical to the current one; for the sample package contact Keil support
or use the appnote page above.