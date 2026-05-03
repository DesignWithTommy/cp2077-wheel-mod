// Push real vehicle telemetry (engine RPM, radio on/off, transmission
// gear) from the game's own Blackboard into the gwheel plugin so the
// LED rev strip shows actual simulated RPM scaled by the current gear's
// shift point (the bar saturates at the AI's upshift, not at global
// redline), and so the music-visualizer mode fires from the game's
// authoritative radio state rather than audio-amplitude heuristics.
//
// Blackboard fields used (VehicleDef, scripts/core/blackboard/blackboardDefinitions.script):
//   RPMValue       : Float   — current engine RPM, driven by the audio/
//                              gameplay system (same value car_hud.script
//                              feeds its RPM gauge widget).
//   GearValue      : Int32   — current transmission gear; 0=neutral,
//                              positive=forward gear, negative=reverse.
//   VehRadioState  : Bool    — true while the in-car radio receiver is on.
//
// Engine profile (idle / redline / per-gear shift RPMs) is walked once
// from VehicleRecord.VehEngineData() on attach and pushed to native
// via GWheel_SetEngineProfile so the LED tick can look up the active
// gear's shift point without re-reading TweakDB record handles each
// frame.
//
// Lifecycles that must all converge to an attached listener set:
//   - Fresh mount (VehicleFinishedMountingEvent) — happens on entering
//     a vehicle normally
//   - Save load while inside a vehicle (PlayerPuppet.OnGameAttached) —
//     the mount event doesn't replay post-load, so we re-attach here
//   - Unmount (UnmountingEvent) — tears down listeners and clears state

@addField(VehicleComponent)
public let m_gwheelRpmBbId: ref<CallbackHandle>;

@addField(VehicleComponent)
public let m_gwheelGearBbId: ref<CallbackHandle>;

@addField(VehicleComponent)
public let m_gwheelRadioBbId: ref<CallbackHandle>;

// Per-VehicleComponent engine-profile cache. The TweakDB walk that
// populates these fields uses Handle<>-returning getters (Vehicle_Record,
// VehicleEngineData_Record, VehicleGear_Record) which leak record
// handles per call. Three remounts of the same bike previously triggered
// three walks; the third dereferenced a freed record at
// Cyberpunk2077.exe+0x72512B reading 0xFFFFFFFFFFFFFFFF (Rancho Coronado
// repro, 2026-05-02). Caching the walk result on the VehicleComponent
// (which persists for the vehicle's lifetime) bounds the leak to ~one
// walk per unique vehicle ever mounted, instead of one per mount event.
//
// The cached values are still pushed to the C++ global engine-profile
// state on EVERY mount (cheap atomic stores, no leaks) so switching
// vehicles correctly updates the LED bar's redline reference. The C++
// side has an analogous per-vehicle-pointer cache in
// tweakdb_physics::ResolvePerVehicle covering its own GetRecord call.
@addField(VehicleComponent)
public let m_gwheelEngineProfileCached: Bool;

@addField(VehicleComponent)
public let m_gwheelIdleRpm: Float;

@addField(VehicleComponent)
public let m_gwheelRedlineRpm: Float;

@addField(VehicleComponent)
public let m_gwheelPerGearShifts: array<Float>;

// Resolve max-RPM, register Blackboard listeners, seed initial state.
// Idempotent: if the listeners are already attached (m_gwheelRpmBbId
// is set) this short-circuits. Safe to call from any lifecycle event
// that might be the "first" one to observe the mounted vehicle.
@addMethod(VehicleComponent)
public func GWheelAttach() -> Void {
  if IsDefined(this.m_gwheelRpmBbId) {
    return;  // already attached for this vehicle instance
  }

  let vehicle: ref<VehicleObject> = this.GetVehicle();
  if !IsDefined(vehicle) {
    return;
  }

  // Resolve this vehicle's engine profile and cache it on the
  // VehicleComponent. Walk runs once per VehicleComponent (TweakDB
  // Handle<>-returning getters leak per call); push to the C++ global
  // state runs on every mount (cheap atomic stores, no leaks) so the LED
  // bar tracks the active vehicle's redline correctly across vehicle
  // switches. Tanks / oddball vehicles where the record lookup fails
  // get an empty profile and the LED tick falls back to "bar always
  // dark" (the FFB pipeline already excludes tanks via
  // ChassisKind::Unknown).
  if !this.m_gwheelEngineProfileCached {
    let record: wref<Vehicle_Record> = vehicle.GetRecord();
    if IsDefined(record) {
      let engineData: wref<VehicleEngineData_Record> = record.VehEngineData();
      if IsDefined(engineData) {
        this.m_gwheelIdleRpm = engineData.MinRPM();
        this.m_gwheelRedlineRpm = engineData.MaxRPM();
        let gearCount: Int32 = engineData.GetGearsCount();
        let gearIdx: Int32 = 0;
        while gearIdx < gearCount {
          let gear: wref<VehicleGear_Record> = engineData.GetGearsItem(gearIdx);
          if IsDefined(gear) {
            ArrayPush(this.m_gwheelPerGearShifts, gear.MaxEngineRPM());
          } else {
            ArrayPush(this.m_gwheelPerGearShifts, this.m_gwheelRedlineRpm);
          }
          gearIdx += 1;
        }
      }
    }
    this.m_gwheelEngineProfileCached = true;
  }
  GWheel_SetEngineProfile(this.m_gwheelIdleRpm, this.m_gwheelRedlineRpm, this.m_gwheelPerGearShifts);

  let bb: ref<IBlackboard> = vehicle.GetBlackboard();
  if !IsDefined(bb) {
    return;
  }

  // Seed initial values. For radio we OR two sources because either
  // can momentarily disagree with reality on save-load:
  //   - The native IsRadioReceiverActive() can return false for a
  //     beat after attach while the radio component is still spinning
  //     up (the symptom: load-into-car-with-radio-on shows rev strip
  //     instead of music viz until the user toggles off/on).
  //   - The Blackboard field VehRadioState reflects the saved state
  //     immediately but the listener we register below only fires on
  //     CHANGES, so a true-at-load value would never be delivered.
  // OR-ing them ensures we pick up "radio is on" from whichever
  // source already knows it. Subsequent state changes still flow
  // through the listener registered just below.
  let initialRpm: Float = bb.GetFloat(GetAllBlackboardDefs().Vehicle.RPMValue);
  GWheel_SetEngineRpmRaw(initialRpm);
  let initialGear: Int32 = bb.GetInt(GetAllBlackboardDefs().Vehicle.GearValue);
  GWheel_SetCurrentGear(initialGear);
  let radioOn: Bool = vehicle.IsRadioReceiverActive()
                      || bb.GetBool(GetAllBlackboardDefs().Vehicle.VehRadioState);
  GWheel_SetRadioActive(radioOn);

  // Subscribe to change events.
  this.m_gwheelRpmBbId = bb.RegisterListenerFloat(
    GetAllBlackboardDefs().Vehicle.RPMValue, this, n"OnGwheelRpmChanged");
  this.m_gwheelGearBbId = bb.RegisterListenerInt(
    GetAllBlackboardDefs().Vehicle.GearValue, this, n"OnGwheelGearChanged");
  this.m_gwheelRadioBbId = bb.RegisterListenerBool(
    GetAllBlackboardDefs().Vehicle.VehRadioState, this, n"OnGwheelRadioChanged");

  // The C++ plugin tracks in-vehicle context via
  // GWheel_Set/ClearPlayerVehicle, normally set by the mount event
  // wrappers in gwheel_mount.reds. Save-load-in-vehicle skips those,
  // so explicitly assert the cached pointer here as a safety net.
  GWheel_SetPlayerVehicle(vehicle);

  // Save-load-with-radio-on recovery. At this exact moment both the
  // native IsRadioReceiverActive() and the Blackboard's VehRadioState
  // can read false because the radio component hasn't fully spun up
  // yet — the false→true transition that follows happens before our
  // listener (registered just above) fires for the first time, so we
  // never get notified of the saved state. Schedule a few delayed
  // re-reads to catch the radio after it settles. Cheap to do
  // unconditionally; the LED controller already gates on
  // visualizerWhileMusic so a false reading is a no-op.
  let delaySys: ref<DelaySystem> = GameInstance.GetDelaySystem(vehicle.GetGame());
  if IsDefined(delaySys) {
    delaySys.DelayCallback(GWheelRadioReseedCallback.Create(this), 0.5);
    delaySys.DelayCallback(GWheelRadioReseedCallback.Create(this), 1.5);
    delaySys.DelayCallback(GWheelRadioReseedCallback.Create(this), 3.0);
  }
}

// Re-seeds radio state from whichever source has the truth right
// now. Run via DelayCallback at progressively later offsets after
// GWheelAttach to bridge the post-load gap before the radio
// component finishes spinning up. Idempotent — if the listener has
// already pushed a fresh state, this just confirms it.
@addMethod(VehicleComponent)
public func GWheelReseedRadio() -> Void {
  let vehicle: ref<VehicleObject> = this.GetVehicle();
  if !IsDefined(vehicle) { return; }
  let bb: ref<IBlackboard> = vehicle.GetBlackboard();
  if !IsDefined(bb) { return; }
  let radioOn: Bool = vehicle.IsRadioReceiverActive()
                      || bb.GetBool(GetAllBlackboardDefs().Vehicle.VehRadioState);
  GWheel_SetRadioActive(radioOn);
}

public class GWheelRadioReseedCallback extends DelayCallback {
  private let vc: wref<VehicleComponent>;

  public static func Create(v: ref<VehicleComponent>) -> ref<GWheelRadioReseedCallback> {
    let cb = new GWheelRadioReseedCallback();
    cb.vc = v;
    return cb;
  }

  public func Call() -> Void {
    let v = this.vc;
    if IsDefined(v) {
      v.GWheelReseedRadio();
    }
  }
}

// Tear down listeners + clear plugin state. Idempotent.
@addMethod(VehicleComponent)
public func GWheelDetach() -> Void {
  let vehicle: ref<VehicleObject> = this.GetVehicle();
  if IsDefined(vehicle) {
    let bb: ref<IBlackboard> = vehicle.GetBlackboard();
    if IsDefined(bb) {
      if IsDefined(this.m_gwheelRpmBbId) {
        bb.UnregisterListenerFloat(GetAllBlackboardDefs().Vehicle.RPMValue, this.m_gwheelRpmBbId);
      }
      if IsDefined(this.m_gwheelGearBbId) {
        bb.UnregisterListenerInt(GetAllBlackboardDefs().Vehicle.GearValue, this.m_gwheelGearBbId);
      }
      if IsDefined(this.m_gwheelRadioBbId) {
        bb.UnregisterListenerBool(GetAllBlackboardDefs().Vehicle.VehRadioState, this.m_gwheelRadioBbId);
      }
    }
  }
  this.m_gwheelRpmBbId = null;
  this.m_gwheelGearBbId = null;
  this.m_gwheelRadioBbId = null;

  GWheel_SetEngineRpmRaw(0.0);
  GWheel_SetCurrentGear(0);
  GWheel_ClearEngineProfile();
  GWheel_SetRadioActive(false);
}

@wrapMethod(VehicleComponent)
protected cb func OnVehicleFinishedMountingEvent(evt: ref<VehicleFinishedMountingEvent>) -> Bool {
  let result: Bool = wrappedMethod(evt);
  let character: ref<GameObject> = evt.character as GameObject;
  if IsDefined(character) && character.IsPlayer() && evt.isMounting {
    this.GWheelAttach();
  }
  return result;
}

@wrapMethod(VehicleComponent)
protected cb func OnUnmountingEvent(evt: ref<UnmountingEvent>) -> Bool {
  let result: Bool = wrappedMethod(evt);
  let game: GameInstance = this.GetVehicle().GetGame();
  let childId: EntityID = evt.request.lowLevelMountingInfo.childId;
  let mountChild: ref<GameObject> = GameInstance.FindEntityByID(game, childId) as GameObject;
  if IsDefined(mountChild) && mountChild.IsPlayer() {
    this.GWheelDetach();
  }
  return result;
}

// Callbacks must live on VehicleComponent so `this` binds correctly
// when RegisterListenerFloat / RegisterListenerBool dispatch them.
// Top-level `cb func` without @addMethod is treated as static and
// fails to compile with UNEXPECTED_THIS.
@addMethod(VehicleComponent)
protected cb func OnGwheelRpmChanged(value: Float) -> Bool {
  GWheel_SetEngineRpmRaw(value);
  return true;
}

@addMethod(VehicleComponent)
protected cb func OnGwheelGearChanged(value: Int32) -> Bool {
  GWheel_SetCurrentGear(value);
  return true;
}

@addMethod(VehicleComponent)
protected cb func OnGwheelRadioChanged(value: Bool) -> Bool {
  GWheel_SetRadioActive(value);
  return true;
}

// Belt-and-suspenders: the Blackboard listener should catch every radio
// on/off transition, but if the game ever routes a toggle through a
// code path that doesn't update the Blackboard field, this wrap on the
// explicit event guarantees the plugin sees it. Re-read via the native
// method (state may settle async inside wrappedMethod's handler).
@wrapMethod(VehicleComponent)
protected cb func OnRadioToggleEvent(evt: ref<RadioToggleEvent>) -> Bool {
  let result: Bool = wrappedMethod(evt);
  let vehicle: ref<VehicleObject> = this.GetVehicle();
  if IsDefined(vehicle) {
    GWheel_SetRadioActive(vehicle.IsRadioReceiverActive());
  }
  return result;
}

// Save-load-in-vehicle recovery. PlayerPuppet.OnGameAttached fires after
// every save load; VehicleFinishedMountingEvent does NOT replay for a
// vehicle that was already mounted when the save was written. If V is
// currently mounted at attach time, run the same attach path we'd have
// run on a fresh mount.
@wrapMethod(PlayerPuppet)
protected cb func OnGameAttached() -> Bool {
  let result: Bool = wrappedMethod();
  let vehicle: wref<VehicleObject> = GetMountedVehicle(this);
  if IsDefined(vehicle) {
    let vc: ref<VehicleComponent> = vehicle.GetVehicleComponent();
    if IsDefined(vc) {
      vc.GWheelAttach();
    }
  }
  return result;
}
