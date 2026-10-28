## Why

The SDK supports the F43V3 controller, but the UI cannot select it and the TCP model reference does not describe its hardware. Adding a named selection lets the controller initialize the SDK with the intended variant and gives operators the deployment/version requirements needed for a compatible setup.

## What Changes

- Add F43V3 to the UI variant selector and preserve it through the existing configuration save/load flow, using MAX77675 at 0x44 and MAX77713 at 0x65.
- Document F43V3 in the TCP API model list and explain that users must power off before selecting a different variant and powering on again.
- Set the displayed application version to 1.1.30 and update related version assertions/metadata.
- Record the supported deployed SDK package versions and that the actual Controller virtual environment must install them; Python 3.12 is supported by the existing Python >=3.10 requirement.
- Keep the existing JSON TCP protocol and SDK-native initialization and protection behavior.

## Capabilities

### New Capabilities

- `device-variant-selection`: Select, persist, restore, and initialize the controller using the F43V3 hardware variant and its supported SDK packages.

### Modified Capabilities

None.

## Impact

Affected code includes the UI variant selector and application version, with targeted updates to version assertions and TCP API documentation. Deployment documentation records `ar_display_lab_lib` 2026.8.21.1 and `rj1lib_external` 2026.7.22 for the Controller's installed environment. No TCP handler or PMIC control changes are required. SFTP auto-download, other new variants, changes to powered-on `powerOn` behavior, and field installation are out of scope.
