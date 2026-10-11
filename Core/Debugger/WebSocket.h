// Copyright (c) 2017- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <string>

namespace http {
class ServerRequest;
}

void HandleDebuggerRequest(const http::ServerRequest &request);
// Note: blocks.
void StopAllDebuggers();

struct BreakpointHit;

#ifdef __LIBRETRO__

// The WebSocket debugger isn't built for libretro (no HTTP server there - see WebServer.h), so
// these calls from Core.cpp/Debugger/Breakpoints.cpp resolve to harmless no-ops instead.
inline void WebSocketDebuggerTick() {}
inline bool WebSocketDebuggerHasClients() { return false; }
inline void WebSocketNotifyBreakpointHit(const BreakpointHit &hit) {}
inline void WebSocketNotifyEvent(const char *category, const std::string &json) {}
inline void WebSocketFlushEvents(int timeoutMs) {}

#else

// Notices emulator state changes (cpu.stepping, game.start, ...) and pushes the resulting events to
// connected debuggers, so their own threads never have to read that state themselves. CPU thread
// only; cheap, and safe to call with no debugger connected (it still has to track transitions).
void WebSocketDebuggerTick();

// Whether it's worth building a BreakpointHit at all. False whenever no debugger is connected,
// which is the overwhelmingly common case and the one that must stay free - a log-only breakpoint
// in a hot loop runs this per hit.
bool WebSocketDebuggerHasClients();

// Reports a breakpoint hit to every connected debugger as "cpu.breakpoint.hit", whether or not it
// stopped the CPU. That's what makes log-only breakpoints usable for automation: previously their
// only trace was a log line. CPU thread only.
void WebSocketNotifyBreakpointHit(const BreakpointHit &hit);

// Hands an already formatted event to every connected debugger, under a broadcast category a client
// can turn off with broadcast.config.set ("savedata", "exit", ...; the category must be one seeded in
// HandleDebuggerRequest). Any thread. Check WebSocketDebuggerHasClients() first to skip formatting.
void WebSocketNotifyEvent(const char *category, const std::string &json);

// Waits up to timeoutMs for every connection to have sent what was queued for it, so an event
// pushed just before shutdown (the exit event) gets out. Returns at once with nobody connected.
void WebSocketFlushEvents(int timeoutMs);

#endif
