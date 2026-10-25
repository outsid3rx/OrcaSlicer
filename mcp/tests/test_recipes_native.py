"""Native compilation/admission invariants using a deterministic, non-GUI client."""
import asyncio
import copy
import json
from pathlib import Path
import tempfile
import unittest
import uuid

from orca_slicer_mcp.client import ClientError
from orca_slicer_mcp.recipes_contract import RecipeError, digest, make_native_contract, revision_ref, validate
from orca_slicer_mcp.recipes_native import RecipeService, scope_params, _same_value


def uid():
    return str(uuid.uuid4())


def binding(scope='object'):
    result = {'target': {'session_id': uid(), 'window_id': uid()}, 'scope': scope,
              'plate_id': 'plate:1:1', 'state_token': 'a' * 64}
    if scope in ('object', 'volume', 'height_range'):
        result['object_id'] = 'object:1'
    if scope == 'volume':
        result['volume_id'] = 'volume:1'
    if scope == 'height_range':
        result['height_range'] = [0.25, 10.5]
    return result


class Native:
    def __init__(self, owner, count=1):
        self.binding = copy.deepcopy(owner)
        self.keys = ['setting_' + str(i) for i in range(count)]
        self.definitions = {key: {'type': 2, 'min': 0, 'max': 100, 'enum_values': [], 'units': '',
            'writable': True, 'scopes': [owner['scope']]} for key in self.keys}
        self.rows = {key: {'value': '1', 'own_value': None} for key in self.keys}
        self.calls, self.patches = [], []
        self.lost_ack = False
        self.fail = False
        self.supersede = False
        self.omit_last = False
        self.definition_page_size = 100
        self.on_patch = None
        self.capabilities = {'protocol_version': 1, 'api_revision': 3, 'app_version': 'fixture',
            'capabilities': ['settings.describe', 'settings.get', 'settings.patch', 'jobs.get'], 'scopes': [owner['scope']]}

    def select(self, target):
        if target.model_dump() != self.binding['target']:
            raise AssertionError('target changed')
        return target.model_dump()

    async def call(self, target, method, params, **kwargs):
        self.select(target)
        self.calls.append((method, copy.deepcopy(params)))
        if method == 'system.describe':
            value = self.capabilities
        elif method == 'settings.describe':
            keys = [key for key in params['keys'] if key in self.definitions]
            start = params.get('offset', 0)
            end = min(start + self.definition_page_size, len(keys))
            value = {'settings': {key: self.definitions[key] for key in keys[start:end]},
                'pagination': {'next_offset': end if end < len(keys) else None}}
        elif method == 'settings.get':
            keys = params['keys']
            if 'layer_height' in keys:
                return {'state': 'failed', 'error': {'kind': 'forbidden_setting', 'message': 'Unavailable key'}}
            value = {**scope_params(self.binding), 'state_token': self.binding['state_token'],
                'values': {key: copy.deepcopy(self.rows[key]) for key in keys}}
            if self.omit_last and len(keys) > 1:
                value['values'].pop(keys[-1])
        else:
            raise AssertionError(method)
        return {'state': 'succeeded', 'result': copy.deepcopy(value)}

    async def rpc(self, selected, method, params, request_id):
        assert method == 'settings.patch'
        if self.on_patch:
            self.on_patch()
        self.patches.append(copy.deepcopy((params, request_id)))
        if not self.fail:
            for key, value in params['values'].items():
                self.rows[key] = {'value': value, 'own_value': value}
            self.binding['state_token'] = 'b' * 64
        if self.lost_ack:
            raise ClientError('transport_error', 'Admission reply lost')
        return {'job_id': self.binding['target']['window_id'] + ':1'}

    async def job(self, target, job_id, wait_seconds):
        if self.fail:
            return {'state': 'failed', 'error': {'kind': 'invalid_config', 'message': 'Rejected atomically', 'details': {'validation': 'native-only'}}}
        token = self.binding['state_token']
        if self.supersede:
            self.binding['state_token'] = 'c' * 64
        return {'state': 'succeeded', 'result': {'state_token': token}}


class MemoryStore:
    def __init__(self):
        self.library_id = uid()
        self.records, self.plans, self.operations, self.versions = {}, {}, {}, {}
        self.claimed, self.requests, self.failures = {}, {}, {}

    def describe(self):
        return {'library_id': self.library_id}

    def add(self, native, scope, values=None, compatibility=None):
        payload = {'format': 'orca.recipe', 'format_version': 1, 'recipe_id': uid(), 'revision': 1,
            'created_at': '2026-10-25T00:00:00Z', 'parent': None, 'derived_from': None,
            'name': 'fixture', 'description': '', 'tags': [], 'scope': scope,
            'values': values or {key: '2' for key in native.keys},
            'compatibility': compatibility or {}, 'evidence': [],
            'native_contract': make_native_contract(native.capabilities, scope, native.definitions)}
        # Contract definitions include exactly assigned keys.
        payload['native_contract']['definitions'] = {key: payload['native_contract']['definitions'][key] for key in payload['values']}
        record = {'payload': payload, 'sha256': digest(payload)}
        ref = revision_ref(record)
        self.records[ref['sha256']] = {'record': record, 'head': ref, 'head_version': 1, 'archived': False}
        return ref

    def get(self, ref):
        return copy.deepcopy(self.records[ref['sha256']])

    def replay(self, request_id, tool, action, parameters):
        if request_id in self.failures:
            arguments, error = self.failures[request_id]
            if arguments != digest({'tool': tool, 'action': action, 'parameters': parameters}):
                raise RecipeError('request_conflict', 'Request reused with different arguments')
            raise error
        return None

    def fail_request(self, request_id, tool, action, parameters, error):
        if request_id not in self.requests and request_id not in self.failures:
            self.failures[request_id] = (digest({'tool': tool, 'action': action, 'parameters': parameters}), error)

    def save_plan(self, request_id, parameters, plan, native_request):
        self.plans[plan['plan_id']] = copy.deepcopy((plan, native_request))
        return plan

    def get_plan(self, plan_id, sha=None):
        plan, native = self.plans[plan_id]
        if sha is not None and sha != plan['plan_sha256']:
            raise RecipeError('plan_conflict', 'Wrong hash')
        return copy.deepcopy((plan, native))

    def existing_apply(self, request_id, parameters):
        self.get_plan(parameters['plan_id'], parameters['plan_sha256'])
        if request_id in self.requests and self.requests[request_id] != digest(parameters):
            raise RecipeError('request_conflict', 'Request reused')
        operation_id = self.claimed.get(parameters['plan_id'])
        if operation_id:
            self.requests[request_id] = digest(parameters)
            return copy.deepcopy(self.operations[operation_id])

    def claim_apply(self, request_id, parameters, operation):
        previous = self.existing_apply(request_id, parameters)
        if previous:
            return previous, False
        key = operation['operation_id']
        self.operations[key], self.versions[key] = copy.deepcopy(operation), 0
        self.claimed[operation['plan_id']] = key
        self.requests[request_id] = digest(parameters)
        return copy.deepcopy(operation), True

    def operation_snapshot(self, operation_id):
        operation = self.operations[operation_id]
        plan, native = self.get_plan(operation['plan_id'])
        return copy.deepcopy(operation), self.versions[operation_id], plan, native

    def update_operation(self, operation_id, expected_version, updates):
        if self.versions[operation_id] == expected_version:
            self.operations[operation_id].update(copy.deepcopy(updates))
            self.versions[operation_id] += 1
        validate('Operation', self.operations[operation_id])
        return copy.deepcopy(self.operations[operation_id])


class NativeRecipesTests(unittest.IsolatedAsyncioTestCase):
    def test_scalar_numeric_equivalence_is_exact_and_type_bound(self):
        for expected, observed, kind in [('0.40', '0.4', 1), ('04', '4', 2),
                ('+04', '4', 2), ('30', '30%', 4), ('30.00%', '30%', 4),
                ('3e1%', '30%', 5), ('0.40', '0.4', 5)]:
            with self.subTest(expected=expected, observed=observed, kind=kind):
                self.assertTrue(_same_value(expected, observed, kind))
        for expected, observed, kind in [('0.40', '0.4', 3), ('04', '4', 9),
                ('01', '1', 8), ('0.4,0.4', '0.40,0.40', 0x4001),
                ('30%', '30', 5), ('30', '30%', 5), ('4.0', '4', 2),
                ('4junk', '4', 2), ('0.4junk', '0.4', 1), ('0.4%', '0.4', 1),
                ('0.4000001', '0.4', 1), ('NaN', '0', 1), ('Infinity', '0', 1),
                (' 0.4', '0.4', 1), ('0.4', None, 1), ('30%%', '30%', 4)]:
            with self.subTest(expected=expected, observed=observed, kind=kind):
                self.assertFalse(_same_value(expected, observed, kind))

    async def test_native_scalar_reserialization_verifies_successfully(self):
        owner, native, store, service, _ = self.setup_service()
        native.definitions['setting_0']['type'] = 1
        ref = store.add(native, owner['scope'], values={'setting_0': '0.40'})
        original_rpc = native.rpc
        async def reserializing_rpc(*args):
            ack = await original_rpc(*args)
            native.rows['setting_0'] = {'value': '0.4', 'own_value': '0.4'}
            return ack
        native.rpc = reserializing_rpc
        plan = await service.dispatch('orca_recipe_apply', 'plan', {'recipes': [ref], 'binding': owner}, uid())
        arguments = {'plan_id': plan['plan_id'], 'plan_sha256': plan['plan_sha256'],
                     'accepted_warning_ids': [w['id'] for w in plan['warnings']]}
        operation = await service.dispatch('orca_recipe_apply', 'apply', arguments, uid())
        self.assertEqual((operation['status'], operation['verification']), ('succeeded', 'matched'))
        self.assertEqual(len(native.patches), 1)

    def setup_service(self, count=1, scope='object'):
        owner = binding(scope)
        native, store = Native(owner, count), MemoryStore()
        service = RecipeService(native, store)
        ref = store.add(native, scope)
        return owner, native, store, service, ref

    async def prepare(self, count=1, scope='object'):
        owner, native, store, service, ref = self.setup_service(count, scope)
        plan = await service.dispatch('orca_recipe_apply', 'plan', {'recipes': [ref], 'binding': owner}, uid())
        apply = {'plan_id': plan['plan_id'], 'plan_sha256': plan['plan_sha256'],
                 'accepted_warning_ids': [w['id'] for w in plan['warnings']]}
        return owner, native, store, service, ref, plan, apply

    async def test_override_ownership_and_exact_range_mapping(self):
        owner, native, store, service, ref, plan, apply = await self.prepare(scope='height_range')
        self.assertTrue(plan['diff'][0]['changes_ownership'])
        native_request = store.get_plan(plan['plan_id'])[1]
        self.assertEqual(native_request['params']['range'], owner['height_range'])
        self.assertNotIn('height_range', native_request['params'])
        self.assertEqual(plan['native_request_bytes'], len(json.dumps(native_request, separators=(',', ':'), ensure_ascii=False).encode()))
        self.assertEqual(plan['validation_level'], 'schema_checked_native_validation_pending')

    async def test_claim_exists_before_patch_and_duplicate_never_resends(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        native.on_patch = lambda: self.assertEqual(len(store.operations), 1)
        operation = await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(operation['status'], 'succeeded')
        self.assertEqual(operation['verification'], 'matched')
        self.assertEqual(len(native.patches), 1)
        store.plans[plan['plan_id']][0]['expires_at'] = '2000-01-01T00:00:00Z'
        store.records[ref['sha256']]['archived'] = True
        same = await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(same, operation)
        self.assertEqual(len(native.patches), 1)

    async def test_lost_admission_reply_remains_consumed_after_restart(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        native.lost_ack = True
        operation = await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(operation['status'], 'outcome_unknown')
        other = RecipeService(native, store)
        same = await other.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(operation, same)
        self.assertEqual(len(native.patches), 1)

    async def test_success_followed_by_manual_change_is_superseded(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        native.supersede = True
        operation = await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(operation['native_state'], 'succeeded')
        self.assertEqual(operation['status'], 'outcome_unknown')
        self.assertEqual(operation['verification'], 'superseded')

    async def test_atomic_native_failure_is_verified_unchanged(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        native.fail = True
        operation = await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual((operation['status'], operation['verification']), ('failed', 'unchanged'))

    async def test_noop_requires_matching_own_value(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        native.rows['setting_0'] = {'value': '2', 'own_value': '2'}
        operation = await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual((operation['status'], operation['verification']), ('succeeded', 'unchanged'))
        self.assertIsNone(operation['native_state'])
        self.assertEqual(native.patches, [])

    async def test_all_schema_pages_and_all_256_verification_keys(self):
        owner, native, store, service, ref = self.setup_service(256)
        native.definition_page_size = 37
        plan = await service.dispatch('orca_recipe_apply', 'plan', {'recipes': [ref], 'binding': owner}, uid())
        self.assertEqual(len(plan['diff']), 256)
        native.calls.clear()
        operation = await service.dispatch('orca_recipe_apply', 'apply', {'plan_id': plan['plan_id'],
            'plan_sha256': plan['plan_sha256'], 'accepted_warning_ids': [w['id'] for w in plan['warnings']]}, uid())
        self.assertEqual(operation['status'], 'succeeded')
        reads = [params['keys'] for method, params in native.calls if method == 'settings.get']
        self.assertTrue(all(len(keys) <= 100 for keys in reads))
        self.assertTrue(any(len(keys) == 56 for keys in reads))

    async def test_missing_key_from_value_chunk_cannot_look_successful(self):
        owner, native, store, service, ref = self.setup_service(101)
        native.omit_last = True
        with self.assertRaises(RecipeError) as error:
            await service.dispatch('orca_recipe_apply', 'plan', {'recipes': [ref], 'binding': owner}, uid())
        self.assertEqual(error.exception.kind, 'invalid_response')
        self.assertEqual(store.plans, {})

    async def test_signature_change_and_unknown_material_block_plan(self):
        owner, native, store, service, ref = self.setup_service()
        native.definitions['setting_0']['max'] = 99
        checked = await service.dispatch('orca_recipes', 'check', {'recipes': [ref], 'binding': owner})
        self.assertEqual(checked['status'], 'migration_required')
        native.definitions['setting_0']['max'] = 100
        record = store.records[ref['sha256']]['record']
        record['payload']['compatibility'] = {'filament_types': ['PLA']}
        record['sha256'] = digest(record['payload'])
        newref = revision_ref(record)
        store.records[newref['sha256']] = store.records.pop(ref['sha256'])
        with self.assertRaises(RecipeError) as error:
            await service.dispatch('orca_recipe_apply', 'plan', {'recipes': [newref], 'binding': owner}, uid())
        self.assertEqual(error.exception.kind, 'unknown')
        self.assertEqual(native.patches, [])

    async def test_conflicts_need_explicit_valid_resolution(self):
        owner, native, store, service, ref = self.setup_service()
        other = store.add(native, owner['scope'], {'setting_0': '3'})
        params = {'recipes': [ref, other], 'binding': owner}
        with self.assertRaises(RecipeError) as error:
            await service.dispatch('orca_recipe_apply', 'plan', params, uid())
        self.assertEqual(error.exception.kind, 'composition_conflict')
        params['resolutions'] = [{'key': 'setting_0', 'chosen_value': '3'}]
        plan = await service.dispatch('orca_recipe_apply', 'plan', params, uid())
        self.assertEqual(plan['values']['setting_0'], '3')
        self.assertEqual(plan['diff'][0]['sources'], [other])
        params['resolutions'].append({'key': 'setting_0', 'chosen_value': '2'})
        with self.assertRaises(RecipeError):
            await service.dispatch('orca_recipe_apply', 'plan', params, uid())

    async def test_wrong_plan_hash_does_not_expose_existing_operation(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        apply['plan_sha256'] = '0' * 64
        with self.assertRaises(RecipeError):
            await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(len(native.patches), 1)

    async def test_stale_token_and_missing_warning_ack_do_not_claim(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        bad = {**apply, 'accepted_warning_ids': []}
        with self.assertRaises(RecipeError):
            await service.dispatch('orca_recipe_apply', 'apply', bad, uid())
        native.binding['state_token'] = 'c' * 64
        with self.assertRaises(RecipeError):
            await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(store.operations, {})
        self.assertEqual(native.patches, [])

    async def test_complete_request_byte_limit_includes_target_and_owners(self):
        owner, native, store, service, _ = self.setup_service(20)
        # Two individually portable recipes compose into an oversized native body.
        for definition in native.definitions.values():
            definition.update(type=3, min=None, max=None)
        refs = [store.add(native, owner['scope'], {key: 'x' * 4096 for key in native.keys[offset:offset + 10]})
                for offset in (0, 10)]
        from orca_slicer_mcp.recipes_contract import validate_record
        for ref in refs:
            validate_record(store.get(ref)['record'])
        with self.assertRaises(RecipeError) as error:
            await service.dispatch('orca_recipe_apply', 'plan', {'recipes': refs, 'binding': owner}, uid())
        self.assertEqual(error.exception.kind, 'request_too_large')
        self.assertEqual(store.plans, {})


    async def test_cancel_after_admission_before_ack_persistence_is_consumed_unknown(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        admitted, release = asyncio.Event(), asyncio.Event()
        original = native.rpc
        async def blocked_rpc(*args):
            ack = await original(*args)
            admitted.set()
            await release.wait()
            return ack
        native.rpc = blocked_rpc
        task = asyncio.create_task(service.dispatch('orca_recipe_apply', 'apply', apply, uid()))
        await admitted.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        operation = next(iter(store.operations.values()))
        self.assertEqual(operation['status'], 'outcome_unknown')
        self.assertIsNone(operation['native_job_id'])
        repeated = await RecipeService(native, store).dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(repeated, operation)
        self.assertEqual(len(native.patches), 1)

    async def test_cancel_after_ack_persistence_keeps_observable_job(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        polling, release = asyncio.Event(), asyncio.Event()
        async def blocked_job(*args):
            polling.set()
            await release.wait()
        native.job = blocked_job
        task = asyncio.create_task(service.dispatch('orca_recipe_apply', 'apply', apply, uid()))
        await polling.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        operation = next(iter(store.operations.values()))
        self.assertEqual(operation['status'], 'observing')
        self.assertIsNotNone(operation['native_job_id'])
        self.assertEqual(len(native.patches), 1)

    async def test_unknown_rpc_error_never_claims_definite_native_failure(self):
        owner, native, store, service, ref, plan, apply = await self.prepare()
        async def custom_rpc(*args):
            raise ClientError('unexpected_native_fault', 'Future native fault semantics are unknown')
        native.rpc = custom_rpc
        operation = await service.dispatch('orca_recipe_apply', 'apply', apply, uid())
        self.assertEqual(operation['status'], 'outcome_unknown')
        self.assertEqual(operation['verification'], 'unavailable')


    async def test_native_forbidden_removed_key_is_migration_required(self):
        owner, native, store, service, ref = self.setup_service()
        async def removed(*args, **kwargs):
            return {'state': 'failed', 'error': {'kind': 'forbidden_setting', 'message': 'No longer writable'}}
        native.call = removed
        result = await service.dispatch('orca_recipes', 'check', {'recipes': [ref], 'binding': owner})
        self.assertEqual(result['status'], 'migration_required')
        self.assertEqual(store.plans, {})

    async def test_safe_nozzle_context_queries_no_forbidden_project_identity(self):
        owner, native, store, service, ref = self.setup_service()
        native.capabilities['capabilities'].append('presets.describe')
        original = native.call
        async def with_machine(target, method, params, **kwargs):
            if method == 'presets.describe':
                self.assertEqual(params['preset_type'], 'machine')
                self.assertEqual(params['keys'], ['nozzle_diameter'])
                return {'state': 'succeeded', 'result': {'settings': {'nozzle_diameter': {'value': '0.4,0.4'}}}}
            return await original(target, method, params, **kwargs)
        native.call = with_machine
        entry = store.records[ref['sha256']]
        entry['record']['payload']['compatibility'] = {'nozzle_diameters_mm': ['0.40']}
        entry['record']['sha256'] = digest(entry['record']['payload'])
        ref = revision_ref(entry['record'])
        store.records[ref['sha256']] = entry
        result = await service.dispatch('orca_recipes', 'check', {'recipes': [ref], 'binding': owner})
        self.assertEqual(result['status'], 'compatible')
        self.assertIn('unverified_recipe', [warning['code'] for warning in result['warnings']])


class NativeStoreIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def test_capture_plan_apply_and_cross_client_replay_with_real_sqlite(self):
        from orca_slicer_mcp.recipes_store import RecipeStore
        with tempfile.TemporaryDirectory() as directory:
            owner = binding()
            native = Native(owner)
            path = Path(directory) / 'recipes.sqlite3'
            store = RecipeStore(path)
            other = RecipeStore(path)
            try:
                service = RecipeService(native, store)
                draft = {'name': 'Captured fixture', 'description': '', 'tags': [], 'scope': 'object',
                         'values': {'setting_0': '2'}, 'compatibility': {}}
                creation_id = uid()
                creation = {'draft': draft, 'capture_binding': owner}
                result = await service.dispatch('orca_recipe_library', 'create', creation, creation_id)
                ref = revision_ref(result['record'])
                self.assertEqual(result['record']['payload']['evidence'], [])
                self.assertEqual(len((await service.dispatch('orca_recipes', 'list', {}))['items']), 1)
                plan = await service.dispatch('orca_recipe_apply', 'plan', {'recipes': [ref], 'binding': owner}, uid())
                native.on_patch = lambda: self.assertEqual(other.get_operation(next(iter(
                    row[0] for row in other.conn.execute('SELECT operation_id FROM operations'))))['status'], 'dispatching')
                params = {'plan_id': plan['plan_id'], 'plan_sha256': plan['plan_sha256'],
                          'accepted_warning_ids': [warning['id'] for warning in plan['warnings']]}
                operation = await service.dispatch('orca_recipe_apply', 'apply', params, uid())
                self.assertEqual(operation['status'], 'succeeded')
                duplicate = await RecipeService(native, other).dispatch('orca_recipe_apply', 'apply', params, uid())
                self.assertEqual(operation, duplicate)
                self.assertEqual(len(native.patches), 1)
                # Replaying a library receipt never performs another native capture.
                calls = len(native.calls)
                same = await service.dispatch('orca_recipe_library', 'create', creation, creation_id)
                self.assertEqual(same, result)
                self.assertEqual(len(native.calls), calls)
            finally:
                other.close()
                store.close()

    async def test_lost_ack_survives_sqlite_reopen_without_resend(self):
        from orca_slicer_mcp.recipes_store import RecipeStore
        with tempfile.TemporaryDirectory() as directory:
            owner = binding()
            native = Native(owner)
            path = Path(directory) / 'recipes.sqlite3'
            store = RecipeStore(path)
            service = RecipeService(native, store)
            created = await service.dispatch('orca_recipe_library', 'create', {'capture_binding': owner,
                'draft': {'name': 'Fixture', 'description': '', 'tags': [], 'scope': 'object',
                          'values': {'setting_0': '2'}, 'compatibility': {}}}, uid())
            plan = await service.dispatch('orca_recipe_apply', 'plan', {
                'recipes': [revision_ref(created['record'])], 'binding': owner}, uid())
            params = {'plan_id': plan['plan_id'], 'plan_sha256': plan['plan_sha256'],
                      'accepted_warning_ids': [warning['id'] for warning in plan['warnings']]}
            native.lost_ack = True
            operation = await service.dispatch('orca_recipe_apply', 'apply', params, uid())
            store.close()
            store = RecipeStore(path)
            try:
                repeated = await RecipeService(native, store).dispatch('orca_recipe_apply', 'apply', params, uid())
                self.assertEqual(repeated['operation_id'], operation['operation_id'])
                self.assertEqual(repeated['status'], 'outcome_unknown')
                self.assertEqual(len(native.patches), 1)
            finally:
                store.close()


    async def test_other_live_client_observes_dispatching_without_guessing_dead_owner(self):
        from orca_slicer_mcp.recipes_store import RecipeStore
        with tempfile.TemporaryDirectory() as directory:
            owner = binding()
            native = Native(owner)
            store = RecipeStore(Path(directory) / 'recipes.sqlite3')
            other = RecipeStore(store.path)
            try:
                service = RecipeService(native, store)
                created = await service.dispatch('orca_recipe_library', 'create', {'capture_binding': owner,
                    'draft': {'name': 'Fixture', 'description': '', 'tags': [], 'scope': 'object',
                              'values': {'setting_0': '2'}, 'compatibility': {}}}, uid())
                plan = await service.dispatch('orca_recipe_apply', 'plan', {
                    'recipes': [revision_ref(created['record'])], 'binding': owner}, uid())
                params = {'plan_id': plan['plan_id'], 'plan_sha256': plan['plan_sha256'],
                          'accepted_warning_ids': [warning['id'] for warning in plan['warnings']]}
                admitted, release = asyncio.Event(), asyncio.Event()
                original = native.rpc
                async def blocked_rpc(*args):
                    ack = await original(*args)
                    admitted.set()
                    await release.wait()
                    return ack
                native.rpc = blocked_rpc
                task = asyncio.create_task(service.dispatch('orca_recipe_apply', 'apply', params, uid()))
                await admitted.wait()
                operation_id = other.conn.execute('SELECT operation_id FROM operations').fetchone()[0]
                observation = await RecipeService(native, other).dispatch('orca_recipes', 'operation.get',
                    {'operation_id': operation_id})
                self.assertEqual(observation['status'], 'dispatching')
                release.set()
                result = await task
                self.assertEqual(result['status'], 'succeeded')
                self.assertEqual(len(native.patches), 1)
            finally:
                other.close()
                store.close()

    async def test_failed_native_capture_request_is_durable_and_cannot_be_rebound(self):
        from orca_slicer_mcp.recipes_store import RecipeStore
        with tempfile.TemporaryDirectory() as directory:
            owner = binding()
            native = Native(owner)
            store = RecipeStore(Path(directory) / 'recipes.sqlite3')
            try:
                service = RecipeService(native, store)
                params = {'capture_binding': owner, 'draft': {'name': 'Fixture', 'description': '',
                    'tags': [], 'scope': 'object', 'values': {'setting_0': '2'}, 'compatibility': {}}}
                request_id = uid()
                native.binding['state_token'] = 'c' * 64
                with self.assertRaises(RecipeError) as first:
                    await service.dispatch('orca_recipe_library', 'create', params, request_id)
                self.assertEqual(first.exception.kind, 'stale_plan')
                native.binding['state_token'] = owner['state_token']
                count = len(native.calls)
                with self.assertRaises(RecipeError) as repeated:
                    await service.dispatch('orca_recipe_library', 'create', params, request_id)
                self.assertEqual(repeated.exception.kind, 'stale_plan')
                self.assertEqual(len(native.calls), count)
                changed = copy.deepcopy(params)
                changed['draft']['values']['setting_0'] = '3'
                with self.assertRaises(RecipeError) as rebound:
                    await service.dispatch('orca_recipe_library', 'create', changed, request_id)
                self.assertEqual(rebound.exception.kind, 'request_conflict')
                self.assertEqual((await service.dispatch('orca_recipes', 'list', {}))['items'], [])
            finally:
                store.close()

    async def test_request_schema_rejection_does_not_consume_unaccepted_intent(self):
        from orca_slicer_mcp.recipes_store import RecipeStore
        with tempfile.TemporaryDirectory() as directory:
            owner = binding()
            native = Native(owner)
            store = RecipeStore(Path(directory) / 'recipes.sqlite3')
            try:
                service = RecipeService(native, store)
                request_id = uid()
                with self.assertRaises(RecipeError):
                    await service.dispatch('orca_recipe_library', 'create', {'capture_binding': owner}, request_id)
                valid = {'capture_binding': owner, 'draft': {'name': 'Fixture', 'description': '',
                    'tags': [], 'scope': 'object', 'values': {'setting_0': '2'}, 'compatibility': {}}}
                result = await service.dispatch('orca_recipe_library', 'create', valid, request_id)
                self.assertEqual(result['head_version'], 1)
            finally:
                store.close()
