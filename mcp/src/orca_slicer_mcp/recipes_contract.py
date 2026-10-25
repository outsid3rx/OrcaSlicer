"""Portable recipe v1 validation and canonical identities; no filesystem/native I/O."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
from decimal import Decimal, InvalidOperation
from importlib.resources import files
import hashlib
import json
import math
from typing import Any

import rfc8785
from jsonschema import Draft202012Validator, FormatChecker, ValidationError

DUMP_LIMIT = 8 * 1024 * 1024
RECIPE_LIMIT = 65536
NATIVE_LIMIT = 65536
KEY_LIMIT = 256


class RecipeError(Exception):
    def __init__(self, kind: str, message: str, details: dict | None = None):
        super().__init__(message)
        self.kind, self.message, self.details = kind, message, details or {}

    def as_dict(self) -> dict:
        return {"kind": self.kind, "message": self.message, "details": self.details}


def _json_values(value: Any, depth: int = 0, *, max_depth: int = 32, integers_only: bool = False) -> None:
    if depth > max_depth:
        raise RecipeError("invalid_params", "JSON nesting exceeds the supported limit")
    if isinstance(value, str):
        try:
            value.encode("utf-8", errors="strict")
        except UnicodeError:
            raise RecipeError("invalid_params", "Unpaired Unicode surrogate is not valid UTF-8") from None
    elif value is None or isinstance(value, bool):
        return
    elif type(value) is int:
        if abs(value) > 9007199254740991:
            raise RecipeError("invalid_params", "JSON integer exceeds the interoperable JCS range")
    elif type(value) is float:
        if integers_only or not math.isfinite(value):
            raise RecipeError("invalid_params", "Nonfinite numbers and recipe floating-point values are forbidden")
    elif isinstance(value, list):
        for item in value:
            _json_values(item, depth + 1, max_depth=max_depth, integers_only=integers_only)
    elif isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise RecipeError("invalid_params", "JSON object keys must be strings")
            _json_values(key, depth + 1, max_depth=max_depth, integers_only=integers_only)
            _json_values(item, depth + 1, max_depth=max_depth, integers_only=integers_only)
    else:
        raise RecipeError("invalid_params", "Only JSON values are accepted")


def canonical(value: Any) -> bytes:
    _json_values(value)
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, TypeError, ValueError, OverflowError):
        raise RecipeError("invalid_params", "Cannot encode this value as RFC 8785 canonical JSON") from None


def digest(value: Any) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


_SCHEMA = json.loads(files(__package__).joinpath("recipes.schema.json").read_text(encoding="utf-8"))
_VALIDATORS: dict[str, Draft202012Validator] = {}


def validate(definition: str, value: Any) -> None:
    _json_values(value)
    if definition not in _SCHEMA["$defs"]:
        raise RecipeError("invalid_params", "Unknown recipe contract definition")
    if definition not in _VALIDATORS:
        _VALIDATORS[definition] = Draft202012Validator(
            {**_SCHEMA, "$ref": f"#/$defs/{definition}"}, format_checker=FormatChecker())
    try:
        _VALIDATORS[definition].validate(value)
    except ValidationError as error:
        # Do not echo a schema error's input repr (it can contain an entire dump).
        path = "/".join(str(item) for item in error.absolute_path)
        raise RecipeError("invalid_params", "Value does not match recipe contract v1",
                          {"definition": definition, "path": path[:512], "rule": str(error.validator)}) from None


def validate_request(tool: str, action: str | dict, parameters: dict | None = None,
                     request_id: str | None = None) -> None:
    if isinstance(action, dict):
        if parameters is not None or request_id is not None:
            raise RecipeError("invalid_params", "Supply tool arguments in one form")
        arguments = action
    else:
        arguments = {"action": action, "parameters": parameters}
        if request_id is not None:
            arguments["request_id"] = request_id
    validate("Request", {"tool": tool, "arguments": arguments})
    params = arguments.get("parameters", {})
    binding = params.get("capture_binding") or params.get("binding")
    if binding:
        if binding["scope"] == "height_range" and binding["height_range"][0] >= binding["height_range"][1]:
            raise RecipeError("invalid_params", "Height range requires lower < upper")
        if "draft" in params and params["draft"]["scope"] != binding["scope"]:
            raise RecipeError("invalid_params", "Recipe scope and capture binding must match")
    if "draft" in params:
        _compatibility(params["draft"].get("compatibility", {}))
    references = params.get("recipes", [])
    if len({canonical(item) for item in references}) != len(references):
        raise RecipeError("invalid_params", "Duplicate recipe references are not useful in one operation")


def _compatibility(compatibility: dict) -> None:
    values = list(compatibility.get("nozzle_diameters_mm", []))
    values += list(compatibility.get("layer_height_mm", {}).values())
    try:
        if any(Decimal(item) <= 0 for item in values):
            raise RecipeError("invalid_params", "Nozzle and layer dimensions must be positive")
        if "layer_height_mm" in compatibility:
            bounds = compatibility["layer_height_mm"]
            if Decimal(bounds["min"]) > Decimal(bounds["max"]):
                raise RecipeError("invalid_params", "Layer height minimum exceeds maximum")
    except (InvalidOperation, TypeError):
        raise RecipeError("invalid_params", "Invalid decimal compatibility condition") from None
    for name in ("printer_model_ids", "filament_types", "nozzle_diameters_mm"):
        entries = compatibility.get(name, [])
        if len(set(entries)) != len(entries):
            raise RecipeError("invalid_params", "Compatibility entries must be unique", {"field": name})


def revision_ref(record: dict) -> dict:
    return {"recipe_id": record["payload"]["recipe_id"], "revision": record["payload"]["revision"], "sha256": record["sha256"]}


def validate_record(record: dict) -> None:
    payload = record.get("payload", {}) if isinstance(record, dict) else {}
    if payload.get("format") == "orca.recipe" and payload.get("format_version") != 1:
        raise RecipeError("unsupported_format", "Only recipe format version 1 is supported")
    validate("RecipeRecord", record)
    _json_values(payload, integers_only=True, max_depth=24)
    if len(canonical(payload)) > RECIPE_LIMIT:
        raise RecipeError("invalid_dump", "Recipe payload exceeds 64 KiB")
    if digest(payload) != record["sha256"]:
        raise RecipeError("digest_mismatch", "Recipe payload hash does not match its identity")
    if set(payload["values"]) != set(payload["native_contract"]["definitions"]):
        raise RecipeError("invalid_dump", "Every recipe setting needs exactly one native definition signature")
    parent = payload["parent"]
    if payload["revision"] == 1:
        if parent is not None:
            raise RecipeError("invalid_dump", "First revision cannot have a parent")
    elif (parent is None or parent["recipe_id"] != payload["recipe_id"] or parent["revision"] != payload["revision"] - 1):
        raise RecipeError("invalid_dump", "Revision parent must be the preceding revision of the same recipe")
    if payload["derived_from"] and payload["derived_from"]["recipe_id"] == payload["recipe_id"]:
        raise RecipeError("invalid_dump", "A fork must have a different recipe ID")
    _compatibility(payload["compatibility"])


def validate_dump(value: dict) -> None:
    if isinstance(value, dict) and value.get("format") == "orca.recipe-library" and value.get("format_version") != 1:
        raise RecipeError("unsupported_format", "Only library dump version 1 is supported")
    validate("Dump", value)
    _json_values(value, max_depth=24, integers_only=True)
    if len(canonical(value)) > DUMP_LIMIT:
        raise RecipeError("invalid_dump", "Library dump exceeds 8 MiB")
    records = {}
    for record in value["recipes"]:
        validate_record(record)
        payload = record["payload"]
        identity = (payload["recipe_id"], payload["revision"])
        if identity in records:
            raise RecipeError("identity_conflict", "Dump repeats a recipe revision identity")
        records[identity] = record
    def resolve(reference):
        record = records.get((reference["recipe_id"], reference["revision"]))
        if record is None or record["sha256"] != reference["sha256"]:
            raise RecipeError("invalid_dump", "Dump does not contain the exact referenced revision")
        return record
    for record in records.values():
        parent = record["payload"]["parent"]
        if parent:
            resolve(parent)
    heads = set()
    for head in value["heads"]:
        resolve(head["ref"])
        identifier = head["ref"]["recipe_id"]
        if identifier in heads:
            raise RecipeError("invalid_dump", "Dump repeats a recipe head")
        heads.add(identifier)
    if heads != {identifier for identifier, _ in records}:
        raise RecipeError("invalid_dump", "Dump needs exactly one head per included recipe ID")
    # Check only resolvable provenance links; unresolved foreign origins remain
    # metadata. Iterative traversal bounds the work without Python recursion.
    states = {}
    for identity in records:
        stack = [(identity, False)]
        while stack:
            current, leaving = stack.pop()
            if leaving:
                states[current] = 2
                continue
            if states.get(current) == 1:
                raise RecipeError("invalid_dump", "Cyclic recipe provenance")
            if states.get(current) == 2:
                continue
            states[current] = 1
            stack.append((current, True))
            p = records[current]["payload"]
            for edge in (p["parent"], p["derived_from"]):
                if edge:
                    child = (edge["recipe_id"], edge["revision"])
                    if child in records:
                        resolve(edge)
                        stack.append((child, False))


def parse_dump(raw: str | bytes) -> dict:
    try:
        encoded = raw.encode("utf-8", errors="strict") if isinstance(raw, str) else raw
        if not isinstance(encoded, bytes) or len(encoded) > DUMP_LIMIT:
            raise RecipeError("invalid_dump", "Import requires UTF-8 JSON text up to 8 MiB")
        def pairs(items):
            result = {}
            for key, value in items:
                if key in result:
                    raise RecipeError("invalid_dump", "Duplicate JSON key in imported dump")
                result[key] = value
            return result
        def constant(_):
            raise RecipeError("invalid_dump", "Nonfinite JSON number in imported dump")
        value = json.loads(encoded.decode("utf-8", errors="strict"), object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeError, ValueError, TypeError, RecursionError):
        raise RecipeError("invalid_dump", "Import requires bounded, valid UTF-8 JSON") from None
    validate_dump(value)
    return value


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")


def expires_at(seconds: int = 600) -> str:
    return (datetime.now(timezone.utc) + timedelta(seconds=seconds)).isoformat(timespec="microseconds").replace("+00:00", "Z")


def expired(value: str) -> bool:
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")) <= datetime.now(timezone.utc)
    except (ValueError, TypeError):
        raise RecipeError("invalid_params", "Invalid UTC expiry") from None


def warning(code: str, message: str, acknowledgement_required: bool = True) -> dict:
    contents = {"code": code, "message": message, "acknowledgement_required": acknowledgement_required}
    return {"id": digest(contents), **contents}


def normalize_definition(scope: str, key: str, definition: dict) -> dict:
    return {"scope": scope, "key": key, "type": definition["type"], "min": definition.get("min"),
            "max": definition.get("max"), "enum_values": definition.get("enum_values", []), "units": definition.get("units")}


def make_native_contract(capabilities: dict, scope: str, defs: dict[str, dict]) -> dict:
    if capabilities.get("protocol_version") != 1 or type(capabilities.get("api_revision")) is not int:
        raise RecipeError("native_contract_changed", "Native API must report protocol 1 and its API revision")
    return {"protocol_version": 1, "api_revision": capabilities["api_revision"], "app_version": capabilities["app_version"],
            "definitions": {key: {"native_type": definition["type"], "sha256": digest(normalize_definition(scope, key, definition))}
                            for key, definition in sorted(defs.items())}}


def request_schema(tool: str | None = None, action: str | None = None) -> dict:
    """Return selected action schemas and only their referenced definitions."""
    choices = [item for item in _SCHEMA['$defs']['Request']['oneOf']
               if (tool is None or item['properties']['tool']['const'] == tool)
               and (action is None or item['properties']['arguments']['properties']['action']['const'] == action)]
    if not choices:
        raise RecipeError('invalid_params', 'Unknown recipe tool/action filter')
    definitions = {}
    def visit(value):
        if isinstance(value, dict):
            ref = value.get('$ref', '')
            if ref.startswith('#/$defs/'):
                name = ref.removeprefix('#/$defs/')
                if name not in definitions:
                    definitions[name] = _SCHEMA['$defs'][name]
                    visit(definitions[name])
            for item in value.values():
                visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)
    visit(choices)
    return {'$schema': _SCHEMA['$schema'], 'oneOf': choices, '$defs': definitions}


def public_error(error) -> dict:
    """Keep transport errors inside the stable recipe error vocabulary."""
    kind = error.kind
    aliases = {key: 'invalid_params' for key in (
        'invalid_request', 'invalid_resolution', 'invalid_value', 'scope_mismatch',
        'warning_acknowledgement_required')}
    aliases.update(request_too_large='native_limit', archived_recipe='incompatible_recipe',
                   incompatible='incompatible_recipe', unknown='compatibility_unknown',
                   unauthorized='stale_target', busy='native_busy')
    kind = aliases.get(kind, kind)
    if kind not in _SCHEMA['$defs']['Error']['properties']['kind']['enum']:
        kind = 'native_unavailable'
    return {'kind': kind, 'message': error.message[:4096] or 'Recipe request failed',
            'details': getattr(error, 'details', {}) or {}}
