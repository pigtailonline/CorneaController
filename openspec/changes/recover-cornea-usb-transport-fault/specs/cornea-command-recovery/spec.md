## Purpose

Define reliable, bounded recovery and correlated command outcomes for Cornea panels so transient USB failures cannot leave hardware operations or operator controls permanently stuck.

## ADDED Requirements

### Requirement: Per-serial transient transport recovery
When a Cornea command fails because its USB transport or worker becomes unavailable, the driver SHALL quarantine that serial's failed session, prevent new commands from using it, and attempt bounded recovery using the expected serial. Recovery SHALL succeed only after a newly created session completes initialization and verifies the expected serial. The driver SHALL retry the interrupted command no more than once, and only when recovery and the retry can complete within the original command deadline.

#### Scenario: Serial is rediscovered and initialized
- **WHEN** a command fails with a retryable transport or worker error and the expected serial is rediscovered and initialized within the recovery deadline
- **THEN** the driver creates a new session generation and retries the interrupted command exactly once

#### Scenario: Serial remains unavailable
- **WHEN** the expected serial is not rediscovered or initialized within the recovery deadline
- **THEN** the driver returns a structured recovery failure, releases the command and UI busy states, and does not retry indefinitely

#### Scenario: Recovery exceeds the original command deadline
- **WHEN** recovery cannot complete with enough time to retry before the original command deadline
- **THEN** the driver responds within the original deadline, releases the caller and UI busy state, and prevents any later recovery result from completing that expired command

#### Scenario: Failure is not transient
- **WHEN** a command fails because calibration data is missing, initialization is rejected, or another configured non-retryable condition occurs
- **THEN** the driver returns that terminal failure without transport recovery or automatic command retry

### Requirement: Correlated command deadline
Every Power On and Power Off request SHALL complete within a configured driver-owned deadline shorter than the existing 15-second client timeout, with either a success response or a structured failure response. A timeout response SHALL include the command, serial, `success: false`, `errorCode: "COMMAND_TIMEOUT"`, the applied `timeout_ms`, a correlation `requestId`, and `deviceState: "unknown"`. USB recovery SHALL use a separate bounded budget and SHALL NOT extend the original TCP or UI operation deadline.

#### Scenario: Power operation exceeds its deadline
- **WHEN** a Power On or Power Off operation does not complete before the driver-owned deadline
- **THEN** the driver responds before the existing 15-second client timeout with `COMMAND_TIMEOUT`, the matching or driver-generated `requestId`, the applied timeout, and an unknown device state

#### Scenario: Client supplies a request identifier
- **WHEN** a command includes `requestId`
- **THEN** every success, warning, and failure response for that command echoes the same `requestId`

#### Scenario: Legacy client omits a request identifier
- **WHEN** a supported legacy command does not include `requestId`
- **THEN** the driver continues to accept and execute the command, preserves the existing response fields, and may add a driver-generated `requestId`

### Requirement: Expired result isolation
The driver SHALL associate each asynchronous hardware operation with both a request identifier and a device-session generation. A result from an expired request or superseded generation SHALL NOT update a pending command, device state, TCP response, or UI state.

#### Scenario: Timed-out worker completes late
- **WHEN** a worker result arrives after its request timed out
- **THEN** the result is discarded and cannot satisfy or alter any later command

#### Scenario: Replaced session reports a result
- **WHEN** a result arrives from a worker generation that has been replaced during recovery
- **THEN** the result is discarded without changing the current session or device state

### Requirement: Truthful device power state
The driver and operator UI SHALL represent Cornea power state as `On`, `Off`, or `Unknown`. A transport fault, command timeout, or unverified recovery SHALL set the state to `Unknown`; only a verified successful Power On or Power Off completion for the current session generation SHALL set it to `On` or `Off` respectively.

#### Scenario: Transport is lost while cached state is On
- **WHEN** the current session encounters a transport fault while the last cached power state is On
- **THEN** the exposed power state becomes Unknown rather than retaining On

#### Scenario: Recovery completes without a verified power transition
- **WHEN** a replacement session initializes successfully but no current-generation Power On or Power Off operation has completed successfully
- **THEN** the power state remains Unknown

#### Scenario: Current power transition succeeds
- **WHEN** a current-generation Power On or Power Off operation returns verified success
- **THEN** the power state becomes On or Off to match that operation

### Requirement: Operator controls always leave busy state
Every Power On and Power Off operation SHALL release its operator-control busy state after success, failure, exception, disconnect, timeout, or recovery failure. Command-error presentation SHALL be derived from the current session outcome and SHALL NOT be treated as proof of physical power state.

#### Scenario: Power command times out
- **WHEN** an operator-triggered Power operation reaches its deadline
- **THEN** both Power controls leave the in-progress state and the UI presents a retryable error with Unknown power state

#### Scenario: Recovery succeeds
- **WHEN** per-serial recovery creates and verifies a new session
- **THEN** the UI refreshes connected and error presentation from that current session without inheriting stale flags from the failed generation
