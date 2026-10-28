## Context

See `proposal.md` for motivation and `specs/device-variant-selection/spec.md` for the behavior contract. The controller already carries a per-device variant through configuration, the UI change signal, and SDK initialization as `hardware_variant`. The SDK defines the board mapping and protection behavior. The TCP `powerOn` handler already accepts and forwards a variant; its existing powered-on path does not switch the initialized variant.

## Goals / Non-Goals

**Goals:**
- Add F43V3 through the existing variant/configuration path.
- Identify the supported MAX77675/MAX77713 mapping and SDK package versions in operator documentation.
- Keep variant changes compatible with the current power lifecycle.

**Non-Goals:**
- Change TCP handlers or the JSON protocol.
- Add controller-side PMIC sequencing, retries, or bypasses to SDK guardrails.
- Add SFTP package downloads or add other variants.
- Install packages or perform field hardware operations.

## Decisions

### Reuse the existing variant path

Add the single `F43V3` selector option and leave the existing choice list intact. The existing variant-change/configuration code saves the selected string and restores it when the panel is recreated. Pass the string unchanged through the existing controller and Python bridge initialization path as `hardware_variant`; do not add a second mapping layer in the controller.

**Alternative considered:** Add a separate F43V3 config field or bespoke initialization branch. Rejected because the established variant field already persists and reaches the SDK.

### Leave PMIC behavior to the supported SDK

Document F43V3's MAX77675 address `0x44` and MAX77713 address `0x65` as the SDK-supported mapping. Do not change PMIC control or add MAX77713-specific code: the supported SDK already owns the mapping and guardrails.

**Alternative considered:** Duplicate chip setup or guardrail logic in the controller. Rejected because this could diverge from the SDK and there is no controller bypass to fix.

### Preserve the TCP lifecycle and explain the power cycle

Keep `powerOn` request/response handling unchanged. Add F43V3 to the documented variant list and state that variant changes require `powerOff` before `powerOn` with the new variant because an already-powered device returns success without reinitializing.

**Alternative considered:** Change the already-powered behavior to tear down and reinitialize automatically. Rejected as out of scope and a change to an existing lifecycle contract.

### Document the deployed runtime requirements

Record `ar_display_lab_lib` 2026.8.21.1 and `rj1lib_external` 2026.7.22 in the environment/deployment documentation. State that the actual Controller runtime virtual environment must contain both packages. Release setup creates a separate environment, so updating a development environment alone does not satisfy deployment. Python 3.12 is compatible with the existing Python >=3.10 requirement.

## Risks / Trade-offs

- The UI can select F43V3 when the runtime has an older SDK → document the exact SDK package versions and install location; do not add automatic downloading.
- An operator may request a variant change while the panel is powered on → document the required power-off/power-on sequence and preserve current behavior.
- The MAX77713 protection implementation is SDK-owned → continue to use the SDK's native mapping and guardrails without controller-side PMIC changes.

## Migration Plan

Update the Controller version to 1.1.30, install the listed SDK wheels in the virtual environment actually used by the release, then restart the application and select F43V3 for the intended device. Roll back by selecting a previously supported variant and restoring the prior Controller release; no configuration migration is needed because the existing variant string field stores the selection.
