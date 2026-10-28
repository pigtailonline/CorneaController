## 1. F43V3 Selection and Initialization

- [x] 1.1 Add F43V3 to the UI variant choices with its MAX77675/MAX77713 mapping note, preserving existing choices and using the existing config save/restore path.
- [x] 1.2 Verify F43V3 is forwarded through the existing controller/SDK initialization path and TCP `powerOn` lifecycle; do not change powered-on behavior.

## 2. Documentation and Version

- [x] 2.1 Update the TCP API model list and explain that changing variants requires `powerOff` before `powerOn` with the selected variant.
- [x] 2.2 Document `ar_display_lab_lib` 2026.8.21.1 and `rj1lib_external` 2026.7.22 in the active Controller runtime virtual environment, noting release setup creates a separate environment and Python 3.12 is supported by Python >=3.10.
- [x] 2.3 Set the displayed application version to 1.1.30 and update the related version assertion/metadata.

## 3. Verification

- [x] 3.1 Run existing TCP variant-forwarding, Power API, and Python worker tests; add no mirror-only UI string test.
- [x] 3.2 Run the available incremental native build and confirm no F43V3 integration regression.

## 4. Field Acceptance

- [ ] 4.1 Verify F43V3 power-on and power-off on a controlled station using the documented SDK package versions.
  - Not run: no field hardware operation is included in this scope.
