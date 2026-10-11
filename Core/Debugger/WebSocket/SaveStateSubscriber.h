// Copyright (c) 2026- PPSSPP Project.

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

#include "Core/Debugger/WebSocket/WebSocketUtils.h"

// savestate.* - save states over the debugger, to named in-memory slots or to files.
DebuggerSubscriber *WebSocketSaveStateInit(DebuggerEventHandlerMap &map);

void WebSocketSaveStateSave(DebuggerRequest &req);
void WebSocketSaveStateLoad(DebuggerRequest &req);
void WebSocketSaveStateList(DebuggerRequest &req);
void WebSocketSaveStateDelete(DebuggerRequest &req);

// config.* - the console's system settings (confirm button, language, nickname, clock format).
DebuggerSubscriber *WebSocketSystemConfigInit(DebuggerEventHandlerMap &map);

void WebSocketSystemConfigGet(DebuggerRequest &req);
void WebSocketSystemConfigSet(DebuggerRequest &req);
