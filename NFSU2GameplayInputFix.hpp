#pragma once

#ifdef GAME_UG2

#include <windows.h>
#include <XInput.h>
#include <cstdint>
#include <cstdio>

// Defined later in dllmain.cpp. The race update hook below guarantees that the
// same XtendedInput polling routine used by the front end also runs immediately
// before Underground 2 processes RealDriver gameplay input.
void __stdcall ReadControllerData();

namespace U2GameplayInputFix {

static_assert(sizeof(void*) == 4, "UG2 gameplay input fix requires x86");

constexpr std::uintptr_t kRealDriverUpdateAddr     = 0x0041DFC0;
constexpr std::uintptr_t kRealDriverUpdateCallsite = 0x0041E372;

typedef unsigned short(__thiscall* RealDriverUpdate_t)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
static RealDriverUpdate_t OriginalRealDriverUpdate = reinterpret_cast<RealDriverUpdate_t>(kRealDriverUpdateAddr);

static unsigned int g_frameCount = 0;

static void LogRawState() {
  if ((++g_frameCount % 120u) != 0u) return;

  XINPUT_STATE state{};
  DWORD result = XInputGetState(0, &state);

  FILE* f = std::fopen("NFSU_XtendedInput_Input_U2.log", "a");
  if (!f) return;

  std::fprintf(f,
      "U2InputFix: frame=%u xinput=%lu typeP1=%u deviceCount=%d buttons=0x%04X LT=%u RT=%u LX=%d LY=%d RX=%d RY=%d\n",
      g_frameCount,
      static_cast<unsigned long>(result),
      static_cast<unsigned int>(*reinterpret_cast<unsigned char*>(JOYSTICKTYPE_P1_ADDR)),
      *reinterpret_cast<int*>(DEVICE_COUNT_ADDR),
      static_cast<unsigned int>(state.Gamepad.wButtons),
      static_cast<unsigned int>(state.Gamepad.bLeftTrigger),
      static_cast<unsigned int>(state.Gamepad.bRightTrigger),
      static_cast<int>(state.Gamepad.sThumbLX),
      static_cast<int>(state.Gamepad.sThumbLY),
      static_cast<int>(state.Gamepad.sThumbRX),
      static_cast<int>(state.Gamepad.sThumbRY));
  std::fclose(f);
}

// The original call is __thiscall and consumes 3 dword stack arguments. A
// __fastcall wrapper receives ECX as the original this pointer, EDX as the
// unused fastcall slot, and consumes the same 3 stack arguments on return.
static unsigned short __fastcall RealDriverUpdateHook(
    void* self,
    void*,
    std::uint32_t arg1,
    std::uint32_t arg2,
    std::uint32_t arg3) {
  // This is the critical part: refresh XtendedInput's global XInput state before
  // RealDriver asks the scanner/event system for throttle/brake/steering/etc.
  ReadControllerData();
  LogRawState();

  return OriginalRealDriverUpdate(self, arg1, arg2, arg3);
}

static void Install() {
  std::remove("NFSU_XtendedInput_Input_U2.log");
  injector::MakeCALL(kRealDriverUpdateCallsite, RealDriverUpdateHook, true);
}

struct AutoInstall {
  AutoInstall() { Install(); }
};

static AutoInstall g_autoInstall;

}  // namespace U2GameplayInputFix

#endif  // GAME_UG2
