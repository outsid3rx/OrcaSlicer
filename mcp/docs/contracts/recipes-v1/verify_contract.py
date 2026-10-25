"""Verify contract schemas, illustrative fixtures and selected format invariants.

This is specification tooling, not the recipe library or its migration/apply engine.
Run with the MCP Python environment (jsonschema/referencing are already installed).
"""
from pathlib import Path
import copy
import hashlib
import json
from jsonschema import Draft202012Validator, FormatChecker, ValidationError
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parent
LIMIT = 8 * 1024 * 1024
checks = 0


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate_json_key")
        result[key] = value
    return result


def load_bytes(raw):
    if len(raw) > LIMIT:
        raise ValueError("dump_limit")
    def reject_constant(_):
        raise ValueError("nonfinite_json_number")
    value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_keys,
                       parse_constant=reject_constant)
    def visit(item, depth=0):
        if depth > 24:
            raise ValueError("depth_limit")
        if isinstance(item, str):
            item.encode("utf-8", errors="strict")
        elif isinstance(item, dict):
            for k, v in item.items():
                visit(k, depth + 1)
                visit(v, depth + 1)
        elif isinstance(item, list):
            for v in item:
                visit(v, depth + 1)
    visit(value)
    return value


schemas = {}
registry = Registry()
for path in ROOT.glob("*.schema.json"):
    schema = load_bytes(path.read_bytes())
    Draft202012Validator.check_schema(schema)
    schemas[path.stem] = schema
    registry = registry.with_resource(schema["$id"], Resource.from_contents(schema))


def validate(name, value):
    Draft202012Validator(schemas[name + ".schema"], registry=registry,
                         format_checker=FormatChecker()).validate(value)


def canonical_fixture(value):
    # Fixtures deliberately use only the JCS subset of ASCII object keys and
    # safe integers. This helper is NOT a general RFC 8785 number serializer.
    def visit(v):
        if isinstance(v, dict):
            for k, item in v.items():
                if not k.isascii():
                    raise ValueError("fixture_jcs_subset_only")
                visit(item)
        elif isinstance(v, list):
            for item in v:
                visit(item)
        elif type(v) is int and abs(v) > 9007199254740991:
            raise ValueError("integer_outside_jcs_interoperable_range")
        elif isinstance(v, float):
            raise ValueError("fixture_jcs_subset_only")
        elif isinstance(v, str):
            v.encode("utf-8", errors="strict")
    visit(value)
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical_fixture(value)).hexdigest()


def verify_plan_digest(plan):
    payload = {key: value for key, value in plan.items() if key != "plan_sha256"}
    if digest(payload) != plan["plan_sha256"]:
        raise ValueError("plan_digest_mismatch")


def revision_ref(record):
    p = record["payload"]
    return {"recipe_id": p["recipe_id"], "revision": p["revision"], "sha256": record["sha256"]}


def validate_record(record):
    validate("recipe", record)
    payload = record["payload"]
    if len(canonical_fixture(payload)) > 65536:
        raise ValueError("recipe_limit")
    if digest(payload) != record["sha256"]:
        raise ValueError("digest_mismatch")
    if set(payload["values"]) != set(payload["native_contract"]["definitions"]):
        raise ValueError("definition_key_mismatch")
    parent = payload["parent"]
    if payload["revision"] == 1:
        if parent is not None:
            raise ValueError("first_revision_has_parent")
    elif (parent is None or parent["recipe_id"] != payload["recipe_id"] or
          parent["revision"] != payload["revision"] - 1):
        raise ValueError("invalid_parent")
    origin = payload["derived_from"]
    if origin and origin["recipe_id"] == payload["recipe_id"]:
        raise ValueError("self_fork")


def validate_dump(value):
    validate("dump", value)
    records = {}
    for record in value["recipes"]:
        validate_record(record)
        p = record["payload"]
        identity = (p["recipe_id"], p["revision"])
        if identity in records:
            raise ValueError("duplicate_revision_identity")
        records[identity] = record
    def resolve(reference):
        found = records.get((reference["recipe_id"], reference["revision"]))
        if found is None or found["sha256"] != reference["sha256"]:
            raise ValueError("unresolved_or_conflicting_reference")
    for record in records.values():
        if record["payload"]["parent"] is not None:
            resolve(record["payload"]["parent"])
    heads = set()
    for head in value["heads"]:
        resolve(head["ref"])
        identifier = head["ref"]["recipe_id"]
        if identifier in heads:
            raise ValueError("duplicate_head")
        heads.add(identifier)
    if heads != {identifier for identifier, _ in records}:
        raise ValueError("head_set_mismatch")


def check(label, action, reject=False):
    global checks
    try:
        action()
    except (ValidationError, ValueError, UnicodeError):
        if not reject:
            raise
    else:
        if reject:
            raise AssertionError("Invalid case accepted: " + label)
    checks += 1
    print("PASS", label)


def mutated(value, edit):
    result = copy.deepcopy(value)
    edit(result)
    return result


def resign(record):
    record["sha256"] = digest(record["payload"])
    return record


def main():
    recipe = load_bytes((ROOT / "examples/recipe.json").read_bytes())
    dump = load_bytes((ROOT / "examples/library-dump.json").read_bytes())
    plan = load_bytes((ROOT / "examples/plan-request.json").read_bytes())
    imported = load_bytes((ROOT / "examples/import-request.json").read_bytes())
    concrete_plan = load_bytes((ROOT / "examples/plan.json").read_bytes())
    apply_request = load_bytes((ROOT / "examples/apply-request.json").read_bytes())
    check("concrete plan schema", lambda: validate("plan", concrete_plan))
    check("plan hash domain", lambda: verify_plan_digest(concrete_plan))
    check("apply request fixture", lambda: validate("requests", apply_request))
    check("all schemas valid", lambda: None)
    check("recipe fixture", lambda: validate_record(recipe))
    check("dump fixture and full parent closure", lambda: validate_dump(dump))
    check("plan request fixture", lambda: validate("requests", plan))
    check("import request fixture", lambda: validate("requests", imported))
    check("empty library dump", lambda: validate_dump({**dump, "recipes": [], "heads": []}))
    check("whitespace-independent fixture hash", lambda: validate_record(load_bytes(json.dumps(recipe, ensure_ascii=False, indent=4).encode())))
    bad = mutated(recipe, lambda x: x["payload"]["values"].update(enable_support="0"))
    check("changed value invalidates hash", lambda: validate_record(bad), reject=True)
    bad = mutated(recipe, lambda x: x["payload"].update(auth_token="never-export"))
    check("unknown secret field rejected", lambda: validate_record(bad), reject=True)
    bad = mutated(recipe, lambda x: x["payload"]["values"].update(enable_support=True))
    check("native values must be strings", lambda: validate_record(bad), reject=True)
    bad = mutated(recipe, lambda x: x["payload"]["native_contract"]["definitions"].pop("enable_support"))
    resign(bad)
    check("all used keys need contract signatures", lambda: validate_record(bad), reject=True)
    bad = mutated(recipe, lambda x: x["payload"].update(format_version=2))
    resign(bad)
    check("future format rejected", lambda: validate_record(bad), reject=True)
    bad = mutated(recipe, lambda x: x["payload"].update(revision=2))
    resign(bad)
    check("second revision needs parent", lambda: validate_record(bad), reject=True)
    second = copy.deepcopy(recipe)
    second["payload"].update(revision=2, parent=revision_ref(recipe), name="Updated illustrative recipe")
    resign(second)
    good = copy.deepcopy(dump)
    good["recipes"].append(second)
    good["heads"][0]["ref"] = revision_ref(second)
    check("new immutable revision preserves original", lambda: validate_dump(good))
    bad = copy.deepcopy(good)
    bad["recipes"] = [r for r in bad["recipes"] if r["payload"]["recipe_id"] != recipe["payload"]["recipe_id"] or r["payload"]["revision"] != 1]
    check("missing parent rejected", lambda: validate_dump(bad), reject=True)
    bad = copy.deepcopy(dump)
    conflict = copy.deepcopy(recipe)
    conflict["payload"]["name"] = "Conflicting content"
    resign(conflict)
    bad["recipes"].append(conflict)
    check("duplicate identity rejected", lambda: validate_dump(bad), reject=True)
    bad = copy.deepcopy(dump)
    bad["heads"][0]["ref"]["sha256"] = "0" * 64
    check("head hash must resolve", lambda: validate_dump(bad), reject=True)
    bad = copy.deepcopy(dump)
    bad["heads"].append(copy.deepcopy(bad["heads"][0]))
    check("duplicate head rejected", lambda: validate_dump(bad), reject=True)
    bad = mutated(plan, lambda x: x["arguments"]["parameters"]["binding"].update(volume_id="wrong-scope"))
    check("owner selectors constrained by scope", lambda: validate("requests", bad), reject=True)
    bad = mutated(plan, lambda x: x["arguments"]["parameters"]["binding"]["target"].update(window_id="not-a-uuid"))
    check("full window identity required", lambda: validate("requests", bad), reject=True)
    bad = mutated(plan, lambda x: x["arguments"]["parameters"].update(force=True))
    check("no force bypass", lambda: validate("requests", bad), reject=True)
    bad = mutated(plan, lambda x: x["arguments"].pop("request_id"))
    check("mutating plan needs request ID", lambda: validate("requests", bad), reject=True)
    check("duplicate JSON keys rejected", lambda: load_bytes(b'{"a":1,"a":2}'), reject=True)
    check("nonfinite JSON numbers rejected", lambda: load_bytes(b'{"a":NaN}'), reject=True)
    check("unpaired unicode rejected", lambda: load_bytes(b'{"a":"\\ud800"}'), reject=True)
    check("decompression-independent dump byte limit", lambda: load_bytes(b' ' * (LIMIT + 1)), reject=True)
    check("depth limit", lambda: load_bytes(('[' * 26 + '0' + ']' * 26).encode()), reject=True)
    selected_export = {"tool": "orca_recipes", "arguments": {"action": "export", "parameters": {"selection": [revision_ref(recipe)]}}}
    check("selected export has no archive filter", lambda: validate("requests", selected_export))
    selected_export["arguments"]["parameters"]["include_archived"] = False
    check("ambiguous selected archive filter rejected", lambda: validate("requests", selected_export), reject=True)
    op = {"operation_id": "50000000-0000-4000-8000-000000000001", "plan_id": "50000000-0000-4000-8000-000000000002", "plan_sha256": "a" * 64,
          "status": "outcome_unknown", "native_request_id": "50000000-0000-4000-8000-000000000003", "native_job_id": None, "native_state": None,
          "verification": "unavailable", "error": {"kind": "outcome_unknown", "message": "Admission reply was lost"},
          "recovery": {"action": "inspect_scene_do_not_resend", "message": "Inspect original state and intent; do not resend"}}
    check("unknown receipt carries recovery", lambda: validate("operation", op))
    bad = mutated(op, lambda x: x["recovery"].update(action="none"))
    check("unknown receipt cannot omit recovery", lambda: validate("operation", bad), reject=True)
    assert recipe == load_bytes((ROOT / "examples/recipe.json").read_bytes())
    print(f"PASS {checks} contract checks; no runtime library, migration or native apply executed")


if __name__ == "__main__":
    main()
