# Orca MCP recipe library contract v1

**English** | [Русский](CONTRACT.ru.md)

Status: **implemented in MCP 0.4.0**, October 25, 2026. The server creates the database automatically in the user's private directory. The library contract is version 1 and does not change Orca wire protocol v1 / native API revision 3.

The library is part of the existing Python MCP. It stores small, reproducible sets of settings, checks them against the selected Orca window, and composes a single native apply operation. Local storage, import, and export work without an account or a dedicated backend.

## 1. Scope of the first version

A recipe contains **explicit setting assignments for one scope**, rather than a complete printer profile or a mutable “use latest” reference. Several recipes can be combined for one object/scope. One plan is applied through one `settings.patch` with native validation and Undo. Separate plans are created for different objects or scopes: there is no shared atomic transaction across them.

Supported scopes: `process`, `plate`, `object`, `volume`, `height_range`, only within the keys permitted by Orca itself. For example, an `object` recipe can be applied to any explicitly selected object, but cannot automatically become a `process` recipe. Changing scope requires a new revision/fork and a contract check.

In v1, a recipe only sets `values`. Removing overrides, saving profiles, importing models, selecting devices, slicing, and submitting prints remain separate existing operations. The format has no composite recipes with executable dependencies, scripts, or arbitrary native commands. A resulting plan can combine up to 16 exact revisions.

A full public catalog, automatic updates from URLs, and text sharing codes are outside v1. The required portable exchange format is a JSON dump. Author and source fields preserve provenance for a future catalog.

## 2. Recipe, revision, and identity

Machine-readable schema: [recipe.schema.json](recipe.schema.json); definitions: [contract.schema.json](contract.schema.json).

A record consists of a `payload` and the `sha256` of its canonical content.

| Payload field | Meaning |
|---|---|
| `format`, `format_version` | `orca.recipe`, `1` — portable format version |
| `recipe_id`, `revision` | Permanent recipe UUID and positive immutable revision number |
| `parent` | Complete reference to the previous revision of this ID; `null` for the first revision |
| `derived_from` | Reference to the source revision of another recipe for a fork; otherwise `null` |
| `name`, `description`, `tags` | Effect name, explanation, and categories |
| `scope`, `values` | One scope and `key → serialized Orca string` |
| `native_contract` | Observed API version and signature of each key used |
| `compatibility` | Explicit constraints on printer model, materials, nozzle, and layer height |
| `evidence` | Reported physical test results with conditions; not a quality certificate |
| `created_at`, `author`, `license`, `source_url` | Revision timestamp and optional provenance |
| `deprecated`, `deprecation_note` | Author's warning that this revision is deprecated |

A complete `RevisionRef` always contains `recipe_id`, `revision`, and `sha256`. Apply never accepts just a name or `latest`. Search returns an exact reference, the current `head_version`, and archive state.

The first revision has `revision=1, parent=null`. For N>1, the parent has the same ID, number N−1, and exact hash; the source revision must be available in the library or the imported dump. Cyclic provenance is forbidden. An unknown `derived_from` is allowed as explicitly marked incomplete provenance; it is not a dependency and is not fetched automatically.

Once written, a revision is immutable, including its description and evidence. A correction creates a new revision. The default local change to an imported recipe is `fork`: a new ID, revision 1, and `derived_from` pointing to the exact source. Explicit `revise` keeps the ID and requires the current `expected_head_version`; it does not establish authorship. In addition, the head must be the highest stored revision of this ID. If import has already stored newer revisions without advancing the head, revise returns `head_not_latest`: the client must explicitly advance the head through import.prepare/commit or create a fork. Occupied revision numbers are never reused.

The same ID/number/hash on import is a no-op. The same ID/number with different hashes produces `identity_conflict`, without overwriting or partial import. Matching names do not imply matching recipes.

### Canonical representation

`sha256` is the lowercase hex SHA-256 of the UTF-8 representation of **payload according to RFC 8785 (JCS)**. The `sha256` field itself, source-file indentation, import time, and local notes are excluded from the hash. The same payload must produce the same hash in every client.

Recipes v1 use only integers up to 2^53−1, strings, bool, null, arrays, and objects. Decimal measurements in compatibility are strings, for example `"0.4"`; Orca values are also strings (`"4"`, `"30%"`, `"1"`). This does not permit arbitrary strings: the native API performs final parsing. Unpaired UTF-16 surrogates, duplicate JSON keys, and nonnumeric NaN/Infinity literals are rejected before hashing. Unicode text normalization is not performed.

## 3. Compatibility and freshness

`native_contract` is a snapshot captured when a revision is created: `protocol_version`, `api_revision`, `app_version`, `definitions`. The set of definitions keys must exactly match values. Each entry contains the native integer `type` and the `sha256` of the significant part of the definition.

A key signature is the JCS/SHA-256 of `{scope,key,type,min,max,enum_values,units}` from `settings.describe` for the selected scope. Missing min/max/units become `null`; missing enum_values becomes `[]`; no other fields are added. Signatures exclude translatable label/category. The readonly/availability level is checked separately by the key's presence in the writable schema. Presence in general config is insufficient.

The server requests every required page of the schema and values, not just the first page. An API number change alone proves neither incompatibility nor compatibility: protocol, required methods, scope, types, and key signatures are checked.

The `check` result distinguishes:

- `compatible` — the structural contract and declared constraints are confirmed by current data;
- `migration_required` — a key was removed or renamed, or its significant definition changed;
- `incompatible` — a known condition is definitely violated;
- `unknown` — a required condition cannot be checked with the available data.

Checked compatibility constraints concern the current project profile/settings. Physical compatibility of the selected printer and slots remains a `printing.prepare/submit` check. These are separate checks.

For `filament_types`, **all materials affected by the application** must be in the list. Using only the first slot or the object's base material is not sufficient when painting/supports are present. Incomplete identification of applicable materials produces `unknown`, rather than a guess. All applicable nozzle diameters are also checked. The layer_height range is inclusive, min≤max, and decimal strings are compared as numbers. An unsatisfied or undetermined declared condition blocks plan; there is no force flag.

An absent constraint means “the author did not specify a condition,” not “tested for everything.” Without positive evidence matching the current verifiable conditions, an `unverified_recipe` warning is created. An unsuccessful test does not remove this warning and adds `unsatisfactory_evidence`; incomplete or different conditions produce `evidence_environment_changed`. A positive matching imported report is still marked `reported_evidence`: it does not become trusted evidence from MCP itself. These warnings require acknowledgement. Evidence from an imported file remains a claim by its source. Matching schemas do not prove identical physical behavior across Orca versions.

A record's age alone does not make it incompatible. The date and environment of the last reported test, availability of a newer locally known revision, deprecated state, and current check result are shown. A new revision is offered for review but never replaces an old one automatically. A previously prepared plan retains exact references to its source revisions.

## 4. Composition and apply plan

[plan.schema.json](plan.schema.json), [requests.schema.json](requests.schema.json).

Plan input: exact references to 1–16 revisions, one `binding`, and optional conflict resolutions. Binding contains the full window UUID pair, scope, current plate_id, state_token, and required object_id/volume_id/height_range. Process changes the project's edited process; object/volume/range affect all associated instances; this is explicitly shown in impact/warnings.

All recipes must have the same scope as the binding. For create/revise/fork, draft.scope must also match capture_binding.scope. Identical values for a key are merged while retaining all sources. Different values for a key produce `composition_conflict`: list order does not select a winner. To continue, the client supplies `resolutions=[{key,chosen_value}]`, where chosen_value is one of the conflicting strings. Extraneous, duplicate, or non-conflict resolutions are rejected.

Plan must:

1. Check hashes, revision availability, and the current native contract; collect a consistent snapshot of initial effective/own values. state_token must match at the start and end of reading all pages.
2. Show the diff, provenance, and warnings. `before_own=null` means inheritance. Even when the effective value is equal, creating an override is shown as `changes_ownership=true`.
3. Convert Binding through the existing scope_params: height_range becomes the native `range` field, and target is passed to native params; UI selectors are not guessed. Freeze one native `settings.patch` with the original target/owner/state_token and a separate UUID `native_request_id`. The descriptor/auth secret is absent from the plan and database.
4. Check **≤256 keys** and **≤65 536 bytes for the complete JSON-RPC request**, including method, id, target, and state_token. A separate string-length limit does not replace the complete-request check.
5. Store an immutable plan with a UUID, library_id, 10-minute TTL, schema fingerprint, and `plan_sha256`. The plan fingerprint is the JCS/SHA-256 of all public Plan fields except plan_sha256 itself. Fields do not change after creation.

`schema_fingerprint` is the JCS/SHA-256 of `{protocol_version:1,scope,definitions:[...]}`, where definitions are the significant definition objects from section 3, sorted by key. app_version/api_revision are excluded from this fingerprint: they are separate observed metadata.

The validation level must be `schema_checked_native_validation_pending`. The current native API has no validate-only operation for the complete effective configuration. Plan checks shape, availability, declared constraints, and known enums/ranges, but **the native settings.patch performs complete candidate and inheritance-chain validation during apply, before writing**. A plan must not promise this validation in advance.

`apply` receives plan_id, the exact plan_sha256, and IDs of acknowledged warnings. All warnings with acknowledgement_required=true must be accepted; unknown/duplicate IDs are rejected. The server rechecks the window, owner, state_token, schema, expiry, and archive state of source recipes. Advancing the head to a new revision does not replace an already pinned revision. Archiving a source recipe blocks application that has not yet started.

On repeated apply, access to this library, request_id/argument fingerprint, and plan_sha256 are checked first. For an already claimed plan, the stored Operation is returned even after TTL, state changes, or archiving, without another dispatch. Only an unclaimed plan passes freshness checks and begins application. An incorrect hash returns an error and does not allow the operation to be retrieved/claimed.

Exactly one frozen native patch is applied, without selecting a new window, refreshing state_token, or automatic replanning. If own values already exactly equal all planned values, a confirmed no-op is returned without native dispatch: Operation.status=succeeded, verification=unchanged, native_job_id=null, native_state=null; native_request_id is pinned but not sent. Stale state produces `conflict`/`stale_plan`; the client reads again and makes a decision.

Applying recipes does not start slicing/printing. Subsequent preview, slicing, reporting, and submission confirmation remain in the existing skill. Recipe display names, descriptions, and notes are data, not instructions to the agent.

## 5. Completion, uncertainty, and multiple clients

[operation.schema.json](operation.schema.json).

Every mutating library/plan operation receives a UUID request_id. It is unique within library_id and is stored with the canonical fingerprint of **tool + action + parameters** and the result. The same ID and arguments return the previous operation/result; different arguments produce `request_conflict`. Receipts are not automatically deleted in v1. `wait_seconds` applies only to observation and does not create a new action.

Before the first native I/O, the server claims the plan, creates a durable operation, and stores the exact native request body in one SQLite transaction. After commit, the status is `dispatching`. Two clients cannot dispatch one plan twice. Any repeated apply for a claimed plan_id returns the existing operation, including failed/outcome_unknown; a new request_id does not redispatch this plan.

States:

| status | Meaning |
|---|---|
| `dispatching` | Claim recorded; dispatch has started or its outcome has not yet been recorded |
| `observing` | Native job_id is known; observe only that job |
| `succeeded` | Native job succeeded and values were verified; or a confirmed no-op with verification=unchanged and null native job/state |
| `failed` | Definite refusal before dispatch, or a native error with verified absence of changes |
| `outcome_unknown` | Insufficient evidence of the outcome; retain intent and receipt |

Operation.recovery is mandatory: `{action,message}`, where action is `none`, `observe_job`, or `inspect_scene_do_not_resend`. action=none is forbidden for outcome_unknown; message explains that no resend occurs. For observing with a known job, action=observe_job.

Readback after a native patch uses exact batches of ≤100 keys and checks state_token consistency. A patch receipt may contain only the first page and does not replace full verification. The verification result separately distinguishes matched/unchanged/not_matched/superseded/unavailable/pending. If the project changes after native success but before verification, native_state=succeeded is retained, but the operation remains outcome_unknown with verification=superseded; this does not assert that the native patch did not execute.

`operation.get` returns durable state and, if job_id is known and the original window is alive, updates it through jobs.get. This observation may update the local receipt, but does not create/redispatch native work or change revisions/heads. Losing the MCP connection/wait does not cancel the native job. After restart, unfinished dispatching without a receipt is treated as outcome_unknown; observing with a known job_id can continue to be observed. If native history has already been lost, the API cannot prove the old outcome by request_id.

Resending the native patch is not automatic recovery in v1. Permitted manually organized recovery requires exactly the original native_request_id and body, subject to limited native retention; a new ID or fresh state_token does not count as recovery. Outcome_unknown cannot be “fixed” with a new apply. An intentional repeat application requires reading current state, a new plan, and an explicit client decision. There is no automatic Undo/rollback over possible manual edits.

SQLite uses WAL, foreign_keys=ON, synchronous=FULL, and short write transactions with a bounded busy timeout. Native I/O is not performed inside a transaction. CAS head_version prevents lost updates and ABA during archive/unarchive. Recipe/head changes increment library_generation; creating plans and updating receipts do not change content generation.

## 6. Dump and import

[dump.schema.json](dump.schema.json), [import-plan.schema.json](import-plan.schema.json).

A dump contains format/version, exported_at, recipes, and heads with archived. An empty library exports empty recipes/heads. For selection=all, include_archived defaults to true: a full export includes all revisions and heads. With include_archived=false, all revisions of archived IDs are excluded. For an explicit selection list, include_archived is not accepted: selected exact revisions are exported regardless of archive state, which is preserved; a selected export includes selected revisions and the transitive closure of parent. For a selected export, the head of each exported ID is the highest included revision of that ID; archived is copied from the library. Unresolved derived_from is retained as provenance with a warning. Names are not IDs.

Export excludes the SQLite database, descriptor/auth, credentials, apply plans, operations, runtime window/object IDs, and print permissions. User descriptions and other permitted text are transferred as-is: the schema is not a scanner for secrets the author may have written in a note. Importing a file alone neither applies settings to the model nor proves recipe quality.

V1 limits: UTF-8 JSON ≤8 MiB, depth≤24, ≤4096 revisions, ≤1024 heads; individual payload ≤64 KiB, values≤256, value string≤4096 characters. Duplicate JSON keys, unknown fields, incorrect formats, invalid hashes/references, and invalid structure are rejected before writing. RecipeRef/ID+revision/head entries must be unique. The set of head IDs must exactly match the set of recipe IDs. All sizes are additionally checked against actual bytes, not just JSON Schema.

`dump_sha256` is the JCS/SHA-256 of the entire Dump object, including exported_at and array order; source JSON whitespace has no effect. Export sorts recipes by recipe_id/revision and heads by recipe_id.

`import.prepare` receives `parameters.dump` as **original JSON text**, allowing duplicate keys to be detected before parsing by the MCP client. The file format itself remains a Dump JSON object. The server validates and stores a bounded copy of the dump, base_library_generation, and an import plan with library_id and a 10-minute TTL. import_plan_sha256 is the JCS/SHA-256 of all public ImportPlan fields except import_plan_sha256 itself; it includes dump_sha256 and base_library_generation. Identical records are marked duplicates. Any identity_conflict blocks the entire plan without inserts. Existing heads are preserved by default; new IDs receive the exported head/archived. Explicit advance_heads can only advance an existing head through a complete verified parent chain; it neither rolls back nor unarchives. An archived recipe requires separate explicit unarchive.

`import.commit` checks import_plan_sha256, TTL, generation, and warnings, then inserts all records, updates selected heads, and stores the request_id receipt in one transaction. A changed library produces library_conflict; import must be prepared again. Import itself does not run URLs, scripts, plugins, or background updates.

## 7. Updates and migrations

Three versions are distinguished: SQLite `user_version`, portable `format_version`, and the observed native contract. Revision is a fourth, independent recipe-content number.

- **Storage:** a SQLite version migration supported by the code runs with a consistent backup through the SQLite backup API, in exclusive maintenance mode and a transaction. An unsupported newer version opens with an error and no writes. No dispatching/observing operations may be active during migration; unknown receipts are preserved. Every client checks the storage version before a mutating phase and does not continue writing with old code after migration.
- **Portable format:** v1 accepts only format_version=1. An unknown future version is rejected. When a supported older format exists, a converter first preserves the original and creates a verifiable copy; it does not redefine an existing ID/revision/hash with different content.
- **Orca settings:** signature changes produce migration_required. A known verified rule may prepare proposed new values and an explanation; v1 does not ship such a rule set and does not guess renames. An acknowledged adaptation is stored through revise/fork with a new revision/ID, parent/derived_from, and a newly captured contract. Replacing the fingerprint inside an old payload is forbidden. A new revision with the same values and a new contract is allowed after explicit review of changed definitions; this does not automatically establish physical compatibility.
- **Author update:** a new immutable revision is imported. Reviewing differences and explicitly advancing the head are separate from API adaptation. Physical test evidence does not automatically transfer to changed parameters.

A file dump exchanges recipes; it does not restore the operation log. It must not replace the working SQLite database and lose consumed intents. Returning to a database backup when a dispatch may be unfinished is maintenance that preserves/reconciles the log, not an automatic repeat application.

## 8. MCP tools

These tools are implemented in MCP 0.4.0. Arguments and action allowlists: requests.schema.json. Successful responses have `{contract_version:1,ok:true,data:{...}}`; errors have `{contract_version:1,ok:false,error:{kind,message,details}}`, with MCP isError=true. A successful apply/operation.get call means a receipt was obtained: status=outcome_unknown still has isError=true and explicit recovery without resend.

| Tool | Actions | Main data result |
|---|---|---|
| `orca_recipes` | `describe` | library_id, library_generation, storage/format/contract versions, limits, available actions, and request_schema; optional parameters.tool/action filters |
| | `list` | items with exact ref/head_version/archived, offset/limit/next_offset |
| | `get` | RecipeRecord, current head/ref/version/archived, provenance note |
| | `check` | checks and conflicts lists, overall status; no Orca changes |
| | `export` | Dump; if limits are exceeded, an error suggesting a narrower selection |
| | `operation.get` | Operation; observation only, no patch dispatch |
| `orca_recipe_library` | `create` | First revision's RecipeRecord + head_version=1 |
| | `revise`, `fork` | New RecipeRecord + head_version; revise uses CAS, fork creates a new ID |
| | `archive` | head ref/head_version/archived after CAS |
| | `import.prepare` | ImportPlan, without changing the recipe library |
| | `import.commit` | inserted/duplicates/head_changes and new library_generation |
| `orca_recipe_apply` | `plan` | Plan or composition/compatibility error; does not change Orca |
| | `apply` | Operation; only one frozen settings.patch |

Capture_binding for create/revise/fork obtains current key definitions for draft.scope and checks read consistency. It does not change the model or count as physical validation. The stored recipe contains no target/owner/state_token from capture. The evidence field of a created/revised revision must be explicitly supplied when present: without it, the server stores an empty list rather than inheriting confirmation of an earlier print.

`orca_recipes` declares readOnlyHint=true, destructiveHint=false. The other two groups have readOnlyHint=false; the library group has destructiveHint=false (revisions are retained and archive is reversible), and apply has destructiveHint=true. All have openWorldHint=false: this contract does not send data to printers or external catalogs. Import/export v1 accept/return JSON in the tool payload, without arbitrary filesystem paths; the client reads/writes local dumps within its permissions.

Creating a receipt/plan requires a durable write even when the model is unchanged, so plan/import.prepare belong to the mutating group. Concrete Plan/Operation/ImportPlan results have separate schemas; the common response.schema.json specifies only the envelope. The remaining results are described in this table and implemented in the schemas and service, subject to the core invariants.

## 9. Storage

One SQLite library for the current user is shared by all MCP clients. The default macOS path is `~/Library/Application Support/orca-slicer-mcp/recipes.sqlite3`; Linux: `$XDG_DATA_HOME/orca-slicer-mcp/recipes.sqlite3` or `~/.local/share/...`; Windows: `%LOCALAPPDATA%/orca-slicer-mcp/recipes.sqlite3`. Explicit startup `--recipes-db` selects another absolute database path, including for isolated tests. Tool arguments do not change the database path.

Logical entities: library_id/generation metadata; immutable revisions; mutable heads with head_version/archived; durable requests/receipts; apply/import plans; apply operations. Payloads are stored canonically. The database, WAL/SHM, and backups are private to the current user. An access error does not permit silently creating another empty database.

Recipe references contain no local project paths and require no chat binding. Changes to window UUID, document epoch, or scope owners invalidate old bindings but do not delete the library.

## 10. Examples and implementation acceptance criteria

[examples/recipe.json](examples/recipe.json), [examples/library-dump.json](examples/library-dump.json), [examples/plan-request.json](examples/plan-request.json), [examples/import-request.json](examples/import-request.json), [examples/plan.json](examples/plan.json), [examples/apply-request.json](examples/apply-request.json).

Examples contain **synthetic native signatures**, are marked as fixtures, and are not intended for application. These are not tested recipes for detachable supports/bridges/ironing. Real recipes require capturing the current contract and separate print tests.

Verifiable invariants of the first implementation:

1. Export → import → export preserves payload/ref/hash; foreign IDs and identical names are not merged.
2. The same id/revision with a different hash rejects the entire import; an error/crash does not leave half a library.
3. Two clients do not lose a concurrent revision: CAS conflicts, and archiving does not create ABA.
4. A key/type/range/enum/scope change blocks an old recipe; values are retained until explicit adaptation.
5. Composition of identical values works; conflicting values require a choice; an equal inherited value does not hide override creation.
6. A plan for another UUID/plate/object/state/schema or beyond TTL is not applied. A head change does not change a pinned payload.
7. A single intent is recorded before native I/O. Failure before/after admission and lost replies do not cause a second automatic patch.
8. All pages and both native limits of 256 keys/64 KiB are checked. Success is not declared from one receipt page.
9. Storage migration preserves revisions/hashes/unknown intents and a rollback backup; an old client does not write after a schema change.
10. Import does not execute description/URL contents, treat authorship as trust, or transfer print permission.

JSON-schema validation confirms shape, examples, hashes, and negative cases. Separate tests cover SQLite, concurrency, MCP stdio, and lost replies; an isolated GUI check confirms application on native API revision 3. This does not confirm physical print quality.

The first implementation supports only storage initialization 0 → 1 and explicit backup through the SQLite backup API; future storage or settings-semantic migrations are not yet available. An unsupported version is rejected before writing. The current native API does not expose exact material/model applicability: declared constraints of these types produce compatibility_unknown and block application. Nozzle diameter is read from the machine preset; layer height from the effective process.

Validate the contract from the MCP directory:

```sh
.venv/bin/python docs/contracts/recipes-v1/verify_contract.py
```

This requires jsonschema and referencing from the existing MCP environment. Validation does not connect to Orca or apply anything.
