#include "wheel.h"
#include "config.h"
#include "device_table.h"
#include "input_bindings.h"
#include "logging.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace direct_wheel::wheel {
namespace {
struct State {
  IDirectInput8W *pDI = nullptr;
  IDirectInputDevice8W *pWheel = nullptr;
  IDirectInputDevice8W *pPedals = nullptr; // optional separate pedal device
  std::atomic<bool> ready{false};
  std::atomic<bool> hasFFB{false};
  std::atomic<float> globalStrength{1.0f};

  std::mutex snapMtx;
  Snapshot snap{};

  std::mutex capsMtx;
  Caps caps{};

  std::atomic<uint32_t> initAttempts{0};
  std::atomic<bool> handshakeActive{false};

  IDirectInputEffect *pConstantEffect = nullptr;
  IDirectInputEffect *pSpringEffect = nullptr;
  IDirectInputEffect *pDamperEffect = nullptr;
  IDirectInputEffect *pFrictionEffect = nullptr;
  IDirectInputEffect *pSineEffect = nullptr;
  HWND hFFBWindow = nullptr;

  // Jolt state: collision triggers a short high-magnitude pulse
  std::atomic<float> joltMagnitude{0.f};
  std::atomic<uint32_t> joltTicksLeft{0};

  std::atomic<bool> pendingReset{false};
  std::atomic<uint32_t> deferredFFBAttempts{0};
};

State &S() {
  static State s;
  return s;
}

HWND FindOwnGameWindow() {
  const DWORD myPid = GetCurrentProcessId();
  auto ownedByMe = [&](HWND h) -> bool {
    if (!h || !IsWindow(h))
      return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == myPid;
  };
  HWND h = FindWindowW(L"RED4Engine", nullptr);
  if (ownedByMe(h))
    return h;
  h = FindWindowW(L"Cyberpunk 2077", nullptr);
  if (ownedByMe(h))
    return h;

  struct Ctx {
    DWORD pid;
    HWND hit;
  } ctx{myPid, nullptr};
  EnumWindows(
      [](HWND w, LPARAM p) -> BOOL {
        auto *c = reinterpret_cast<Ctx *>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid != c->pid)
          return TRUE;
        if (!IsWindowVisible(w))
          return TRUE;
        c->hit = w;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.hit;
}

float NormalizePedal(LONG v) {
  constexpr float kMax = 65535.f;
  return std::clamp(static_cast<float>(v) / kMax, 0.f, 1.f);
}

float NormalizeSteer(LONG v) {
  constexpr float kMax = 32767.f;
  return std::clamp(static_cast<float>(v - 32767) / kMax, -1.f, 1.f);
}

int ScaleConstant(float v) {
  // Raw mapping [-1..+1] → [-10000..+10000].
  // Per-effect scaling (activeTorqueStrengthPct, constantForcePct)
  // is applied by the caller before passing the value here.
  return static_cast<int>(std::clamp(v, -1.0f, 1.0f) * 10000.0f);
}

int ScaleSpring(float v) {
  auto cfg = config::Current();
  float mul = cfg.ffb.springForcePct / 100.0f;
  return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * mul * 10000.0f);
}

int ScaleDamper(float v) {
  auto cfg = config::Current();
  float mul = cfg.ffb.damperForcePct / 100.0f;
  return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * mul * 10000.0f);
}

int ScaleFriction(float v) {
  auto cfg = config::Current();
  float mul = cfg.ffb.frictionForcePct / 100.0f;
  return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * mul * 10000.0f);
}

int ScaleSine(float v) {
  auto cfg = config::Current();
  float mul = cfg.ffb.sineForcePct / 100.0f;
  return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * mul * 10000.0f);
}

// Device selection: enumerate ALL attached game controllers, log them
// all, then pick the best candidate. Priority:
//   1. Device with FFB support (real wheel)
//   2. Non-virtual device without FFB
//   3. Anything left
// Known virtual devices (vJoy, ViGEm) are deprioritized.

struct DeviceCandidate {
  DIDEVICEINSTANCEW info;
  bool hasFFB = false;
  bool isVirtual = false;
};

static std::vector<DeviceCandidate> g_candidates;

bool IsVirtualDevice(const wchar_t *name) {
  // Common virtual joystick / gamepad emulators
  const wchar_t *kVirtualNames[] = {
      L"vJoy",       L"ViGEm",     L"Virtual", L"Xbox360",
      L"DS4Windows", L"BetterJoy", L"XOutput",
  };
  for (const auto *vn : kVirtualNames) {
    if (wcsstr(name, vn))
      return true;
  }
  return false;
}

BOOL CALLBACK EnumJoysticksCallback(const DIDEVICEINSTANCEW *pdidInstance,
                                    VOID *pContext) {
  auto &st = S();

  // Probe FFB capability by checking DIDC_FORCEFEEDBACK on the device
  IDirectInputDevice8W *probe = nullptr;
  bool hasFFB = false;
  if (SUCCEEDED(
          st.pDI->CreateDevice(pdidInstance->guidInstance, &probe, nullptr))) {
    probe->SetDataFormat(&c_dfDIJoystick2);
    DIDEVCAPS caps = {};
    caps.dwSize = sizeof(DIDEVCAPS);
    if (SUCCEEDED(probe->GetCapabilities(&caps)))
      hasFFB = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    probe->Release();
  }

  char utf8Name[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, pdidInstance->tszProductName, -1, utf8Name,
                      sizeof(utf8Name), nullptr, nullptr);
  bool isVirtual = IsVirtualDevice(pdidInstance->tszProductName);

  log::InfoF("[direct_wheel] device #%d: \"%s\" ffb=%s virtual=%s devType=0x%04lX (type=%d subtype=%d)",
             (int)g_candidates.size() + 1, utf8Name, hasFFB ? "yes" : "no",
             isVirtual ? "yes" : "no",
             (unsigned long)pdidInstance->dwDevType,
             GET_DIDEVICE_TYPE(pdidInstance->dwDevType),
             GET_DIDEVICE_SUBTYPE(pdidInstance->dwDevType));

  DeviceCandidate c;
  c.info = *pdidInstance;
  c.hasFFB = hasFFB;
  c.isVirtual = isVirtual;
  g_candidates.push_back(c);

  return DIENUM_CONTINUE; // keep enumerating — we want ALL devices
}

// After enumeration, pick the best device from g_candidates.
// If config::axes.wheelDeviceName is non-empty, select the first
// candidate whose product name contains that substring (case-insensitive).
// Otherwise fall back to the FFB-score heuristic.
// Returns the index, or -1 if empty / no match.
int SelectBestDevice(const std::string &wheelNameHint) {
  // Helper: case-insensitive substring search
  auto ci_contains = [](const std::string &h, const std::string &n) -> bool {
    if (n.empty())
      return true;
    auto it =
        std::search(h.begin(), h.end(), n.begin(), n.end(), [](char a, char b) {
          return std::tolower((unsigned char)a) ==
                 std::tolower((unsigned char)b);
        });
    return it != h.end();
  };

  // Name-pinned selection
  if (!wheelNameHint.empty()) {
    for (int i = 0; i < static_cast<int>(g_candidates.size()); ++i) {
      char utf8[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, g_candidates[i].info.tszProductName, -1,
                          utf8, sizeof(utf8), nullptr, nullptr);
      if (ci_contains(std::string(utf8), wheelNameHint))
        return i;
    }
    log::WarnF("[direct_wheel] wheelDeviceName \"%s\" not matched — falling "
               "back to heuristic",
               wheelNameHint.c_str());
  }

  // Score-based heuristic: FFB real > non-FFB real > FFB virtual > non-FFB
  // virtual
  int bestIdx = -1;
  int bestScore = -1;
  log::InfoF("[direct_wheel] SelectBestDevice: hint=\"%s\", candidates=%d",
             wheelNameHint.c_str(), (int)g_candidates.size());
  for (int i = 0; i < static_cast<int>(g_candidates.size()); ++i) {
    const auto &c = g_candidates[i];

    if (wheelNameHint.empty()) {
      // If Auto-detecting, completely ignore virtual devices (vJoy, etc.)
      // and standard gamepads (Xbox controllers). They typically have
      // resting trigger values that translate to 100% throttle,
      // causing the car to drive itself when no real wheel is present.
      if (c.isVirtual) {
        log::InfoF("[direct_wheel]   #%d: SKIPPED (virtual)", i);
        continue;
      }

      const int devType = GET_DIDEVICE_TYPE(c.info.dwDevType);
      if ((devType == DI8DEVTYPE_GAMEPAD || devType == DI8DEVTYPE_1STPERSON) && !c.hasFFB) {
        char utf8[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, c.info.tszProductName, -1, utf8, sizeof(utf8), nullptr, nullptr);
        log::InfoF("[direct_wheel]   #%d: SKIPPED (devType=%d is gamepad/1stperson, no FFB) name=\"%s\"", i, devType, utf8);
        continue;
      }
    }

    int score = 0;
    if (!c.isVirtual)
      score += 10;
    if (c.hasFFB)
      score += 5;
    if (score > bestScore) {
      bestScore = score;
      bestIdx = i;
    }
  }
  log::InfoF("[direct_wheel] SelectBestDevice: bestIdx=%d bestScore=%d", bestIdx, bestScore);
  return bestIdx;
}

void InitFFB(State &st) {
  if (!st.pWheel)
    return;
  log::Info("[direct_wheel] InitFFB: starting FFB initialization...");

  DWORD rgdwAxes[1] = {DIJOFS_X};
  LONG rglDirection[1] = {0};

  DIEFFECT diEffect = {0};
  diEffect.dwSize = sizeof(DIEFFECT);
  diEffect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
  diEffect.cAxes = 1;
  diEffect.rgdwAxes = rgdwAxes;
  diEffect.rglDirection = rglDirection;
  diEffect.dwSamplePeriod = 0;
  diEffect.dwGain = DI_FFNOMINALMAX;
  diEffect.dwTriggerButton = DIEB_NOTRIGGER;
  diEffect.dwTriggerRepeatInterval = 0;
  diEffect.dwDuration = INFINITE;

  // Constant Force
  DICONSTANTFORCE cf = {0};
  diEffect.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
  diEffect.lpvTypeSpecificParams = &cf;
  HRESULT hr = st.pWheel->CreateEffect(GUID_ConstantForce, &diEffect,
                                       &st.pConstantEffect, nullptr);
  if (FAILED(hr)) {
    log::WarnF("[direct_wheel] InitFFB: GUID_ConstantForce FAILED hr=0x%08lX",
               (unsigned long)hr);
  } else {
    HRESULT hrd = st.pConstantEffect->Download();
    HRESULT hrs = st.pConstantEffect->Start(1, 0);
    log::InfoF("[direct_wheel] InitFFB: GUID_ConstantForce created OK, "
               "Download=0x%08lX Start=0x%08lX",
               (unsigned long)hrd, (unsigned long)hrs);
  }

  // Spring Force
  DICONDITION spr = {0};
  diEffect.cbTypeSpecificParams = sizeof(DICONDITION);
  diEffect.lpvTypeSpecificParams = &spr;
  hr = st.pWheel->CreateEffect(GUID_Spring, &diEffect, &st.pSpringEffect,
                               nullptr);
  if (FAILED(hr)) {
    log::WarnF("[direct_wheel] InitFFB: GUID_Spring FAILED hr=0x%08lX",
               (unsigned long)hr);
  } else {
    HRESULT hrd = st.pSpringEffect->Download();
    HRESULT hrs = st.pSpringEffect->Start(1, 0);
    log::InfoF("[direct_wheel] InitFFB: GUID_Spring created OK, "
               "Download=0x%08lX Start=0x%08lX",
               (unsigned long)hrd, (unsigned long)hrs);
  }

  // Damper Force
  DICONDITION dmp = {0};
  diEffect.cbTypeSpecificParams = sizeof(DICONDITION);
  diEffect.lpvTypeSpecificParams = &dmp;
  hr = st.pWheel->CreateEffect(GUID_Damper, &diEffect, &st.pDamperEffect,
                               nullptr);
  if (FAILED(hr)) {
    log::WarnF("[direct_wheel] InitFFB: GUID_Damper FAILED hr=0x%08lX",
               (unsigned long)hr);
  } else {
    HRESULT hrd = st.pDamperEffect->Download();
    HRESULT hrs = st.pDamperEffect->Start(1, 0);
    log::InfoF("[direct_wheel] InitFFB: GUID_Damper created OK, "
               "Download=0x%08lX Start=0x%08lX",
               (unsigned long)hrd, (unsigned long)hrs);
  }

  // Friction Force
  DICONDITION fric = {0};
  diEffect.cbTypeSpecificParams = sizeof(DICONDITION);
  diEffect.lpvTypeSpecificParams = &fric;
  hr = st.pWheel->CreateEffect(GUID_Friction, &diEffect, &st.pFrictionEffect,
                               nullptr);
  if (FAILED(hr)) {
    log::WarnF("[direct_wheel] InitFFB: GUID_Friction FAILED hr=0x%08lX",
               (unsigned long)hr);
  } else {
    HRESULT hrd = st.pFrictionEffect->Download();
    HRESULT hrs = st.pFrictionEffect->Start(1, 0);
    log::InfoF("[direct_wheel] InitFFB: GUID_Friction created OK, "
               "Download=0x%08lX Start=0x%08lX",
               (unsigned long)hrd, (unsigned long)hrs);
  }

  // Sine (periodic vibration for road surface)
  DIPERIODIC sine = {0};
  sine.dwMagnitude = 0;
  sine.dwPeriod = 40000; // 40ms = 25Hz base vibration
  diEffect.cbTypeSpecificParams = sizeof(DIPERIODIC);
  diEffect.lpvTypeSpecificParams = &sine;
  diEffect.dwDuration = INFINITE;
  hr = st.pWheel->CreateEffect(GUID_Sine, &diEffect, &st.pSineEffect, nullptr);
  if (FAILED(hr)) {
    log::WarnF("[direct_wheel] InitFFB: GUID_Sine FAILED hr=0x%08lX",
               (unsigned long)hr);
  } else {
    HRESULT hrd = st.pSineEffect->Download();
    HRESULT hrs = st.pSineEffect->Start(1, 0);
    log::InfoF("[direct_wheel] InitFFB: GUID_Sine created OK, Download=0x%08lX "
               "Start=0x%08lX",
               (unsigned long)hrd, (unsigned long)hrs);
  }

  // Turn off autocenter
  DIPROPDWORD dipdw;
  dipdw.diph.dwSize = sizeof(DIPROPDWORD);
  dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
  dipdw.diph.dwObj = 0;
  dipdw.diph.dwHow = DIPH_DEVICE;
  dipdw.dwData = DIPROPAUTOCENTER_OFF;
  st.pWheel->SetProperty(DIPROP_AUTOCENTER, &dipdw.diph);

  st.caps.hasFFB = (st.pConstantEffect || st.pSpringEffect || st.pDamperEffect);
  st.hasFFB.store(st.caps.hasFFB);
}
} // namespace

namespace centering_state {
inline std::atomic<int> s_lastCoefPct{-1};
inline std::atomic<int> s_lastConstPct{INT_MIN};
inline std::atomic<uint64_t> s_lastCallTickMs{0};
inline std::atomic<uint32_t> s_joltDurationMs{0};

inline void Reset() {
  s_lastCoefPct.store(-1, std::memory_order_release);
  s_lastConstPct.store(INT_MIN, std::memory_order_release);
  s_joltDurationMs.store(0, std::memory_order_release);
}
} // namespace centering_state

bool Init() {
  log::Info(
      "[direct_wheel] wheel::Init (DirectInput) - Init deferred to Pump()");
  return true;
}

void Shutdown() {
  auto &st = S();
  st.ready.store(false, std::memory_order_release);
  if (st.pConstantEffect) {
    st.pConstantEffect->Release();
    st.pConstantEffect = nullptr;
  }
  if (st.pSpringEffect) {
    st.pSpringEffect->Release();
    st.pSpringEffect = nullptr;
  }
  if (st.pDamperEffect) {
    st.pDamperEffect->Release();
    st.pDamperEffect = nullptr;
  }
  if (st.pWheel) {
    st.pWheel->Unacquire();
    st.pWheel->Release();
    st.pWheel = nullptr;
  }
  if (st.pPedals) {
    st.pPedals->Unacquire();
    st.pPedals->Release();
    st.pPedals = nullptr;
  }
  if (st.pDI) {
    st.pDI->Release();
    st.pDI = nullptr;
  }
  log::Info("[direct_wheel] wheel::Shutdown complete");
}

bool IsReady() { return S().ready.load(std::memory_order_acquire); }

const Caps &GetCaps() {
  auto &st = S();
  std::lock_guard lk(st.capsMtx);
  static thread_local Caps tl;
  tl = st.caps;
  return tl;
}
namespace {
struct BinderState {
  int target = -1;
  struct DevState {
    IDirectInputDevice8W *pDev = nullptr;
    DIJOYSTATE2 baseState{};
    char name[256]{};
  };
  std::vector<DevState> devs;
  std::atomic<bool> active{false};
  std::chrono::steady_clock::time_point startTime;
};
BinderState g_binder;
} // namespace

static void DoResetDevices(); // forward declaration
static bool g_axesLive = false; // pump thread only; cleared on every device reset

void BeginAxisBinding(int target) {
  auto &st = S();
  if (!st.pDI)
    return;

  g_binder.target = target;
  for (auto &ds : g_binder.devs) {
    if (ds.pDev) {
      ds.pDev->Unacquire();
      ds.pDev->Release();
    }
  }
  g_binder.devs.clear();

  st.pDI->EnumDevices(
      DI8DEVCLASS_GAMECTRL,
      [](const DIDEVICEINSTANCEW *inst, VOID *) -> BOOL {
        IDirectInputDevice8W *pDev = nullptr;
        if (SUCCEEDED(
                S().pDI->CreateDevice(inst->guidInstance, &pDev, nullptr))) {
          pDev->SetDataFormat(&c_dfDIJoystick2);
          pDev->SetCooperativeLevel(GetDesktopWindow(),
                                    DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
          if (SUCCEEDED(pDev->Acquire())) {
            DIJOYSTATE2 js{};
            if (SUCCEEDED(pDev->GetDeviceState(sizeof(DIJOYSTATE2), &js))) {
              BinderState::DevState ds;
              ds.pDev = pDev;
              ds.baseState = js;
              WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, ds.name,
                                  sizeof(ds.name), nullptr, nullptr);
              g_binder.devs.push_back(ds);
            } else {
              pDev->Release();
            }
          } else {
            pDev->Release();
          }
        }
        return DIENUM_CONTINUE;
      },
      nullptr, DIEDFL_ATTACHEDONLY);

  g_binder.startTime = std::chrono::steady_clock::now();
  g_binder.active.store(true, std::memory_order_release);
  log::InfoF("[direct_wheel] Started axis auto-binding for target %d "
             "(listening on %d devices, 5s timeout)",
             target, (int)g_binder.devs.size());
}

void Pump() {
  auto &st = S();
  if (!st.pDI) {
    if (FAILED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
                                  IID_IDirectInput8W, (VOID **)&st.pDI,
                                  nullptr))) {
      return;
    }
  }
  if (g_binder.active.load(std::memory_order_acquire)) {

    // Timeout after 5 seconds
    auto elapsed = std::chrono::steady_clock::now() - g_binder.startTime;
    if (elapsed > std::chrono::seconds(5)) {
      log::Warn("[direct_wheel] Axis binding timed out (5s) — no axis movement "
                "detected");
      g_binder.active.store(false, std::memory_order_release);
      for (auto &cleanupDs : g_binder.devs) {
        if (cleanupDs.pDev) {
          cleanupDs.pDev->Unacquire();
          cleanupDs.pDev->Release();
        }
      }
      g_binder.devs.clear();
      g_binder.target = -1;
      return;
    }

    for (auto &ds : g_binder.devs) {
      if (FAILED(ds.pDev->Poll()))
        ds.pDev->Acquire();
      DIJOYSTATE2 cur{};
      if (SUCCEEDED(ds.pDev->GetDeviceState(sizeof(DIJOYSTATE2), &cur))) {
        const LONG kThreshold = 8000; // ~12% of 65535
        std::string foundAxis;
        if (std::abs(cur.lX - ds.baseState.lX) > kThreshold)
          foundAxis = "lX";
        else if (std::abs(cur.lY - ds.baseState.lY) > kThreshold)
          foundAxis = "lY";
        else if (std::abs(cur.lZ - ds.baseState.lZ) > kThreshold)
          foundAxis = "lZ";
        else if (std::abs(cur.lRx - ds.baseState.lRx) > kThreshold)
          foundAxis = "lRx";
        else if (std::abs(cur.lRy - ds.baseState.lRy) > kThreshold)
          foundAxis = "lRy";
        else if (std::abs(cur.lRz - ds.baseState.lRz) > kThreshold)
          foundAxis = "lRz";
        else if (std::abs(cur.rglSlider[0] - ds.baseState.rglSlider[0]) >
                 kThreshold)
          foundAxis = "slider0";
        else if (std::abs(cur.rglSlider[1] - ds.baseState.rglSlider[1]) >
                 kThreshold)
          foundAxis = "slider1";

        if (!foundAxis.empty()) {
          log::InfoF("[direct_wheel] Auto-bound axis %s on device \"%s\" for "
                     "target %d",
                     foundAxis.c_str(), ds.name, g_binder.target);

          config::SetPedalDeviceName(ds.name);

          if (g_binder.target == 0)
            config::SetAxisThrottle(foundAxis);
          else if (g_binder.target == 1)
            config::SetAxisBrake(foundAxis);
          else if (g_binder.target == 2)
            config::SetAxisClutch(foundAxis);

          g_binder.active.store(false, std::memory_order_release);
          for (auto &cleanupDs : g_binder.devs) {
            if (cleanupDs.pDev) {
              cleanupDs.pDev->Unacquire();
              cleanupDs.pDev->Release();
            }
          }
          g_binder.devs.clear();
          g_binder.target = -1;

          ResetDevices();
          return;
        }
      }
    }
    // While binding, we don't pump normal wheel physics
    return;
  }

  // Handle pending reset from the settings thread (safe: we're on the pump
  // thread)
  if (st.pendingReset.exchange(false, std::memory_order_acq_rel)) {
    DoResetDevices();
    return; // will re-enumerate on next tick
  }

  if (!st.pWheel) {
    g_candidates.clear();
    st.pDI->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumJoysticksCallback, nullptr,
                        DIEDFL_ATTACHEDONLY);

    if (g_candidates.empty())
      return;

    const int bestIdx =
        SelectBestDevice(config::Current().axes.wheelDeviceName);
    if (bestIdx < 0)
      return;

    const auto &chosen = g_candidates[bestIdx];
    HRESULT hr =
        st.pDI->CreateDevice(chosen.info.guidInstance, &st.pWheel, nullptr);
    if (FAILED(hr)) {
      log::WarnF(
          "[direct_wheel] CreateDevice on selected device failed hr=0x%08lX",
          (unsigned long)hr);
      g_candidates.clear();
      return;
    }

    char utf8Name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, chosen.info.tszProductName, -1, utf8Name,
                        sizeof(utf8Name), nullptr, nullptr);
    log::InfoF("[direct_wheel] selected device: \"%s\" (index %d of %d, "
               "ffb=%s, virtual=%s)",
               utf8Name, bestIdx, (int)g_candidates.size(),
               chosen.hasFFB ? "yes" : "no", chosen.isVirtual ? "yes" : "no");
    g_candidates.clear();

    st.pWheel->SetDataFormat(&c_dfDIJoystick2);

    // Initial cooperative level: NONEXCLUSIVE|BACKGROUND so we can
    // read axes immediately even before the game window exists.
    // FFB needs EXCLUSIVE, which we'll set up once HWND is found.
    HWND hwnd = FindOwnGameWindow();
    if (hwnd) {
      HRESULT hrCoop = st.pWheel->SetCooperativeLevel(
          hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
      log::InfoF(
          "[direct_wheel] SetCooperativeLevel(EXCLUSIVE|BACKGROUND) hr=0x%08lX",
          (unsigned long)hrCoop);
    } else {
      log::Info("[direct_wheel] Game window not found yet — using "
                "NONEXCLUSIVE, FFB deferred");
      st.pWheel->SetCooperativeLevel(GetDesktopWindow(),
                                     DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    }

    // Acquire the device
    HRESULT hrAcq = st.pWheel->Acquire();
    log::InfoF("[direct_wheel] Initial Acquire hr=0x%08lX",
               (unsigned long)hrAcq);

    std::lock_guard lk(st.capsMtx);
    std::strncpy(st.caps.productName, utf8Name, 255);
    st.caps.hasFFB = chosen.hasFFB;
    st.caps.operatingRangeDeg = 900;

    // Only init FFB if we got EXCLUSIVE access
    if (hwnd) {
      InitFFB(st);
    }

    st.ready.store(true, std::memory_order_release);
    log::InfoF("[direct_wheel] DirectInput wheel connected: \"%s\"", utf8Name);
    input_bindings::SetDeviceLayout(utf8Name);
  }

  // Open separate pedal device if configured and not yet open.
  // We re-check every pump tick (in case the pedals are plugged in
  // after the wheel). Uses NONEXCLUSIVE|BACKGROUND — pedals need no FFB.
  const auto cfgPedal = config::Current().axes.pedalDeviceName;
  if (!cfgPedal.empty() && !st.pPedals) {
    g_candidates.clear();
    st.pDI->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumJoysticksCallback, nullptr,
                        DIEDFL_ATTACHEDONLY);

    for (const auto &cand : g_candidates) {
      char utf8Name2[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, cand.info.tszProductName, -1, utf8Name2,
                          sizeof(utf8Name2), nullptr, nullptr);

      // Case-insensitive substring match
      std::string haystack(utf8Name2);
      std::string needle(cfgPedal);
      auto ci_contains = [](const std::string &h, const std::string &n) {
        if (n.empty())
          return true;
        auto it = std::search(h.begin(), h.end(), n.begin(), n.end(),
                              [](char a, char b) {
                                return std::tolower((unsigned char)a) ==
                                       std::tolower((unsigned char)b);
                              });
        return it != h.end();
      };

      if (!ci_contains(haystack, needle))
        continue;

      IDirectInputDevice8W *dev = nullptr;
      if (FAILED(st.pDI->CreateDevice(cand.info.guidInstance, &dev, nullptr)))
        continue;
      dev->SetDataFormat(&c_dfDIJoystick2);
      dev->SetCooperativeLevel(GetDesktopWindow(),
                               DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
      HRESULT hrAcq = dev->Acquire();
      log::InfoF("[direct_wheel] pedal device matched: \"%s\" Acquire=0x%08lX",
                 utf8Name2, (unsigned long)hrAcq);
      st.pPedals = dev;
      break;
    }
    g_candidates.clear();
  }

  // Deferred FFB init: if we started without EXCLUSIVE access,
  // retry until the game window appears. Cap at 3 attempts so
  // devices that genuinely lack DirectInput FFB (Moza, Simagic,
  // etc.) don't churn Unacquire/Acquire every tick forever.
  constexpr uint32_t kMaxDeferredFFBAttempts = 3;
  if (st.pWheel && !st.hasFFB.load() && !st.pConstantEffect &&
      st.deferredFFBAttempts.load() < kMaxDeferredFFBAttempts) {
    HWND hwnd = FindOwnGameWindow();
    if (hwnd) {
      uint32_t attempt = st.deferredFFBAttempts.fetch_add(1) + 1;
      st.pWheel->Unacquire();
      HRESULT hrCoop = st.pWheel->SetCooperativeLevel(
          hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
      log::InfoF("[direct_wheel] Deferred FFB attempt %u/%u: "
                 "SetCooperativeLevel(EXCLUSIVE|BACKGROUND) hr=0x%08lX",
                 attempt, kMaxDeferredFFBAttempts, (unsigned long)hrCoop);
      HRESULT hrAcq = st.pWheel->Acquire();
      log::InfoF(
          "[direct_wheel] Deferred FFB attempt %u/%u: Acquire hr=0x%08lX",
          attempt, kMaxDeferredFFBAttempts, (unsigned long)hrAcq);
      InitFFB(st);
      if (!st.hasFFB.load() && attempt >= kMaxDeferredFFBAttempts) {
        log::Warn("[direct_wheel] FFB init failed after max retries — device "
                  "does not support "
                  "DirectInput force feedback (E_NOTIMPL). Wheel axes will "
                  "still work. "
                  "If your wheelbase uses a proprietary SDK (Moza Pit House, "
                  "Simagic, etc.) "
                  "its own centering / FFB profile will remain active.");
      }
    }
  }

  if (FAILED(st.pWheel->Poll())) {
    HRESULT hr = st.pWheel->Acquire();
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
      // Keep trying to re-acquire (FOREGROUND mode loses access on alt-tab)
      hr = st.pWheel->Acquire();
    }
    if (FAILED(hr)) {
      Snapshot s; // all zeros, connected=false
      std::lock_guard lk(st.snapMtx);
      st.snap = s;
      return;
    }
  }

  DIJOYSTATE2 js;
  if (FAILED(st.pWheel->GetDeviceState(sizeof(DIJOYSTATE2), &js))) {
    Snapshot s; // all zeros, connected=false
    std::lock_guard lk(st.snapMtx);
    st.snap = s;
    return;
  }

  // Read pedal state from separate device (if configured and open)
  DIJOYSTATE2 jsPedals = js; // fallback: same device
  if (st.pPedals) {
    if (FAILED(st.pPedals->Poll()))
      st.pPedals->Acquire();
    if (FAILED(st.pPedals->GetDeviceState(sizeof(DIJOYSTATE2), &jsPedals)))
      jsPedals = js; // on failure, fall back to wheel device
  }

  const auto cfg = config::Current();
  const auto axes = cfg.axes;

  Snapshot s;
  s.connected = true;

  float rawSteer = NormalizeSteer(config::AxisMap::Read(js, axes.steer));
  if (cfg.input.invertSteering) rawSteer = -rawSteer;
  s.steer = rawSteer;

  float rawThrottle = NormalizePedal(config::AxisMap::Read(jsPedals, axes.throttle));
  if (cfg.input.invertThrottle) rawThrottle = 1.0f - rawThrottle;
  s.throttle = rawThrottle;

  float rawBrake = NormalizePedal(config::AxisMap::Read(jsPedals, axes.brake));
  if (cfg.input.invertBrake) rawBrake = 1.0f - rawBrake;

  float rawClutch = NormalizePedal(config::AxisMap::Read(jsPedals, axes.clutch));
  if (cfg.input.invertClutch) rawClutch = 1.0f - rawClutch;

  // clutchAsBrake: merge clutch into the brake channel so either pedal
  // can brake. The raw clutch value is preserved separately for the
  // flight module (GetRawClutch) which uses it as an analog air brake.
  if (cfg.input.clutchAsBrake && rawClutch > rawBrake)
      rawBrake = rawClutch;

  s.brake = rawBrake;
  s.clutch = rawClutch;
  { // ChoomLink: a tester's G920 sent every axis as exactly 32767 for 80 s (Nov 2 17:35), i.e. no data, which became
    // gas 0.50 + brake 0.50. Until steer, throttle or brake leaves 32767 once, the wheel adds no input.
    const long rs = config::AxisMap::Read(js, axes.steer), rt = config::AxisMap::Read(jsPedals, axes.throttle),
               rb = config::AxisMap::Read(jsPedals, axes.brake);
    if (!g_axesLive && (rs != 32767 || rt != 32767 || rb != 32767)) {
      g_axesLive = true;
      log::InfoF("[direct_wheel] wheel axes live (steer=%ld thr=%ld brk=%ld)", rs, rt, rb);
    }
    if (!g_axesLive) {
      static ULONGLONG sWarned = 0;
      if (GetTickCount64() - sWarned >= 30000) {
        sWarned = GetTickCount64();
        log::Warn("[direct_wheel] wheel sends no data (every axis 32767): wheel input off until it moves");
      }
      s.steer = s.throttle = s.brake = s.clutch = 0.0f;
    }
  }
  { // ChoomLink: raw pedal values once a second, to tell an idle-pressed pedal from a real press
    static ULONGLONG sLast = 0;
    static int sLines = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - sLast >= 1000 && sLines < 600) { // ~10 min per game, not forever
      sLast = now; ++sLines;
      log::InfoF("[direct_wheel:pedal] raw thr=%ld brk=%ld clu=%ld -> thr=%.2f brk=%.2f clu=%.2f (inv %d/%d)",
                 config::AxisMap::Read(jsPedals, axes.throttle), config::AxisMap::Read(jsPedals, axes.brake),
                 config::AxisMap::Read(jsPedals, axes.clutch), s.throttle, s.brake, s.clutch,
                 cfg.input.invertThrottle, cfg.input.invertBrake);
      log::InfoF("[direct_wheel:pedal] all axes: lX=%ld lY=%ld lZ=%ld lRx=%ld lRy=%ld lRz=%ld slider0=%ld slider1=%ld",
                 jsPedals.lX, jsPedals.lY, jsPedals.lZ, jsPedals.lRx, jsPedals.lRy, jsPedals.lRz,
                 jsPedals.rglSlider[0], jsPedals.rglSlider[1]);
    }
  }
  s.pov = static_cast<uint16_t>(js.rgdwPOV[0] & 0xFFFF);

  uint64_t bits = 0;
  for (int i = 0; i < 64; ++i) {
    if (js.rgbButtons[i] & 0x80)
      bits |= (1ull << i);
  }
  s.buttons = bits;

  {
    std::lock_guard lk(st.snapMtx);
    st.snap = s;
  }
}

Snapshot CurrentSnapshot() {
  auto &st = S();
  std::lock_guard lk(st.snapMtx);
  return st.snap;
}

// Returns pipe-separated list of all attached game controller product names.
// Performs a quick enumeration — called only on user request (device picker),
// not on every tick. Thread-safe: only reads pDI, does not modify state.
std::string GetConnectedDeviceList() {
  auto &st = S();
  if (!st.pDI)
    return "";

  struct Ctx {
    IDirectInput8W *pDI;
    std::string result;
  };
  Ctx ctx{st.pDI, ""};

  st.pDI->EnumDevices(
      DI8DEVCLASS_GAMECTRL,
      [](const DIDEVICEINSTANCEW *inst, VOID *pCtx) -> BOOL {
        auto *c = static_cast<Ctx *>(pCtx);
        char utf8[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, utf8,
                            sizeof(utf8), nullptr, nullptr);
        if (!c->result.empty())
          c->result += '|';
        c->result += utf8;
        return DIENUM_CONTINUE;
      },
      &ctx, DIEDFL_ATTACHEDONLY);

  return ctx.result;
}

// Drop wheel + pedal device handles so Pump() re-opens them on the
// next tick with the updated wheelDeviceName / pedalDeviceName config.
void ResetDevices() {
  // Just set a flag — the pump thread will do the actual reset safely.
  S().pendingReset.store(true, std::memory_order_release);
  log::Info("[direct_wheel] ResetDevices: pending reset requested");
}

// Actually release and re-open devices. Called from the pump thread only.
static void DoResetDevices() {
  auto &st = S();
  st.ready.store(false, std::memory_order_release);

  // Release FFB effects before dropping the device
  if (st.pConstantEffect) {
    st.pConstantEffect->Release();
    st.pConstantEffect = nullptr;
  }
  if (st.pSpringEffect) {
    st.pSpringEffect->Release();
    st.pSpringEffect = nullptr;
  }
  if (st.pDamperEffect) {
    st.pDamperEffect->Release();
    st.pDamperEffect = nullptr;
  }
  if (st.pFrictionEffect) {
    st.pFrictionEffect->Release();
    st.pFrictionEffect = nullptr;
  }
  if (st.pSineEffect) {
    st.pSineEffect->Release();
    st.pSineEffect = nullptr;
  }

  if (st.pWheel) {
    st.pWheel->Unacquire();
    st.pWheel->Release();
    st.pWheel = nullptr;
  }
  if (st.pPedals) {
    st.pPedals->Unacquire();
    st.pPedals->Release();
    st.pPedals = nullptr;
  }
  st.hasFFB.store(false, std::memory_order_release);
  st.deferredFFBAttempts.store(0, std::memory_order_release);
  centering_state::Reset();
  g_axesLive = false;
  log::Info("[direct_wheel] DoResetDevices: devices released, will "
            "re-enumerate on next Pump()");
}

static uint64_t s_cfCount = 0;
void PlayConstant(float magnitude) {
  auto &st = S();
  if (!st.pConstantEffect)
    return;
  DICONSTANTFORCE cf = {0};
  cf.lMagnitude = ScaleConstant(magnitude);
  DIEFFECT eff = {0};
  eff.dwSize = sizeof(DIEFFECT);
  eff.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
  eff.lpvTypeSpecificParams = &cf;
  HRESULT hr = st.pConstantEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS |
                                                           DIEP_START);
  if (FAILED(hr)) {
    st.pWheel->Acquire();
    hr = st.pConstantEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS |
                                                     DIEP_START);
  }
  if (++s_cfCount <= 5 || s_cfCount % 500 == 0) {
    log::InfoF(
        "[direct_wheel:ffb] PlayConstant #%llu mag=%.3f scaled=%d hr=0x%08lX",
        (unsigned long long)s_cfCount, magnitude, (int)cf.lMagnitude,
        (unsigned long)hr);
  }
}
void StopConstant() {
  if (S().pConstantEffect)
    S().pConstantEffect->Stop();
}
void PlayDamper(float coefficient) {
  auto &st = S();
  if (!st.pDamperEffect)
    return;
  int val = ScaleDamper(coefficient);
  DICONDITION cond = {0};
  cond.lPositiveCoefficient = val;
  cond.lNegativeCoefficient = val;
  cond.dwPositiveSaturation = 10000;
  cond.dwNegativeSaturation = 10000;
  DIEFFECT eff = {0};
  eff.dwSize = sizeof(DIEFFECT);
  eff.cbTypeSpecificParams = sizeof(DICONDITION);
  eff.lpvTypeSpecificParams = &cond;
  HRESULT hr = st.pDamperEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS |
                                                         DIEP_START);
  if (FAILED(hr)) {
    st.pWheel->Acquire();
    st.pDamperEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
  }
}
void StopDamper() {
  if (S().pDamperEffect)
    S().pDamperEffect->Stop();
}
void PlaySpring(float coefficient) {
  auto &st = S();
  if (!st.pSpringEffect)
    return;
  int val = ScaleSpring(coefficient);
  DICONDITION cond = {0};
  cond.lPositiveCoefficient = val;
  cond.lNegativeCoefficient = val;
  cond.dwPositiveSaturation = 10000;
  cond.dwNegativeSaturation = 10000;
  DIEFFECT eff = {0};
  eff.dwSize = sizeof(DIEFFECT);
  eff.cbTypeSpecificParams = sizeof(DICONDITION);
  eff.lpvTypeSpecificParams = &cond;
  HRESULT hr = st.pSpringEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS |
                                                         DIEP_START);
  if (FAILED(hr)) {
    st.pWheel->Acquire();
    st.pSpringEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
  }
}
void StopSpring() {
  if (S().pSpringEffect)
    S().pSpringEffect->Stop();
}
void PlayFriction(float coefficient) {
  auto &st = S();
  if (!st.pFrictionEffect)
    return;
  int val = ScaleFriction(coefficient);
  DICONDITION cond = {0};
  cond.lPositiveCoefficient = val;
  cond.lNegativeCoefficient = val;
  cond.dwPositiveSaturation = 10000;
  cond.dwNegativeSaturation = 10000;
  DIEFFECT eff = {0};
  eff.dwSize = sizeof(DIEFFECT);
  eff.cbTypeSpecificParams = sizeof(DICONDITION);
  eff.lpvTypeSpecificParams = &cond;
  HRESULT hr = st.pFrictionEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS |
                                                           DIEP_START);
  if (FAILED(hr)) {
    st.pWheel->Acquire();
    st.pFrictionEffect->SetParameters(&eff,
                                      DIEP_TYPESPECIFICPARAMS | DIEP_START);
  }
}
void StopFriction() {
  if (S().pFrictionEffect)
    S().pFrictionEffect->Stop();
}
void PlaySine(float magnitude) {
  auto &st = S();
  if (!st.pSineEffect)
    return;
  DIPERIODIC sine = {0};
  sine.dwMagnitude = static_cast<DWORD>(ScaleSine(magnitude));
  sine.dwPeriod = 40000; // 25Hz vibration
  DIEFFECT eff = {0};
  eff.dwSize = sizeof(DIEFFECT);
  eff.cbTypeSpecificParams = sizeof(DIPERIODIC);
  eff.lpvTypeSpecificParams = &sine;
  HRESULT hr =
      st.pSineEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
  if (FAILED(hr)) {
    st.pWheel->Acquire();
    st.pSineEffect->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
  }
}
void StopSine() {
  if (S().pSineEffect)
    S().pSineEffect->Stop();
}
void PlayRoadSurface(float magnitude, int periodMs) {}
void StopRoadSurface() {}
void PlayCarAirborne() {}
void StopCarAirborne() {}
void SetGlobalStrength(float mul) { S().globalStrength.store(mul); }
void StopAll() {
  StopConstant();
  StopDamper();
  StopSpring();
  StopFriction();
  StopSine();
}
void PlayLeds(float level) {}
void ClearLeds() {}
bool IsHandshakeActive() { return false; }
void SetSurfaceBaselineMag(float mag) {}
void TriggerJolt(float lateralKick, int durationMs) {
  auto &st = S();
  auto cfg = config::Current();
  float mul = cfg.ffb.joltForcePct / 100.0f;
  st.joltMagnitude.store(lateralKick * mul, std::memory_order_release);
  // Convert duration to ticks at 250Hz
  uint32_t ticks = std::max(1u, static_cast<uint32_t>(durationMs * 250 / 1000));
  st.joltTicksLeft.store(ticks, std::memory_order_release);
}
void UpdateCenteringSpring(float absSpeedMps, float angVelMagRad,
                           float suspensionActivity, float lateralVelocityMps,
                           float steer, float throttle, float brake,
                           bool isReversing, bool isOnGround, bool enabled,
                           float stationaryMps, float cruiseMps,
                           float centeringBaseline, int yawFeedbackPct,
                           float yawRef, int activeTorqueStrengthPct,
                           bool debugLog) {
  if (!enabled || !isOnGround) {
    StopAll();
    return;
  }

  // Below stationary threshold → no forces
  if (absSpeedMps < stationaryMps) {
    StopAll();
    return;
  }

  // Speed factor: ramps 0→1 as speed goes from 0→cruiseMps
  float speedRatio =
      std::clamp(absSpeedMps / std::max(cruiseMps, 1.0f), 0.f, 1.5f);
  float speedSq = speedRatio * speedRatio;

  // Yaw factor
  float yawRatio = std::abs(angVelMagRad) / std::max(yawRef, 0.01f);
  float yawRamp = std::clamp(yawRatio, 0.f, 1.f);

  // Grip factor — decays past yaw limit
  float gripFactor =
      yawRatio < 1.0f ? 1.0f : std::exp(-2.0f * (yawRatio - 1.0f));

  // === Spring: centering force scales with speed² ===
  // Three contributors to spring stiffness:
  //   1) Base centering — from vehicle wheelbase (always present)
  //   2) Constant Force slider — extra speed-based centering stiffness
  //   3) Cornering feedback slider — yaw-based stiffness in turns
  auto cfg2 = config::Current();
  float constantPct = cfg2.ffb.constantForcePct / 100.0f;
  float nonLinPct = cfg2.ffb.springNonLinearityPct / 100.0f;

  float springCoef = std::clamp(
      centeringBaseline * 1.0f * speedSq * gripFactor +
          speedRatio * 0.5f * constantPct // Constant Force: smooth centering
          + yawRamp * 0.5f * (yawFeedbackPct / 100.0f), // Cornering feedback
      0.f, 1.0f);

  if (nonLinPct > 0.0f) {
      float absSteer = std::abs(steer);
      float p = 1.0f - (0.5f * nonLinPct); // 1.0 to 0.5
      float boost = std::pow(std::max(absSteer, 0.01f), p - 1.0f); // x^(p-1)
      springCoef = std::clamp(springCoef * boost, 0.0f, 1.0f);
  }
  if (isReversing)
    springCoef *= 0.4f;
  PlaySpring(springCoef);

  // === Damper: viscous resistance scales with speed ===
  // Base 0.3 so it's felt even at low speed; ramps up to 0.7 at cruise.
  float damperCoef = std::clamp(0.3f + speedRatio * 0.4f, 0.f, 0.7f);
  PlayDamper(damperCoef);

  // === Active Torque: directional push in turns (PlayConstant) ===
  float absSteer = std::abs(steer);

  float steerShape =
      std::sqrt(std::max(absSteer, 0.001f)) * (1.0f - absSteer * absSteer);
  float loadFactor = std::clamp(std::abs(angVelMagRad * absSpeedMps) /
                                    std::max(yawRef * cruiseMps, 0.01f),
                                0.f, 1.5f);
  float weight = 1.0f + 0.3f * brake - 0.05f * throttle;
  float yawPush = -copysignf(1.0f, steer) * steerShape * loadFactor *
                  gripFactor * weight * (activeTorqueStrengthPct / 100.0f);

  float activeForce = yawPush;
  if (isReversing)
    activeForce *= 0.4f;

  // === Jolt: collision pulse override ===
  auto &st = S();
  uint32_t jt = st.joltTicksLeft.load(std::memory_order_acquire);
  if (jt > 0) {
    float jm = st.joltMagnitude.load(std::memory_order_acquire);
    activeForce += jm;
    st.joltTicksLeft.store(jt - 1, std::memory_order_release);
  }

  PlayConstant(std::clamp(activeForce, -1.0f, 1.0f));

  // === Friction: road-texture resistance, scales with speed ===
  float frictionCoef = std::clamp(speedRatio * 0.3f, 0.f, 0.4f);
  PlayFriction(frictionCoef);

  // === Sine: periodic vibration from road roughness ===
  // Base vibration at speed (engine/tire hum) + boost from suspension.
  // Ensures the wheel always has subtle road-feel even on smooth asphalt.
  float sineBase = speedRatio * 0.08f; // subtle hum at cruise
  float sineSusp = suspensionActivity * 2.0f * speedRatio;
  float sineMag = std::clamp(sineBase + sineSusp, 0.f, 0.7f);
  PlaySine(sineMag);
}
} // namespace direct_wheel::wheel
