#pragma once
/*
 * BlueBridgeAGDI <-> simulator session (stage 7-2B.2 sections 41-47).
 *
 * Owns the one IpcClient of the DLL and the official debug-session lifecycle:
 *
 *   AG_INITFEATURES   -> Connect()   : pipe name -> connect -> AutoStart ->
 *                                      HELLO -> GET_CAPABILITIES -> validate
 *   AG_EXECITEM|RESET -> ResetHalt()
 *   AG_UNINIT         -> Disconnect(): client disconnect (reader joined), the
 *                                      simulator keeps running (LeaveSimulator
 *                                      Running=1) and the server applies its own
 *                                      teardown semantics (abort program, halt,
 *                                      clear breakpoints).
 *
 * Nothing here touches Keil memory, and no Keil callback is ever invoked from
 * the IPC reader thread.
 */

#include <atomic>
#include <string>

#include "IpcClient.h"
#include "IpcTypes.h"

namespace AgdiSession {

/* Full connect + handshake + capability validation. false = target unavailable
 * (the caller reports the official "cancel using this driver" to µVision). */
bool Connect();

/* Idempotent; safe to call from AG_UNINIT even when already lost. */
void Disconnect();

/* true while a handshaken session is alive AND the pipe is still up. */
bool Usable();

/* true once the transport is gone (simulator killed / pipe broken). */
bool Lost();

/* true when the CURRENT disconnect was requested by the driver itself
 * (AG_UNINIT / session teardown) instead of the target dying underneath us.
 * Used to distinguish "user stopped debugging" from "connection lost" -- only
 * the latter may terminate the µVision session (spec section 107). */
bool IntentionalDisconnect();

bbx::IpcClient &Client();
const bbx::TargetCaps &Caps();
uint32_t SessionId();
const std::wstring &PipeName();

/* convenience wrappers that keep the invalidation rules in one place */
uint32_t ResetHalt();
uint32_t Step();

}  // namespace AgdiSession