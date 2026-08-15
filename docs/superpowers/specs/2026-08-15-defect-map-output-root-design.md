# Independent Defect-Map Output Root

## Objective

Allow CorneaController to read large HDF5 calibration files from the existing
local `cal_path` while writing generated defect-map PNG files to a separate
local or SMB/UNC directory.

## Configuration

Add an optional field under `python` in `cornea_config.json`:

```json
{
  "python": {
    "cal_path": "C:/google_env/hdf5_files",
    "defect_map_output_root": "\\\\REMOTE-PC\\DefectMaps"
  }
}
```

`defect_map_output_root` accepts the same path forms Python supports on
Windows, including local absolute paths and UNC share paths. The Windows user
running CorneaController must already have permission to create directories
and files at the destination. Credential management is outside the
application's scope.

## Behavior

- When `defect_map_output_root` is non-empty, maps are written to
  `<defect_map_output_root>/<Panel ID>/`.
- When the field is absent or empty, existing behavior is preserved and maps
  are written to `<cal_path>/defect_maps/<Panel ID>/`.
- HDF5 lookup always continues to use `cal_path`; setting the new field does
  not move or duplicate calibration data.
- Existing export timing and de-duplication remain unchanged: export occurs
  after successful initialization or power-on once a Panel ID is available,
  and the same worker does not re-export an already-exported Panel ID.
- The generated filenames remain unchanged:
  `defect_map_w.png`, `defect_map_r.png`, `defect_map_g.png`,
  `defect_map_b.png`, `defect_map_rgb.png`, and `defect_map_bgr.png`.

## Data Flow

1. `CorneaConfig` loads and saves `python.defect_map_output_root` as part of
   `PythonConfig`.
2. `CorneaWidget` passes it while initializing `PythonBridge`.
3. `PythonBridge` retains the value and includes it in each subprocess
   worker's `init` JSON arguments.
4. `panel_worker.py` selects the independent root when non-empty and otherwise
   uses the legacy `cal_path/defect_maps` destination.

This change applies to the production subprocess path (`use_subprocess=true`),
which owns the existing automatic defect-map export. The embedded-Python
debug fallback does not currently implement that export and remains outside
this change's scope.

## Error Handling

Failure to create or write the output directory—such as an unreachable SMB
share or insufficient permissions—returns `defect_maps.ok=false`, includes the
destination and error text in the worker result, and writes a warning to the
existing log. It must not turn an otherwise successful initialization or
power-on operation into a failure.

No network credentials are stored in `cornea_config.json` and the application
will not automatically retry authentication or mount a share.

## Compatibility

The setting is optional. Existing configuration files continue to work
without edits. Saving an existing configuration writes the field as an empty
string unless a value was configured, preserving the legacy fallback.

## Verification

Automated tests will verify:

- configuration load/save round-trips `defect_map_output_root`;
- the configured value reaches worker `init` arguments;
- the worker chooses `<defect_map_output_root>/<Panel ID>` when configured;
- an absent or empty setting uses `<cal_path>/defect_maps/<Panel ID>`;
- export-path failures remain non-fatal to panel initialization/power-on.

The example configuration and operator documentation will show correct JSON
escaping for a UNC destination and the resulting per-Panel-ID directory.
