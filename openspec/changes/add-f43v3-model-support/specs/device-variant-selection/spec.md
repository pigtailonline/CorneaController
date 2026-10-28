## Purpose

Enables operators and TCP clients to select the F43V3 Cornea hardware variant so the deployed SDK initializes the intended board family and chip addresses.

## ADDED Requirements

### Requirement: F43V3 is selectable and persists as the configured variant

The controller UI SHALL offer `F43V3` as a hardware variant while retaining the existing choices. Selecting it SHALL update the device configuration so that the same variant is restored after the application restarts.

#### Scenario: Select and restore F43V3
- **WHEN** an operator selects `F43V3` for a device and restarts the application
- **THEN** the device configuration retains `F43V3` and the UI restores that selection

### Requirement: F43V3 initialization uses the supported board mapping

When a device is initialized with `F43V3`, the controller SHALL pass `hardware_variant=F43V3` to the supported SDK. The supported SDK mapping for this variant is MAX77675 at address `0x44` and MAX77713 at address `0x65`.

#### Scenario: Initialize a powered-off F43V3 device
- **WHEN** a device configured as `F43V3` is powered on from an unpowered state
- **THEN** initialization reaches the SDK with `hardware_variant=F43V3` and uses the F43V3 MAX77675/MAX77713 mapping

### Requirement: TCP variant selection retains the existing power lifecycle

The TCP `powerOn` command SHALL retain its existing JSON request and response protocol. A requested variant SHALL be used for initialization when the device is not already powered on. If the device is already powered on, changing variants SHALL require a successful `powerOff` followed by `powerOn` with the desired variant.

#### Scenario: Power on with the requested variant
- **WHEN** a TCP client sends the existing `powerOn` request with a variant for a powered-off device
- **THEN** the request keeps the existing JSON protocol and the requested variant is used for initialization

#### Scenario: Change variants while the device is powered on
- **WHEN** a TCP client requests `powerOn` with a different variant while the device is already powered on
- **THEN** the active variant is not switched by that request and the client must power off before powering on with the new variant

### Requirement: F43V3 deployments use the supported SDK package versions

The Controller runtime environment used to initialize F43V3 SHALL have `ar_display_lab_lib` version `2026.8.21.1` and `rj1lib_external` version `2026.7.22` installed. The packages SHALL be installed in the virtual environment actually used by Controller; Python 3.12 satisfies the project's Python `>=3.10` requirement.

#### Scenario: Prepare the Controller runtime
- **WHEN** a Controller deployment is prepared for F43V3
- **THEN** both specified SDK packages are installed in its active runtime virtual environment and the environment uses a supported Python version
