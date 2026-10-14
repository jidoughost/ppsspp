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

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/WebSocket/SaveStateSubscriber.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HLE/sceImpose.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceUtility.h"
#include "Core/MIPS/MIPSDebugInterface.h"
#include "Core/SaveState.h"
#include "Core/System.h"

// In-memory slots, by name. A state is the whole machine (RAM, VRAM, kernel, GPU), tens of MB, so
// a client that makes many should drop the ones it's done with (savestate.delete).
static std::mutex g_slotLock;
static std::map<std::string, std::vector<u8>> g_slots;

DebuggerSubscriber *WebSocketSaveStateInit(DebuggerEventHandlerMap &map) {
	map["savestate.save"] = &WebSocketSaveStateSave;
	map["savestate.load"] = &WebSocketSaveStateLoad;
	map["savestate.list"] = &WebSocketSaveStateList;
	map["savestate.delete"] = &WebSocketSaveStateDelete;
	return nullptr;
}

// Runs func on the CPU thread at a point where a state can be taken: game booted, kernel up, and
// no display list in flight (a state can't be taken mid-list - SaveState::Process waits the same
// way). The CPU thread drains its queue at the top of every run-loop lap, which can fall between
// the CPU handing over to the GE and the GE finishing, so a busy lap is simply retried.
static bool RunWhenStateable(DebuggerRequest &req, const std::function<void()> &func) {
	for (int attempt = 0; attempt < 200; ++attempt) {
		bool booted = false, busy = false;
		Core_RunOnCPUThread([&] {
			booted = PSP_GetBootState() == BootState::Complete && __KernelIsRunning();
			busy = coreState == CORE_RUNNING_GE || coreState == CORE_STEPPING_GE;
			if (booted && !busy)
				func();
		});
		if (!booted) {
			req.Fail("Game not running");
			return false;
		}
		if (!busy)
			return true;
		sleep_ms(1, "savestate-retry");
	}
	req.Fail("GPU stayed busy (stepping the GE?), state not taken");
	return false;
}

// Blocks until a queued SaveState::Save/Load reports back, which happens on the CPU thread the next
// time it processes save states (every run-loop lap while stepping, about every frame while not).
// The callback owns the shared state, so a report that arrives after Wait() gave up (and this waiter
// left the stack) still writes into live memory.
struct StateWaiter {
	struct Shared {
		std::mutex lock;
		std::condition_variable cond;
		bool done = false;
		SaveState::Status status = SaveState::Status::FAILURE;
		std::string message;
	};
	std::shared_ptr<Shared> shared = std::make_shared<Shared>();
	SaveState::Status status = SaveState::Status::FAILURE;
	std::string message;

	SaveState::Callback Callback() {
		std::shared_ptr<Shared> state = shared;
		return [state](SaveState::Status s, std::string_view msg, std::string_view) {
			std::lock_guard<std::mutex> guard(state->lock);
			state->status = s;
			state->message = msg;
			state->done = true;
			state->cond.notify_all();
		};
	}

	bool Wait(int seconds) {
		std::unique_lock<std::mutex> guard(shared->lock);
		Core_WakeIdleCPUThread();
		if (!shared->cond.wait_for(guard, std::chrono::seconds(seconds), [this] { return shared->done; }))
			return false;
		status = shared->status;
		message = shared->message;
		return true;
	}
};

// Save a state (savestate.save)
//
// Works paused or running; a paused CPU stays paused. Answers once the state is actually taken.
//
// Parameters (exactly one of):
//  - slot: string, any name - an in-memory slot in this emulator process, replaced if it exists.
//  - path: string, a file to write (the same format the app's save states use).
//
// Response (same event name):
//  - slot or path: as given.
//  - size: bytes (in-memory slots only, uncompressed).
//  - ticks: CPU cycles into emulation at the moment saved.
//  - ms: how long it took.
void WebSocketSaveStateSave(DebuggerRequest &req) {
	const bool toFile = req.HasParam("path");
	if (toFile == req.HasParam("slot"))
		return req.Fail("Pass exactly one of 'slot' or 'path'");
	std::string name;
	if (!req.ParamString(toFile ? "path" : "slot", &name))
		return;

	const double start = time_now_d();
	double ticks = 0.0;
	if (toFile) {
		StateWaiter waiter;
		bool ok = RunWhenStateable(req, [&] {
			ticks = (double)CoreTiming::GetTicks(currentMIPS);
			SaveState::Save(Path(name), -1, waiter.Callback());
		});
		if (!ok)
			return;
		if (!waiter.Wait(60))
			return req.Fail("Timed out waiting for the save to be processed");
		if (waiter.status == SaveState::Status::FAILURE)
			return req.Fail("Save failed: " + waiter.message);
		JsonWriter &json = req.Respond();
		json.writeString("path", name);
		json.writeFloat("ticks", ticks);
		json.writeFloat("ms", (time_now_d() - start) * 1000.0);
		return;
	}

	std::vector<u8> data;
	CChunkFileReader::Error err = CChunkFileReader::ERROR_NONE;
	if (!RunWhenStateable(req, [&] {
		ticks = (double)CoreTiming::GetTicks(currentMIPS);
		err = SaveState::SaveToRam(data);
	}))
		return;
	if (err != CChunkFileReader::ERROR_NONE)
		return req.Fail("Save failed");
	const size_t size = data.size();
	{
		std::lock_guard<std::mutex> guard(g_slotLock);
		g_slots[name] = std::move(data);
	}
	JsonWriter &json = req.Respond();
	json.writeString("slot", name);
	json.writeFloat("size", (double)size);
	json.writeFloat("ticks", ticks);
	json.writeFloat("ms", (time_now_d() - start) * 1000.0);
}

// Load a state (savestate.load)
//
// Works paused or running; a paused CPU stays paused (at the loaded state's PC), so a screenshot
// straight after shows the loaded picture. Answers once the state is actually in.
//
// Parameters (exactly one of):
//  - slot: string, an in-memory slot made by savestate.save.
//  - path: string, a save state file (from savestate.save or the app).
//
// Response (same event name):
//  - slot or path: as given.
//  - ticks: CPU cycles into emulation after the load (the loaded state's time).
//  - ms: how long it took.
void WebSocketSaveStateLoad(DebuggerRequest &req) {
	const bool fromFile = req.HasParam("path");
	if (fromFile == req.HasParam("slot"))
		return req.Fail("Pass exactly one of 'slot' or 'path'");
	std::string name;
	if (!req.ParamString(fromFile ? "path" : "slot", &name))
		return;

	const double start = time_now_d();
	double ticks = 0.0;
	if (fromFile) {
		StateWaiter waiter;
		if (!RunWhenStateable(req, [&] { SaveState::Load(Path(name), -1, waiter.Callback()); }))
			return;
		if (!waiter.Wait(60))
			return req.Fail("Timed out waiting for the load to be processed");
		if (waiter.status == SaveState::Status::FAILURE)
			return req.Fail("Load failed: " + waiter.message);
		Core_RunOnCPUThread([&] { ticks = (double)CoreTiming::GetTicks(currentMIPS); });
		JsonWriter &json = req.Respond();
		json.writeString("path", name);
		json.writeFloat("ticks", ticks);
		json.writeFloat("ms", (time_now_d() - start) * 1000.0);
		return;
	}

	std::vector<u8> data;
	{
		std::lock_guard<std::mutex> guard(g_slotLock);
		auto it = g_slots.find(name);
		if (it == g_slots.end())
			return req.Fail("No such slot: " + name);
		// A copy: loading reads from it, and the slot stays for the next load.
		data = it->second;
	}
	CChunkFileReader::Error err = CChunkFileReader::ERROR_NONE;
	std::string error;
	if (!RunWhenStateable(req, [&] {
		err = SaveState::LoadFromRam(data, &error);
		if (err == CChunkFileReader::ERROR_NONE) {
			// As SaveState::Process does after a load: a pending exception belonged to the old state,
			// and the time spent loading isn't the game running slow.
			Core_ResetException();
			__DisplaySetWasPaused();
		}
		ticks = (double)CoreTiming::GetTicks(currentMIPS);
	}))
		return;
	if (err != CChunkFileReader::ERROR_NONE)
		return req.Fail("Load failed: " + error);
	JsonWriter &json = req.Respond();
	json.writeString("slot", name);
	json.writeFloat("ticks", ticks);
	json.writeFloat("ms", (time_now_d() - start) * 1000.0);
}

// List in-memory slots (savestate.list)
//
// No parameters.
//
// Response (same event name):
//  - slots: array of objects with name (string) and size (bytes).
void WebSocketSaveStateList(DebuggerRequest &req) {
	JsonWriter &json = req.Respond();
	json.pushArray("slots");
	std::lock_guard<std::mutex> guard(g_slotLock);
	for (const auto &slot : g_slots) {
		json.pushDict();
		json.writeString("name", slot.first);
		json.writeFloat("size", (double)slot.second.size());
		json.pop();
	}
	json.pop();
}

// Drop an in-memory slot (savestate.delete)
//
// Parameters:
//  - slot: string.
//
// Response (same event name) with no extra data. Fails if there's no such slot.
void WebSocketSaveStateDelete(DebuggerRequest &req) {
	std::string name;
	if (!req.ParamString("slot", &name))
		return;
	std::lock_guard<std::mutex> guard(g_slotLock);
	if (g_slots.erase(name) == 0)
		return req.Fail("No such slot: " + name);
	req.Respond();
}

DebuggerSubscriber *WebSocketSystemConfigInit(DebuggerEventHandlerMap &map) {
	map["config.get"] = &WebSocketSystemConfigGet;
	map["config.set"] = &WebSocketSystemConfigSet;
	return nullptr;
}

static const char *const g_languageNames[] = {
	"japanese", "english", "french", "spanish", "german", "italian", "dutch", "portuguese",
	"russian", "korean", "chineseTraditional", "chineseSimplified",
};

static void WriteSystemConfig(JsonWriter &json) {
	const int lang = GetPSPLanguage();
	json.writeString("buttonPreference", g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS ? "cross" : "circle");
	json.writeInt("language", lang);
	json.writeString("languageName", lang >= 0 && lang < (int)ARRAY_SIZE(g_languageNames) ? g_languageNames[lang] : "unknown");
	json.writeString("nickname", g_Config.sNickName);
	json.writeInt("timeZone", g_Config.iTimeZone);
	json.writeInt("dateFormat", g_Config.iDateFormat);
	json.writeInt("timeFormat", g_Config.iTimeFormat);
	json.writeBool("daylightSavings", g_Config.bDayLightSavings);
	// read only: the JIT compiles the check in, so it is set at launch (--ignore-bad-mem)
	json.writeBool("ignoreBadMemAccess", g_Config.bIgnoreBadMemAccess);
}

// Read the system settings (config.get)
//
// No parameters.
//
// Response (same event name):
//  - buttonPreference: "cross" or "circle", the console's confirm button (system dialogs use it).
//  - language: PSP system language id (0 japanese, 1 english, ... 11 chineseSimplified).
//  - languageName: the same as a name.
//  - nickname: string.
//  - timeZone: minutes from UTC.
//  - dateFormat: 0 YYYYMMDD, 1 MMDDYYYY, 2 DDMMYYYY.
//  - timeFormat: 0 24-hour, 1 12-hour.
//  - daylightSavings: boolean.
//  - ignoreBadMemAccess: boolean, read only (set at launch with --ignore-bad-mem).
// For the emulated date and time itself, see replay.time.get/set.
void WebSocketSystemConfigGet(DebuggerRequest &req) {
	JsonWriter &json = req.Respond();
	Core_RunOnCPUThread([&] { WriteSystemConfig(json); });
}

// Change system settings (config.set)
//
// For this run only (headless never saves a config). Games read most of these when they ask the
// system (sceUtilityGetSystemParamInt, the save/message dialogs), so set them before the game does -
// e.g. while stopped at the entry point - for a change to show; sceImpose's copies are refreshed too.
//
// Parameters (all optional, any combination):
//  - buttonPreference: "cross" or "circle".
//  - language: id (number) or name (as in config.get's languageName).
//  - nickname: string, at most 127 bytes.
//  - timeZone: minutes from UTC.
//  - dateFormat: 0-2. timeFormat: 0-1. daylightSavings: boolean.
//
// Response (same event name): the settings after the change, as config.get.
void WebSocketSystemConfigSet(DebuggerRequest &req) {
	std::string button;
	int lang = -2;
	if (req.HasParam("buttonPreference")) {
		if (!req.ParamString("buttonPreference", &button))
			return;
		if (button != "cross" && button != "circle")
			return req.Fail("buttonPreference must be 'cross' or 'circle'");
	}
	if (req.HasParam("language")) {
		uint32_t id = 0;
		std::string name;
		if (req.ParamString("language", &name, DebuggerParamType::OPTIONAL) && !name.empty()) {
			for (int i = 0; i < (int)ARRAY_SIZE(g_languageNames); ++i) {
				if (name == g_languageNames[i])
					lang = i;
			}
			if (lang < 0)
				return req.Fail("Unknown language name: " + name);
		} else if (req.ParamU32("language", &id)) {
			if (id >= ARRAY_SIZE(g_languageNames))
				return req.Fail("language id out of range (0-11)");
			lang = (int)id;
		} else {
			return;
		}
	}
	std::string nickname;
	const bool setNick = req.HasParam("nickname");
	if (setNick) {
		if (!req.ParamString("nickname", &nickname))
			return;
		if (nickname.size() > 127)
			return req.Fail("nickname too long (127 bytes max)");
	}
	uint32_t timeZone = 0, dateFormat = 0, timeFormat = 0;
	bool dst = false;
	const bool setTz = req.HasParam("timeZone"), setDf = req.HasParam("dateFormat"), setTf = req.HasParam("timeFormat");
	const bool setDst = req.HasParam("daylightSavings");
	if (setTz) {
		double tz = 0;
		if (!req.ParamF64("timeZone", &tz))
			return;
		if (tz < -720 || tz > 840)
			return req.Fail("timeZone out of range (-720..840 minutes)");
		timeZone = (uint32_t)(int)tz;
	}
	if (setDf && (!req.ParamU32("dateFormat", &dateFormat) || dateFormat > 2))
		return req.Fail("dateFormat must be 0, 1 or 2");
	if (setTf && (!req.ParamU32("timeFormat", &timeFormat) || timeFormat > 1))
		return req.Fail("timeFormat must be 0 or 1");
	if (setDst && !req.ParamBool("daylightSavings", &dst))
		return;

	JsonWriter &json = req.Respond();
	Core_RunOnCPUThread([&] {
		if (!button.empty())
			g_Config.iButtonPreference = button == "cross" ? PSP_SYSTEMPARAM_BUTTON_CROSS : PSP_SYSTEMPARAM_BUTTON_CIRCLE;
		if (lang >= 0)
			g_Config.iLanguage = lang;
		if (setNick)
			g_Config.sNickName = nickname;
		if (setTz)
			g_Config.iTimeZone = (int)timeZone;
		if (setDf)
			g_Config.iDateFormat = (int)dateFormat;
		if (setTf)
			g_Config.iTimeFormat = (int)timeFormat;
		if (setDst)
			g_Config.bDayLightSavings = dst;
		if (PSP_GetBootState() == BootState::Complete)
			__ImposeRefreshSystemParams();
		WriteSystemConfig(json);
	});
}
