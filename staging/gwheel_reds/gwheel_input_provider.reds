// gwheel's IModSettingsInputProvider: bridges the Mod Settings InputBinding
// listening UI to the gwheel C++ capture natives. Registered once at
// OnGameAttached (gwheel_settings.reds). The selector controller polls
// PollLatestCapture each frame during listening; on a non-empty return the
// captured tagged token (e.g. "wheel:5", "wheel-axis:lY", "wheel-pov:18000")
// becomes the field's binding value.
//
// ClaimsBinding identifies tokens this provider owns so the display layer
// can route label lookups (Phase 7 will fill LabelForBinding with per-
// device labels like "Right Paddle"; Phase 5 returns the raw value so the
// resolver's generic-tag fallback ("Button 5", "Wheel Y") renders).

public class GWheelInputProvider extends IModSettingsInputProvider {
  public func BeginCapture(typeHint: String) -> Void {
    GWheel_BeginCapture(typeHint);
  }

  public func PollLatestCapture() -> String {
    return GWheel_PollCapturedInput();
  }

  public func CancelCapture() -> Void {
    GWheel_CancelCapture();
  }

  public func ClaimsBinding(value: String) -> Bool {
    return Equals(StrLeft(value, 6),  "wheel:")
        || Equals(StrLeft(value, 11), "wheel-axis:")
        || Equals(StrLeft(value, 10), "wheel-pov:");
  }

  public func LabelForBinding(value: String) -> String {
    return value;
  }
}
