# Calibration from print evidence

Use this workflow for a concrete quality problem or a requested calibration in
OrcaSlicer. A working printer does not need a full calibration sequence for every
task. Choose the test that resolves the current uncertainty.

## Establish a reusable baseline

Read the explicit device, reported nozzle, plate, loaded material and selected
printer/filament/process presets through the connected tools. Inspect effective
settings and scoped overrides, including unsaved changes. Ask only for missing
facts that affect the decision, such as the filament identity/condition or the
desired fit, strength or finish. Use existing photos and measurements first.

Before a change, preserve the current project or affected settings in a restorable
form using supported export, preset or recipe operations. Keep saved preset
baselines distinct from the current working configuration; record both when they
differ. Prefer a user preset copy or a scoped project override over overwriting a
known-good preset. Show the relevant `before → after` values, scope and rollback
path. A recipe captures settings for its scope, not necessarily an entire project.

## Run the smallest informative experiment

- State the symptom, hypothesis and observable acceptance criterion before the
  test: for example, less stringing without weaker layer adhesion, or a requested
  clearance on the same mating sample.
- Consider existing evidence of wet filament, a dirty plate, a worn/clogged nozzle
  or mechanical trouble before compensating with a profile. Request a targeted
  check only when it distinguishes plausible causes.
- Change one investigated parameter at a time. A calibration tower may vary that
  parameter within one print; record the range and how regions map to values.
  Keep model, orientation, material and other relevant conditions comparable.
  If coupled changes are necessary, explain them and limit causal conclusions.
- Use the running build's advertised calibration operations or a suitable test
  model. Do not invent tool actions or transplant Creality K1/Klipper commands to
  another printer. This workflow does not require root access, firmware changes,
  Moonraker/Fluidd installation or a slicer upgrade.
- Slice, inspect and submit through the parent skill's existing workflow. Test
  preparation or a valid profile alone does not establish print quality.

## Retain evidence and accept only supported results

Use a local project evidence folder for supplied photos, measurements and test
artifacts. Keep a compact record with date, device/model, nozzle, plate, filament
identity and condition when known, preset names/revisions and scoped overrides,
test/model orientation, changed values, expected result, observed result and
artifact paths. Preserve failed or inconclusive attempts as such.

Distinguish settings/schema validation, completed slicing, upload/queue acceptance,
and physically observed quality. If physical evidence is missing, mark the setting
as a candidate awaiting evaluation. Reuse applicable accepted results; repeat a
test only after a relevant condition changes or a specific uncertainty requires it.
Age alone is a reason to review applicability, not automatically to reprint.

After a successful test, compare against the baseline and, when needed to validate
the intended use, the original model. Save/export the accepted user profile or
recipe revision with its conditions, evidence and rationale. Report remaining
uncertainty; one successful sample does not prove every material or geometry.
Follow the recipe library's applicability and evidence rules when saving there.

Adapted from the workflow discussed in [the source chat](codex://threads/01a15aa5-bbe4-7851-a5e5-30ae2e57d79b)
about [3dprinter.cobanov.dev](https://3dprinter.cobanov.dev/). The transferable part
is the controlled comparison and retained evidence; printer-specific access and
calibration procedures must be checked for the actual device and native build.
