#pragma once

#ifdef GAME_UG2

#include <windows.h>
#include <XInput.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>

namespace U2RealRumble {

static_assert(sizeof(void*) == 4, "Underground 2 rumble backend requires a 32-bit build");

// SPEED2.EXE 1.2 native wheel FFB path, recovered from the supplied executable.
// The game calculates these effects in FUN_00406460, but XtendedInput disables
// DirectInput device enumeration. We keep the native calculations and replace
// only the effect output with XInput dual-motor rumble.
constexpr std::uintptr_t kHasFFBDeviceCall   = 0x00406497;
constexpr std::uintptr_t kSpringCall         = 0x00406503;
constexpr std::uintptr_t kConstantForceCall  = 0x004065DB;
constexpr std::uintptr_t kDamperCall         = 0x004065FD;
constexpr std::uintptr_t kCompositeStartCall = 0x00406631;
constexpr std::uintptr_t kCompositeStopCall  = 0x00406651;
constexpr std::uintptr_t kImpactCall1        = 0x00406775;
constexpr std::uintptr_t kImpactCall2        = 0x004067A6;
constexpr std::uintptr_t kSurfaceCall        = 0x004068C5;
constexpr std::uintptr_t kSpecialDamperCall  = 0x004068F3;
constexpr std::uintptr_t kCleanupCall        = 0x00406931;

static inline float Saturate(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static inline float AbsF(float v) {
  return v < 0.0f ? -v : v;
}

static inline float Normalize10000(int value) {
  float v = static_cast<float>(value < 0 ? -value : value) / 10000.0f;
  return Saturate(v);
}

static inline float Quantize(float v) {
  v = Saturate(v);
  return std::floor(v * 32.0f + 0.5f) / 32.0f;
}

static void Log(const char* fmt, ...) {
  FILE* f = std::fopen("NFSU_XtendedInput_Rumble_U2.log", "a");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fclose(f);
}

struct RumbleState {
  float frameLow;
  float frameHigh;
  float pulseLow;
  float pulseHigh;
  float lastLow;
  float lastHigh;
  DWORD pulseUntil;
  DWORD lastSendTick;
  DWORD lastActiveTick;
  unsigned int frameCounter;
  unsigned int callbackCount[8];
  bool compositeActive;
};

static RumbleState g_states[XUSER_MAX_COUNT]{};

static RumbleState* StateFor(int device) {
  if (device < 0 || device >= XUSER_MAX_COUNT) return nullptr;
  return &g_states[device];
}

static void LogCallback(RumbleState* s, unsigned int slot, const char* fmt, ...) {
  if (!s || slot >= 8) return;
  unsigned int count = ++s->callbackCount[slot];
  if (count > 8 && (count % 240u) != 0u) return;

  FILE* f = std::fopen("NFSU_XtendedInput_Rumble_U2.log", "a");
  if (!f) return;
  std::fprintf(f, "U2Rumble: cb[%u] #%u ", slot, count);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fclose(f);
}

static void AddLow(RumbleState* s, float value) {
  if (!s) return;
  value = Saturate(value);
  if (value > s->frameLow) s->frameLow = value;
}

static void AddHigh(RumbleState* s, float value) {
  if (!s) return;
  value = Saturate(value);
  if (value > s->frameHigh) s->frameHigh = value;
}

static void AddPulse(RumbleState* s, float low, float high, DWORD durationMs) {
  if (!s) return;
  low = Saturate(low);
  high = Saturate(high);
  if (low > s->pulseLow) s->pulseLow = low;
  if (high > s->pulseHigh) s->pulseHigh = high;
  DWORD until = GetTickCount() + durationMs;
  if (static_cast<LONG>(until - s->pulseUntil) > 0) s->pulseUntil = until;
}

static void SendHardware(int device, RumbleState* s, float low, float high, bool force = false) {
  if (!s || device < 0 || device >= XUSER_MAX_COUNT) return;

  DWORD now = GetTickCount();
  if (!force && now - s->lastSendTick < 16) return; // at most ~60 Hz

  low = Quantize(low);
  high = Quantize(high);
  if (!force && AbsF(low - s->lastLow) < 0.025f && AbsF(high - s->lastHigh) < 0.025f) return;

  XINPUT_VIBRATION vibration{};
  vibration.wLeftMotorSpeed = static_cast<WORD>(low * 65535.0f);
  vibration.wRightMotorSpeed = static_cast<WORD>(high * 65535.0f);
  DWORD result = XInputSetState(static_cast<DWORD>(device), &vibration);
  s->lastSendTick = now;

  if (result == ERROR_SUCCESS) {
    s->lastLow = low;
    s->lastHigh = high;
  }

  if ((low > 0.0f || high > 0.0f) && ((s->frameCounter % 120u) == 0u)) {
    Log("U2Rumble: output dev=%d low=%.3f high=%.3f result=%lu",
        device, low, high, static_cast<unsigned long>(result));
  }
}

static void StopHardware(int device, bool force = false) {
  auto* s = StateFor(device);
  if (!s) return;
  s->frameLow = 0.0f;
  s->frameHigh = 0.0f;
  s->pulseLow = 0.0f;
  s->pulseHigh = 0.0f;
  s->pulseUntil = 0;
  s->compositeActive = false;
  if (force || s->lastLow != 0.0f || s->lastHigh != 0.0f) {
    SendHardware(device, s, 0.0f, 0.0f, true);
  }
}

static void Apply(int device) {
  auto* s = StateFor(device);
  if (!s) return;

  DWORD now = GetTickCount();
  float low = s->frameLow;
  float high = s->frameHigh;

  if (static_cast<LONG>(s->pulseUntil - now) > 0) {
    if (s->pulseLow > low) low = s->pulseLow;
    if (s->pulseHigh > high) high = s->pulseHigh;
  } else {
    s->pulseLow = 0.0f;
    s->pulseHigh = 0.0f;
  }

  low = Saturate(low);
  high = Saturate(high);
  if (low > 0.02f || high > 0.02f) {
    s->lastActiveTick = now;
    SendHardware(device, s, low, high, false);
  } else if ((s->lastLow != 0.0f || s->lastHigh != 0.0f) && now - s->lastActiveTick >= 100) {
    SendHardware(device, s, 0.0f, 0.0f, false);
  }
}

// Replaces only FUN_005bfd70 at LocalPlayer/RealDriver's native FFB update callsite.
// This makes U2 execute its native force calculations when an XInput controller is present,
// without restoring DirectInput enumeration or creating a fake DirectInput device.
static bool __stdcall HasXInputRumbleController(int device) {
  auto* s = StateFor(device);
  if (!s) return false;

  XINPUT_STATE state{};
  DWORD result = XInputGetState(static_cast<DWORD>(device), &state);
  if (result != ERROR_SUCCESS) {
    StopHardware(device, false);
    return false;
  }

  ++s->frameCounter;
  s->frameLow = 0.0f;
  s->frameHigh = 0.0f;
  return true;
}

// GUID_Spring (13541C27-8E33-11D0-9AD0-00A0C9A06E35).
// A centering spring makes sense for a wheel but not as permanent gamepad vibration,
// so retain it only for diagnostics and do not map it directly to the motors.
static void __fastcall SpringHook(void*, void*, int device, int center, int saturation, int coefficient) {
  auto* s = StateFor(device);
  LogCallback(s, 0, "Spring dev=%d center=%d saturation=%d coefficient=%d",
              device, center, saturation, coefficient);
}

// GUID_ConstantForce. U2 derives this from steering/vehicle forces.
static void __fastcall ConstantForceHook(void*, void*, int device, int force, int phase) {
  auto* s = StateFor(device);
  float n = Normalize10000(force);
  AddLow(s, n * 0.20f);
  AddHigh(s, n * 0.05f);
  LogCallback(s, 1, "Constant dev=%d force=%d phase=%d norm=%.3f", device, force, phase, n);
}

// GUID_Damper. Keep it subtle; it is mainly steering resistance on a wheel.
static void __fastcall DamperHook(void*, void*, int device, int coefficient) {
  auto* s = StateFor(device);
  float n = Normalize10000(coefficient);
  AddLow(s, n * 0.10f);
  AddHigh(s, n * 0.025f);
  LogCallback(s, 2, "Damper dev=%d coefficient=%d norm=%.3f", device, coefficient, n);
}

// Native force-group enable/disable. The PC implementation uses this to start or stop
// a collection of wheel effects. Keep state for diagnostics; individual translated
// effects below drive the actual motors.
static void __fastcall CompositeStartHook(void*, void*, int device) {
  auto* s = StateFor(device);
  if (s) s->compositeActive = true;
  LogCallback(s, 3, "CompositeStart dev=%d", device);
}

static void __fastcall CompositeStopHook(void*, void*, int device) {
  auto* s = StateFor(device);
  if (s) s->compositeActive = false;
  LogCallback(s, 3, "CompositeStop dev=%d", device);
}

// U2 creates a short 75 ms GUID_Square effect here after abrupt force/suspension changes.
// This maps naturally to a gamepad kick.
static void __fastcall ImpactHook(void*, void*, int device, int magnitude) {
  auto* s = StateFor(device);
  float n = Normalize10000(magnitude);
  AddPulse(s, 0.28f + n * 0.62f, 0.18f + n * 0.52f, 105);
  LogCallback(s, 4, "Impact dev=%d magnitude=%d norm=%.3f", device, magnitude, n);
}

// Native road-surface periodic force. waveform:
// 0=Sine, 1=Square, 2=Triangle, 3=SawtoothUp, 4=SawtoothDown.
// U2 already computes magnitude and period from the current road surfaces and speed.
static void __fastcall SurfaceHook(void*, void*, int device, int waveform, int magnitude, int periodMs) {
  auto* s = StateFor(device);
  float n = Normalize10000(magnitude);

  if (n > 0.01f) {
    float waveBoost = 1.0f;
    if (waveform == 1) waveBoost = 1.10f;       // square: sharper texture
    else if (waveform >= 3) waveBoost = 0.92f;  // sawtooth: slightly less harsh

    AddLow(s, (0.05f + n * 0.25f) * waveBoost);
    AddHigh(s, (0.09f + n * 0.50f) * waveBoost);
  }

  LogCallback(s, 5, "Surface dev=%d wave=%d magnitude=%d periodMs=%d norm=%.3f",
              device, waveform, magnitude, periodMs, n);
  Apply(device);
}

// Special full-strength damper path used by U2 for a specific road/surface state.
static void __fastcall SpecialDamperHook(void*, void*, int device, int magnitude) {
  auto* s = StateFor(device);
  float n = Normalize10000(magnitude);
  AddPulse(s, 0.38f + n * 0.50f, 0.22f + n * 0.34f, 130);
  LogCallback(s, 6, "SpecialDamper dev=%d magnitude=%d norm=%.3f", device, magnitude, n);
  Apply(device);
}

// This call is reached by U2's native FFB cleanup branch. Stop the XInput motors too.
static void __fastcall CleanupHook(void*, void*, int device) {
  auto* s = StateFor(device);
  LogCallback(s, 7, "Cleanup dev=%d", device);
  StopHardware(device, false);
}

static void Install() {
  std::remove("NFSU_XtendedInput_Rumble_U2.log");
  Log("U2Rumble: installing native SPEED2 FFB -> XInput backend");
  Log("U2Rumble: update=0x00406460 hasDeviceCall=0x%08X", static_cast<unsigned int>(kHasFFBDeviceCall));
  Log("U2Rumble: effects Spring/Constant/Damper/Impact/Surface/SpecialDamper");

  injector::MakeCALL(kHasFFBDeviceCall, HasXInputRumbleController, true);
  injector::MakeCALL(kSpringCall, SpringHook, true);
  injector::MakeCALL(kConstantForceCall, ConstantForceHook, true);
  injector::MakeCALL(kDamperCall, DamperHook, true);
  injector::MakeCALL(kCompositeStartCall, CompositeStartHook, true);
  injector::MakeCALL(kCompositeStopCall, CompositeStopHook, true);
  injector::MakeCALL(kImpactCall1, ImpactHook, true);
  injector::MakeCALL(kImpactCall2, ImpactHook, true);
  injector::MakeCALL(kSurfaceCall, SurfaceHook, true);
  injector::MakeCALL(kSpecialDamperCall, SpecialDamperHook, true);
  injector::MakeCALL(kCleanupCall, CleanupHook, true);

  Log("U2Rumble: hooks installed");
}

// NFSU2_Addresses.h is included by the existing XtendedInput translation unit.
// A static initializer lets us install these independent callsite hooks without
// modifying the legacy dllmain.cpp flow. Existing XtendedInput Init() does not
// touch any of the callsites above.
struct AutoInstall {
  AutoInstall() { Install(); }
  ~AutoInstall() {
    for (int i = 0; i < XUSER_MAX_COUNT; ++i) StopHardware(i, true);
  }
};

static AutoInstall g_autoInstall;

} // namespace U2RealRumble

#endif // GAME_UG2
