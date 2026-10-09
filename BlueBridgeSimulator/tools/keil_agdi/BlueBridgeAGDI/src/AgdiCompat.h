#pragma once
/*
 * BlueBridgeAGDI - compatibility wrapper around the official Keil AGDI header.
 *
 * The ABI is NOT re-declared here: the official `AGDI.H` (Keil AGDI SDK,
 * apntex_173.zip) is the single source of truth.  This wrapper only supplies
 * what AGDI.H expects from its usual companion header (COMTYP.H) and flips the
 * official `_IN_TARG_` switch so that AGDI.H declares the entry points as
 * `_EXPO_` (= __declspec(dllexport)) __cdecl functions.
 *
 * Expected include path: build_agdi.ps1 passes -AgdiSdkDir /I to the compiler.
 */

#include <Windows.h>

/* Type pre-declarations AGDI.H relies on (see official COMTYP.H). */
typedef signed __int64     INT64;
typedef unsigned __int64   UINT64;
typedef signed short int   INT16;
typedef unsigned short int WORD16;

/* Official switch, see AGDI.H:
 *   "define if used in Mon166,Emu..."
 * It turns the AG_* / EnumUv* declarations into real dllexport prototypes. */
#define _IN_TARG_
#include "AGDI.H"

/* AGDI.H keeps a global `supp` in the official sample; we keep our own state
 * private in AgdiDriver.cpp instead (thin-DLL rule). */