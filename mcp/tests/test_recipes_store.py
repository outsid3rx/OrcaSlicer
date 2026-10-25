"""Storage invariants across clients, processes, restarts and interrupted imports."""
import copy
import json
import multiprocessing
import os
from pathlib import Path
import sqlite3
import tempfile
import unittest
import uuid
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch

from orca_slicer_mcp.recipes_contract import RecipeError, digest, revision_ref
from orca_slicer_mcp.recipes_store import RecipeStore

FIXTURES = Path(__file__).resolve().parents[1] / "docs/contracts/recipes-v1/examples"


def uid():
    return str(uuid.uuid4())


def fixture(name):
    return json.loads((FIXTURES / name).read_text())


def create_params():
    record = fixture("recipe.json")
    payload = record["payload"]
    draft = {key: copy.deepcopy(payload[key]) for key in (
        "name", "description", "scope", "values", "compatibility", "tags", "evidence")}
    return {"draft": draft, "capture_binding": fixture("plan-request.json")["arguments"]["parameters"]["binding"]}, payload["native_contract"]


def make_plan(store, reference):
    record = store.get(reference)["record"]
    plan = fixture("plan.json")
    plan.update(plan_id=uid(), library_id=store.library_id, recipes=[reference],
                native_request_id=uid(), values=record["payload"]["values"])
    from orca_slicer_mcp.recipes_contract import utc_now, expires_at
    plan.update(created_at=utc_now(), expires_at=expires_at())
    plan["warnings"] = []
    plan["diff"] = [{"key": key, "before_effective": "0", "before_own": None,
        "after": value, "changes_ownership": True, "sources": [reference]}
        for key, value in plan["values"].items()]
    native = {"jsonrpc": "2.0", "id": plan["native_request_id"], "method": "settings.patch",
              "params": {**plan["binding"], "values": plan["values"]}}
    plan["native_request_bytes"] = len(json.dumps(native, ensure_ascii=False, separators=(",", ":")).encode())
    plan["plan_sha256"] = digest({k: v for k, v in plan.items() if k != "plan_sha256"})
    args = {"recipes": [reference], "binding": plan["binding"]}
    return store.save_plan(uid(), args, plan, native)


def initial_operation(plan):
    return {"operation_id": uid(), "plan_id": plan["plan_id"], "plan_sha256": plan["plan_sha256"],
        "status": "dispatching", "native_request_id": plan["native_request_id"], "native_job_id": None,
        "native_state": None, "verification": "pending", "error": None,
        "recovery": {"action": "inspect_scene_do_not_resend", "message": "Dispatch intent is consumed"}}


def apply_args(plan):
    return {"plan_id": plan["plan_id"], "plan_sha256": plan["plan_sha256"], "accepted_warning_ids": []}


def process_claim(path, plan, gate, replies):
    store = RecipeStore(path)
    try:
        gate.wait(timeout=10)
        op, new = store.claim_apply(uid(), apply_args(plan), initial_operation(plan))
        replies.put((op["operation_id"], new))
        # Keep ownership alive until both processes have completed the claim.
        gate.wait(timeout=10)
    finally:
        store.close()


class RecipeStoreTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "recipes.sqlite3"
        self.store = RecipeStore(self.path)
        self.addCleanup(self.store.close)

    def create(self, store=None):
        params, native = create_params()
        return (store or self.store).create(uid(), params, native)

    def test_empty_library_has_valid_export_and_private_sidecars(self):
        self.assertEqual(self.store.export()["recipes"], [])
        self.assertEqual(self.store.describe()["library_generation"], 0)
        if os.name == "posix":
            for path in [self.path, Path(str(self.path) + "-wal"), Path(str(self.path) + "-shm")]:
                if path.exists():
                    self.assertEqual(path.stat().st_mode & 0o077, 0)

    def test_revision_is_immutable_and_replay_preserves_original_result(self):
        params, native = create_params()
        request = uid()
        created = self.store.create(request, params, native)
        self.assertEqual(self.store.create(request, params, native), created)
        source = created["record"]
        revised = copy.deepcopy(params)
        revised.update(recipe_id=source["payload"]["recipe_id"], expected_head_version=1)
        revised["draft"]["values"]["enable_support"] = "0"
        result = self.store.revise(uid(), revised, native)
        self.assertEqual(result["head_version"], 2)
        self.assertEqual(result["record"]["payload"]["parent"], revision_ref(source))
        self.assertEqual(self.store.get(revision_ref(source))["record"], source)
        self.assertEqual(self.store.create(request, params, native), created)
        bad = copy.deepcopy(params)
        bad["draft"]["name"] = "different"
        with self.assertRaises(RecipeError) as error:
            self.store.create(request, bad, native)
        self.assertEqual(error.exception.kind, "request_conflict")

    def test_fork_uses_new_identity_and_explicit_source(self):
        created = self.create()
        params, native = create_params()
        params["source"] = created["head"]
        result = self.store.fork(uid(), params, native)
        self.assertNotEqual(result["head"]["recipe_id"], created["head"]["recipe_id"])
        self.assertEqual(result["record"]["payload"]["derived_from"], created["head"])
        self.assertEqual(result["record"]["payload"]["revision"], 1)

    def test_parallel_revisions_use_head_compare_and_swap(self):
        created = self.create()
        params, native = create_params()
        params.update(recipe_id=created["head"]["recipe_id"], expected_head_version=1)
        other = RecipeStore(self.path)
        self.addCleanup(other.close)
        def revise(store):
            try:
                return store.revise(uid(), params, native)["head_version"]
            except RecipeError as error:
                return error.kind
        with ThreadPoolExecutor(max_workers=2) as pool:
            values = list(pool.map(revise, [self.store, other]))
        self.assertCountEqual(values, [2, "library_conflict"])

    def test_archive_unarchive_prevents_aba_and_search_handles_russian(self):
        created = self.create()
        identifier = created["head"]["recipe_id"]
        first = self.store.archive(uid(), {"recipe_id": identifier, "expected_head_version": 1, "archived": True})
        self.assertEqual(first["head_version"], 2)
        self.assertEqual(self.store.list_recipes()["items"], [])
        self.store.archive(uid(), {"recipe_id": identifier, "expected_head_version": 2, "archived": False})
        with self.assertRaises(RecipeError):
            self.store.archive(uid(), {"recipe_id": identifier, "expected_head_version": 1, "archived": True})
        self.assertEqual(len(self.store.list_recipes(query="ПОДДЕРЖКИ")["items"]), 1)

    def test_dump_round_trip_and_duplicate_import_are_content_preserving(self):
        self.create()
        exported = self.store.export()
        destination = RecipeStore(Path(self.directory.name) / "second.sqlite3")
        self.addCleanup(destination.close)
        prepare = destination.import_prepare(uid(), {"dump": json.dumps(exported)})
        committed = destination.import_commit(uid(), {"import_plan_id": prepare["import_plan_id"],
            "import_plan_sha256": prepare["import_plan_sha256"], "accepted_warning_ids": []})
        self.assertEqual(len(committed["inserted"]), 1)
        self.assertEqual(destination.export()["recipes"], exported["recipes"])
        repeated = destination.import_prepare(uid(), {"dump": json.dumps(exported)})
        self.assertEqual(repeated["insert_refs"], [])
        self.assertEqual(len(repeated["duplicate_refs"]), 1)
        before = destination.describe()["library_generation"]
        destination.import_commit(uid(), {"import_plan_id": repeated["import_plan_id"],
            "import_plan_sha256": repeated["import_plan_sha256"], "accepted_warning_ids": []})
        self.assertEqual(destination.describe()["library_generation"], before)

    def test_identity_conflict_rejects_entire_import_without_partial_insert(self):
        existing = self.create()
        exported = self.store.export()
        conflict = exported["recipes"][0]
        conflict["payload"]["name"] = "Same identity, altered content"
        conflict["sha256"] = digest(conflict["payload"])
        exported["heads"][0]["ref"] = revision_ref(conflict)
        extra = fixture("library-dump.json")["recipes"][1]
        exported["recipes"].insert(0, extra)
        exported["heads"].append({"ref": revision_ref(extra), "archived": False})
        before = self.store.describe()["library_generation"]
        with self.assertRaises(RecipeError) as error:
            self.store.import_prepare(uid(), {"dump": json.dumps(exported)})
        self.assertEqual(error.exception.kind, "identity_conflict")
        self.assertEqual(self.store.describe()["library_generation"], before)
        self.assertEqual(len(self.store.export()["recipes"]), 1)
        self.assertEqual(self.store.get(existing["head"])["record"], existing["record"])

    def test_import_commit_is_atomic_when_a_write_fails_midway(self):
        incoming = fixture("library-dump.json")
        prepared = self.store.import_prepare(uid(), {"dump": json.dumps(incoming)})
        original = self.store._insert_record
        calls = []
        def interrupted(db, record):
            calls.append(record)
            if len(calls) == 2:
                raise RuntimeError("simulated interruption")
            original(db, record)
        with patch.object(self.store, "_insert_record", side_effect=interrupted):
            with self.assertRaises(RuntimeError):
                self.store.import_commit(uid(), {"import_plan_id": prepared["import_plan_id"],
                    "import_plan_sha256": prepared["import_plan_sha256"], "accepted_warning_ids": []})
        self.assertEqual(self.store.export()["recipes"], [])
        self.assertEqual(self.store.describe()["library_generation"], 0)

    def test_import_generation_change_requires_new_prepare(self):
        prepared = self.store.import_prepare(uid(), {"dump": json.dumps(fixture("library-dump.json"))})
        self.create()
        with self.assertRaises(RecipeError) as error:
            self.store.import_commit(uid(), {"import_plan_id": prepared["import_plan_id"],
                "import_plan_sha256": prepared["import_plan_sha256"], "accepted_warning_ids": []})
        self.assertEqual(error.exception.kind, "library_conflict")

    def test_imported_newer_revision_never_moves_existing_head_implicitly(self):
        created = self.create()
        base = created["record"]
        second = copy.deepcopy(base)
        second["payload"].update(revision=2, parent=created["head"], name="Second")
        second["sha256"] = digest(second["payload"])
        incoming = {"format": "orca.recipe-library", "format_version": 1, "exported_at": base["payload"]["created_at"],
            "recipes": [base, second], "heads": [{"ref": revision_ref(second), "archived": False}]}
        plan = self.store.import_prepare(uid(), {"dump": json.dumps(incoming)})
        self.store.import_commit(uid(), {"import_plan_id": plan["import_plan_id"], "import_plan_sha256": plan["import_plan_sha256"], "accepted_warning_ids": []})
        self.assertEqual(self.store.get_head(created["head"]["recipe_id"])["ref"], created["head"])
        params, native = create_params()
        params.update(recipe_id=created["head"]["recipe_id"], expected_head_version=1)
        with self.assertRaises(RecipeError) as error:
            self.store.revise(uid(), params, native)
        self.assertEqual(error.exception.kind, "head_not_latest")
        advanced = self.store.import_prepare(uid(), {"dump": json.dumps(incoming), "advance_heads": [revision_ref(second)]})
        self.store.import_commit(uid(), {"import_plan_id": advanced["import_plan_id"], "import_plan_sha256": advanced["import_plan_sha256"], "accepted_warning_ids": []})
        self.assertEqual(self.store.get_head(created["head"]["recipe_id"])["ref"], revision_ref(second))

    def test_multiple_processes_claim_one_plan_only_once(self):
        created = self.create()
        plan = make_plan(self.store, created["head"])
        context = multiprocessing.get_context("spawn")
        gate = context.Barrier(2)
        replies = context.Queue()
        children = [context.Process(target=process_claim, args=(self.path, plan, gate, replies)) for _ in range(2)]
        for child in children:
            child.start()
        for child in children:
            child.join(20)
            self.assertEqual(child.exitcode, 0)
        results = [replies.get(timeout=2) for _ in children]
        self.assertEqual(len({op for op, _ in results}), 1)
        self.assertEqual(sum(new for _, new in results), 1)

    def test_live_owner_remains_live_and_dead_owner_becomes_uncertain(self):
        created = self.create()
        plan = make_plan(self.store, created["head"])
        args = apply_args(plan)
        original, new = self.store.claim_apply(uid(), args, initial_operation(plan))
        self.assertTrue(new)
        observer = RecipeStore(self.path)
        self.addCleanup(observer.close)
        self.assertEqual(observer.get_operation(original["operation_id"])["status"], "dispatching")
        self.store.close()
        uncertain = observer.get_operation(original["operation_id"])
        self.assertEqual(uncertain["status"], "outcome_unknown")
        self.assertEqual(uncertain["recovery"]["action"], "inspect_scene_do_not_resend")
        with patch("orca_slicer_mcp.recipes_store.expired", return_value=True):
            replayed = observer.existing_apply(uid(), args)
        self.assertEqual(replayed["operation_id"], original["operation_id"])
        self.assertEqual(replayed["status"], "outcome_unknown")

    def test_claim_checks_archive_and_digest_inside_transaction(self):
        created = self.create()
        plan = make_plan(self.store, created["head"])
        self.store.archive(uid(), {"recipe_id": created["head"]["recipe_id"], "expected_head_version": 1, "archived": True})
        with self.assertRaises(RecipeError) as error:
            self.store.claim_apply(uid(), apply_args(plan), initial_operation(plan))
        self.assertEqual(error.exception.kind, "stale_plan")
        wrong = apply_args(plan)
        wrong["plan_sha256"] = "0" * 64
        with self.assertRaises(RecipeError) as error:
            self.store.existing_apply(uid(), wrong)
        self.assertEqual(error.exception.kind, "digest_mismatch")

    def test_operation_updates_are_compare_and_swap_and_replay_returns_latest(self):
        created = self.create()
        plan = make_plan(self.store, created["head"])
        request = uid()
        op, _ = self.store.claim_apply(request, apply_args(plan), initial_operation(plan))
        _, version, _, _ = self.store.operation_snapshot(op["operation_id"])
        job = plan["binding"]["target"]["window_id"] + ":1"
        current = self.store.update_operation(op["operation_id"], version, {"status": "observing", "native_job_id": job,
            "native_state": "running", "recovery": {"action": "observe_job", "message": "Observe this same native job"}})
        stale = self.store.update_operation(op["operation_id"], version, {"status": "outcome_unknown"})
        self.assertEqual(stale, current)
        self.assertEqual(self.store.replay(request, "orca_recipe_apply", "apply", apply_args(plan)), current)

    def test_backup_preserves_unknown_intents_and_rejects_future_schema(self):
        created = self.create()
        plan = make_plan(self.store, created["head"])
        op, _ = self.store.claim_apply(uid(), apply_args(plan), initial_operation(plan))
        other = RecipeStore(self.path)
        self.addCleanup(other.close)
        self.store.close()
        self.assertEqual(other.get_operation(op["operation_id"])["status"], "outcome_unknown")
        backup = other.backup()
        restored = RecipeStore(backup)
        self.addCleanup(restored.close)
        self.assertEqual(restored.get_operation(op["operation_id"])["status"], "outcome_unknown")
        self.assertEqual(restored.get(created["head"])["record"], created["record"])
        other.conn.execute("PRAGMA user_version=999")
        with self.assertRaises(RecipeError) as error:
            other.archive(uid(), {"recipe_id": created["head"]["recipe_id"], "expected_head_version": 1, "archived": True})
        self.assertEqual(error.exception.kind, "unsupported_format")
        with self.assertRaises(RecipeError):
            RecipeStore(self.path)

    @unittest.skipUnless(os.name == "posix", "POSIX file modes/symlink semantics")
    def test_storage_refuses_symlinks_and_nonprivate_files(self):
        alias = Path(self.directory.name) / "alias.sqlite3"
        alias.symlink_to(self.path)
        with self.assertRaises(RecipeError):
            RecipeStore(alias)
        self.store.close()
        self.path.chmod(0o644)
        with self.assertRaises(RecipeError):
            RecipeStore(self.path)

    def test_failed_request_keeps_its_identity_and_original_failure(self):
        created = self.create()
        request = uid()
        args = {"recipe_id": created["head"]["recipe_id"], "expected_head_version": 99, "archived": True}
        with self.assertRaises(RecipeError) as first:
            self.store.archive(request, args)
        self.assertEqual(first.exception.kind, "library_conflict")
        with self.assertRaises(RecipeError) as repeated:
            self.store.replay(request, "orca_recipe_library", "archive", args)
        self.assertEqual(repeated.exception.as_dict(), first.exception.as_dict())
        altered = {**args, "expected_head_version": 1}
        with self.assertRaises(RecipeError) as changed:
            self.store.archive(request, altered)
        self.assertEqual(changed.exception.kind, "request_conflict")
        self.assertFalse(self.store.get_head(created["head"]["recipe_id"])["archived"])
        self.store.fail_request(request, "orca_recipe_library", "archive", altered,
                                RecipeError("other", "Never overwrite original binding"))
        with self.assertRaises(RecipeError) as original:
            self.store.replay(request, "orca_recipe_library", "archive", args)
        self.assertEqual(original.exception.kind, "library_conflict")

    def test_failed_native_preflight_can_be_recorded_without_clobbering_claim(self):
        params, _ = create_params()
        request = uid()
        self.store.fail_request(request, "orca_recipe_library", "create", params,
                                RecipeError("conflict", "Native input changed"))
        with self.assertRaises(RecipeError) as repeated:
            self.store.replay(request, "orca_recipe_library", "create", params)
        self.assertEqual(repeated.exception.kind, "conflict")
        created = self.create()
        plan = make_plan(self.store, created["head"])
        args = apply_args(plan)
        dispatch = uid()
        operation, _ = self.store.claim_apply(dispatch, args, initial_operation(plan))
        self.store.fail_request(dispatch, "orca_recipe_apply", "apply", args,
                                RecipeError("conflict", "Must not hide consumed intent"))
        self.assertEqual(self.store.replay(dispatch, "orca_recipe_apply", "apply", args)["operation_id"], operation["operation_id"])

    def test_raw_duplicate_keys_are_rejected_before_import(self):
        before = self.store.describe()["library_generation"]
        with self.assertRaises(RecipeError):
            self.store.import_prepare(uid(), {"dump": '{"format":"orca.recipe-library","format":"orca.recipe-library"}'})
        self.assertEqual(self.store.describe()["library_generation"], before)


if __name__ == "__main__":
    unittest.main()
