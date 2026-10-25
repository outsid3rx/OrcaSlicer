"""Recipe compilation and one-shot native execution.

Native reads are safe to repeat. A durable claim precedes patch admission; neither
this module nor operation observation ever resends an admitted or uncertain intent.
"""
from __future__ import annotations

from decimal import Decimal, InvalidOperation
import json
import re
import uuid

from .client import ClientError, Target
from .recipes_contract import (RecipeError, digest, validate, validate_request, request_schema,
    revision_ref, utc_now, expires_at, expired, normalize_definition, make_native_contract)


def scope_params(binding):
    return {('range' if key == 'height_range' else key): value
            for key, value in binding.items() if key not in ('target', 'state_token')}


def _warning(code, message, ref=None):
    return {'id': digest({'code': code, 'message': message, 'ref': ref}),
            'code': code, 'message': message, 'acknowledgement_required': True}


def _error(kind, message, **details):
    raise RecipeError(kind, message, details)


def _job_error(job):
    error = job.get('error')
    if not isinstance(error, dict):
        return None
    # Native errors may also carry domain-specific details. The portable
    # Operation error schema intentionally retains only bounded kind/message.
    return {'kind': str(error.get('kind') or 'native_error')[:80],
            'message': str(error.get('message') or 'Native observation failed')[:4096]}


def _recovery(job=False):
    return {'action': 'observe_job' if job else 'inspect_scene_do_not_resend',
            'message': 'Observe the saved job; never resend this plan.' if job else
                       'Inspect the scene and saved receipt. This plan is consumed and must not be resent.'}


def _same_value(expected, observed, native_type):
    """Accept exact scalar numeric equivalence, never a native parser prefix.

    Orca serializes parsed scalar numbers again (e.g. 0.40 becomes 0.4).
    Decimal equality avoids an epsilon or guessing the serializer's precision.
    Unknown types, strings, booleans, enums and vectors retain exact matching.
    """
    if expected == observed:
        return True
    if not isinstance(expected, str) or not isinstance(observed, str):
        return False
    if native_type not in (1, 2, 4, 5):
        return False
    if native_type == 5 and expected.endswith('%') != observed.endswith('%'):
        return False
    if native_type in (4, 5):
        expected, observed = expected.removesuffix('%'), observed.removesuffix('%')
    pattern = (r'[+-]?[0-9]+' if native_type == 2 else
               r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?')
    if not re.fullmatch(pattern, expected) or not re.fullmatch(pattern, observed):
        return False
    try:
        left, right = Decimal(expected), Decimal(observed)
        return left.is_finite() and right.is_finite() and left == right
    except InvalidOperation:
        return False


class RecipeService:
    def __init__(self, client, store):
        self.client, self.store = client, store

    async def dispatch(self, tool, action, parameters, request_id=None):
        args = {'action': action, 'parameters': parameters}
        if request_id is not None:
            args['request_id'] = request_id
        validate_request(tool, args)
        try:
            return await self._dispatch(tool, action, parameters, request_id)
        except RecipeError as error:
            if tool in ('orca_recipe_library', 'orca_recipe_apply'):
                self.store.fail_request(request_id, tool, action, parameters, error)
            raise

    async def _dispatch(self, tool, action, parameters, request_id):
        if tool == 'orca_recipe_apply':
            if action == 'apply':
                return await self.apply(parameters, request_id)
            previous = self.store.replay(request_id, tool, action, parameters)
            if previous is not None:
                return previous
            return await self.plan(parameters, request_id)
        if tool == 'orca_recipe_library':
            previous = self.store.replay(request_id, tool, action, parameters)
            if previous is not None:
                return previous
            if action in ('create', 'revise', 'fork'):
                draft, binding = parameters['draft'], parameters['capture_binding']
                if draft['scope'] != binding['scope']:
                    _error('scope_mismatch', 'Draft and capture binding must have exactly the same scope')
                snapshot = await self.snapshot(binding, sorted(draft['values']))
                self.validate_values(draft['values'], snapshot['definitions'])
                native = make_native_contract(snapshot['capabilities'], binding['scope'], snapshot['definitions'])
                return getattr(self.store, action)(request_id, parameters, native)
            return getattr(self.store, action.replace('.', '_'))(request_id, parameters)
        if action == 'check':
            return (await self.check(parameters))[0]
        if action == 'operation.get':
            return await self.observe(parameters['operation_id'], parameters.get('wait_seconds', 0))
        if action == 'describe':
            return {**self.store.describe(), 'request_schema': request_schema(**parameters)}
        if action == 'list':
            return self.store.list_recipes(**parameters)
        if action == 'get':
            return self.store.get(parameters['ref'])
        if action == 'export':
            return self.store.export(**parameters)
        _error('invalid_request', 'Unsupported recipe action')

    async def read(self, binding, method, params):
        result = await self.client.call(Target(**binding['target']), method, params, wait_seconds=1)
        if result.get('state') != 'succeeded':
            error = result.get('error') or {}
            _error(error.get('kind', 'observation_unavailable'), error.get('message',
                'Native observation is incomplete; no recipe action was dispatched'))
        data = result.get('result')
        if not isinstance(data, dict):
            _error('invalid_response', 'Native observation has no object result')
        return data

    @staticmethod
    def _owner(binding, result, token=None):
        if result.get('state_token') != (token or binding['state_token']):
            _error('stale_plan', 'The native state changed during recipe observation')
        for key in ('scope', 'plate_id', 'object_id', 'volume_id'):
            if key in binding and result.get(key) != binding[key]:
                _error('stale_target', 'Settings response belongs to a different scope owner', field=key)
        if binding['scope'] == 'height_range' and result.get('range') != binding['height_range']:
            _error('stale_target', 'Settings response belongs to a different height range')

    async def values(self, binding, keys, token=None):
        """Read every exact key, checking owner/token on every chunk and at end."""
        all_values = {}
        for offset in range(0, len(keys), 100):
            chunk = keys[offset:offset + 100]
            page = await self.read(binding, 'settings.get', {**scope_params(binding), 'keys': chunk, 'limit': 100})
            self._owner(binding, page, token)
            rows = page.get('values', {})
            if set(rows) != set(chunk) or not all(isinstance(row, dict) and
                    isinstance(row.get('value'), str) and 'own_value' in row and
                    (row['own_value'] is None or isinstance(row['own_value'], str)) for row in rows.values()):
                _error('invalid_response', 'Native settings did not return every requested key')
            all_values.update(rows)
        end = await self.read(binding, 'settings.get', {**scope_params(binding), 'keys': keys[:1], 'limit': 100})
        self._owner(binding, end, token)
        return all_values

    async def snapshot(self, binding, keys):
        try:
            return await self._snapshot(binding, keys)
        except RecipeError as error:
            if error.kind == 'forbidden_setting':
                _error('migration_required', 'A required key is no longer advertised in this writable scope')
            raise

    async def _snapshot(self, binding, keys):
        # An initial owner read brackets capability and definition pages too.
        first = await self.read(binding, 'settings.get', {**scope_params(binding), 'keys': keys[:1], 'limit': 100})
        self._owner(binding, first)
        cap = await self.read(binding, 'system.describe', {})
        required = {'settings.describe', 'settings.get', 'settings.patch', 'jobs.get'}
        if (type(cap.get('protocol_version')) is not int or cap['protocol_version'] != 1 or
                type(cap.get('api_revision')) is not int or not isinstance(cap.get('app_version'), str) or
                not isinstance(cap.get('capabilities'), list) or not required.issubset(cap['capabilities'])):
            _error('migration_required', 'The selected native build lacks required recipe methods or protocol')
        if binding['scope'] not in cap.get('scopes', []):
            _error('migration_required', 'The selected native build does not support this scope')
        definitions = {}
        for start in range(0, len(keys), 100):
            chunk, offset = keys[start:start + 100], 0
            seen = set()
            while True:
                page = await self.read(binding, 'settings.describe',
                    {'scope': binding['scope'], 'keys': chunk, 'offset': offset, 'limit': 100})
                rows = page.get('settings', {})
                if not isinstance(rows, dict) or not set(rows).issubset(chunk):
                    _error('invalid_response', 'Unexpected keys in native writable schema')
                if set(rows).intersection(definitions):
                    _error('invalid_response', 'Duplicate key in native writable schema pages')
                definitions.update(rows)
                next_offset = page.get('pagination', {}).get('next_offset')
                if next_offset is None:
                    break
                if type(next_offset) is not int or next_offset <= offset or next_offset in seen:
                    _error('invalid_response', 'Native schema pagination did not advance')
                seen.add(next_offset)
                offset = next_offset
        for key, definition in definitions.items():
            if definition.get('writable') is not True or binding['scope'] not in definition.get('scopes', []):
                _error('migration_required', 'A key is not writable in the selected scope', key=key)
            normalize_definition(binding['scope'], key, definition)
        if set(keys) != set(definitions):
            _error('migration_required', 'A required key is absent from the writable scope',
                   keys=sorted(set(keys) - set(definitions)))
        values = await self.values(binding, keys)
        return {'capabilities': cap, 'definitions': definitions, 'values': values}

    @staticmethod
    def fingerprint(scope, definitions):
        return digest({'protocol_version': 1, 'scope': scope, 'definitions': [
            normalize_definition(scope, key, definitions[key]) for key in sorted(definitions)]})

    @staticmethod
    def validate_values(values, definitions):
        for key, text in values.items():
            d = definitions[key]
            enum = d.get('enum_values', [])
            if enum and text not in enum:
                _error('invalid_value', 'Value is not in the native enum', key=key)
            # Only scalar numeric representations can be checked without the native
            # parser. Vectors, expressions and percentages retain native validation.
            if d.get('type') not in (1, 2, 4, 5):
                continue
            try:
                number = Decimal(text)
            except InvalidOperation:
                continue
            if not number.is_finite():
                _error('invalid_value', 'Numeric settings must be finite', key=key)
            for bound, reject in (('min', lambda n, b: n < b), ('max', lambda n, b: n > b)):
                if d.get(bound) is not None and reject(number, Decimal(str(d[bound]))):
                    _error('invalid_value', 'Value is outside its native range', key=key)

    async def environment(self, binding, snapshot):
        """Only expose observations that the native API can establish exactly.

        Material applicability (paint, supports, linked instances) is not described
        exhaustively by this API, so it deliberately remains unknown in v1.
        """
        env = {'orca_version': snapshot['capabilities']['app_version']}
        if 'presets.describe' in snapshot['capabilities'].get('capabilities', []):
            try:
                page = await self.read(binding, 'presets.describe', {'preset_type': 'machine',
                    'keys': ['nozzle_diameter'], 'limit': 100})
                settings = page.get('settings', {})
                # All machine nozzles is safe; never assume the first vector item.
                raw = settings.get('nozzle_diameter', {}).get('value')
                if raw:
                    nozzles = raw.split(',')
                    if all(Decimal(n).is_finite() and Decimal(n) > 0 for n in nozzles):
                        env['nozzle_diameters_mm'] = nozzles
            except (RecipeError, InvalidOperation):
                pass
        try:
            rows = await self.values(binding, ['layer_height'])
            height = rows['layer_height']['value']
            if Decimal(height).is_finite():
                env['layer_height_mm'] = height
        except (RecipeError, InvalidOperation):
            pass
        # Recheck after every optional contextual read; unavailable data is not
        # permission to accept a changed project.
        await self.values(binding, sorted(snapshot['values'])[:1])
        return env

    @staticmethod
    def compatibility(requirements, env):
        checks = []
        for key, expected in requirements.items():
            observed = env.get(key)
            state = 'unknown'
            if observed is not None:
                if key == 'layer_height_mm':
                    state = 'compatible' if Decimal(expected['min']) <= Decimal(observed) <= Decimal(expected['max']) else 'incompatible'
                elif key == 'nozzle_diameters_mm':
                    allowed = {Decimal(n) for n in expected}
                    state = 'compatible' if observed and all(Decimal(n) in allowed for n in observed) else 'incompatible'
                else:
                    state = 'compatible' if observed and all(value in expected for value in observed) else 'incompatible'
            checks.append({'condition': key, 'status': state, 'expected': expected, 'observed': observed})
        return checks

    @staticmethod
    def evidence_warnings(payload, env, ref):
        warnings, matched = [], False
        for evidence in payload.get('evidence', []):
            same = (env.get('printer_model_ids') == [evidence['printer_model_id']] and
                env.get('filament_types') == [evidence['filament_type']] and
                env.get('nozzle_diameters_mm') is not None and
                all(Decimal(n) == Decimal(evidence['nozzle_diameter_mm']) for n in env['nozzle_diameters_mm']) and
                env.get('orca_version') == evidence['orca_version'] and
                evidence.get('layer_height_mm') is not None and
                env.get('layer_height_mm') is not None and
                Decimal(env['layer_height_mm']) == Decimal(evidence['layer_height_mm']))
            if evidence['outcome'] == 'unsatisfactory':
                warnings.append(_warning('unsatisfactory_evidence', 'The source reports an unsatisfactory physical test.', ref))
            if not same:
                warnings.append(_warning('evidence_environment_changed', 'Reported test conditions are incomplete or differ from the current environment.', ref))
            elif evidence['outcome'] == 'satisfactory':
                matched = True
                warnings.append(_warning('reported_evidence', 'Matching physical evidence is a source report, not trusted MCP validation.', ref))
        if not matched:
            warnings.append(_warning('unverified_recipe', 'No matching satisfactory physical test can be confirmed for this recipe.', ref))
        if payload.get('deprecated'):
            warnings.append(_warning('deprecated_recipe', 'The source marked this revision deprecated: ' + payload.get('deprecation_note', ''), ref))
        return warnings

    async def check(self, parameters):
        binding, refs = parameters['binding'], parameters['recipes']
        if len({digest(ref) for ref in refs}) != len(refs):
            _error('invalid_request', 'Recipe references must be unique')
        entries = [self.store.get(ref) for ref in refs]
        keys = set()
        for entry in entries:
            payload = entry['record']['payload']
            if payload['scope'] != binding['scope']:
                _error('scope_mismatch', 'Every recipe must have exactly the binding scope')
            keys.update(payload['values'])
        if len(keys) > 256:
            _error('request_too_large', 'A native recipe plan allows at most 256 keys')
        try:
            snapshot = await self.snapshot(binding, sorted(keys))
        except RecipeError as error:
            if error.kind != 'migration_required':
                raise
            checks = [{'ref': ref, 'status': 'migration_required', 'details': error.details} for ref in refs]
            return {'status': 'migration_required', 'checks': checks, 'conflicts': [], 'warnings': []}, None, entries
        env = await self.environment(binding, snapshot)
        checks, warnings, assignments = [], [], {}
        for ref, entry in zip(refs, entries):
            payload = entry['record']['payload']
            issues = []
            for key, signature in payload['native_contract']['definitions'].items():
                definition = normalize_definition(binding['scope'], key, snapshot['definitions'][key])
                if signature['native_type'] != definition['type'] or signature['sha256'] != digest(definition):
                    issues.append(key)
            conditions = self.compatibility(payload['compatibility'], env)
            status = 'migration_required' if issues or payload['native_contract']['protocol_version'] != 1 else (
                'incompatible' if any(c['status'] == 'incompatible' for c in conditions) else
                'unknown' if any(c['status'] == 'unknown' for c in conditions) else 'compatible')
            checks.append({'ref': ref, 'status': status, 'changed_definitions': issues, 'conditions': conditions,
                'archived': entry['archived'], 'newer_revision': entry['head'] if entry['head']['revision'] > ref['revision'] else None})
            warnings.extend(self.evidence_warnings(payload, env, ref))
            for key, value in payload['values'].items():
                assignments.setdefault(key, {}).setdefault(value, []).append(ref)
        conflicts = [{'key': key, 'values': list(choices)} for key, choices in sorted(assignments.items()) if len(choices) > 1]
        severity = {'compatible': 0, 'unknown': 1, 'incompatible': 2, 'migration_required': 3}
        status = max((c['status'] for c in checks), key=lambda s: severity[s])
        return {'status': status, 'checks': checks, 'conflicts': conflicts,
            'warnings': list({w['id']: w for w in warnings}.values())}, snapshot, entries

    async def plan(self, parameters, request_id):
        checked, snapshot, entries = await self.check(parameters)
        if checked['status'] != 'compatible':
            _error(checked['status'], 'Recipe compatibility must be confirmed before planning', checks=checked['checks'])
        if any(entry['archived'] for entry in entries):
            _error('archived_recipe', 'Unarchive source recipes before preparing a plan')
        assignments = {}
        for ref, entry in zip(parameters['recipes'], entries):
            for key, value in entry['record']['payload']['values'].items():
                assignments.setdefault(key, {}).setdefault(value, []).append(ref)
        conflicts = {key: choices for key, choices in assignments.items() if len(choices) > 1}
        resolutions = {}
        for resolution in parameters.get('resolutions', []):
            key, chosen = resolution['key'], resolution['chosen_value']
            if key in resolutions or key not in conflicts or chosen not in conflicts[key]:
                _error('invalid_resolution', 'Each resolution must uniquely choose an existing conflicting value', key=key)
            resolutions[key] = chosen
        if set(resolutions) != set(conflicts):
            _error('composition_conflict', 'Conflicting values require explicit resolutions', conflicts=checked['conflicts'])
        values = {key: resolutions.get(key, next(iter(choices))) for key, choices in sorted(assignments.items())}
        self.validate_values(values, snapshot['definitions'])
        binding = parameters['binding']
        native_id = str(uuid.uuid4())
        native_request = {'jsonrpc': '2.0', 'id': native_id, 'method': 'settings.patch',
            'params': {**scope_params(binding), 'target': binding['target'],
                       'state_token': binding['state_token'], 'values': values}}
        size = len(json.dumps(native_request, separators=(',', ':'), ensure_ascii=False).encode('utf-8'))
        if size > 65536:
            _error('request_too_large', 'The complete native request exceeds 65,536 bytes', native_request_bytes=size)
        warnings = checked['warnings']
        if binding['scope'] == 'process':
            warnings.append(_warning('project_process_impact', 'This changes the edited process settings for the project.'))
        elif binding['scope'] in ('object', 'volume', 'height_range'):
            warnings.append(_warning('linked_instances_impact', 'This scope affects all instances linked to the selected model object.'))
        diff = [{'key': key, 'before_effective': snapshot['values'][key]['value'],
            'before_own': snapshot['values'][key]['own_value'], 'after': value,
            'changes_ownership': snapshot['values'][key]['own_value'] is None,
            'sources': assignments[key][value]} for key, value in values.items()]
        plan = {'plan_id': str(uuid.uuid4()), 'library_id': self.store.describe()['library_id'],
            'created_at': utc_now(), 'expires_at': expires_at(), 'binding': binding,
            'recipes': parameters['recipes'], 'values': values,
            'schema_fingerprint': self.fingerprint(binding['scope'], snapshot['definitions']),
            'native_request_bytes': size, 'validation_level': 'schema_checked_native_validation_pending',
            'diff': diff, 'warnings': warnings, 'native_request_id': native_id}
        plan['plan_sha256'] = digest(plan)
        validate('Plan', plan)
        return self.store.save_plan(request_id, parameters, plan, native_request)

    async def apply(self, parameters, request_id):
        existing = self.store.existing_apply(request_id, parameters)
        if existing is not None:
            # Repeated apply is a receipt read. Explicit operation.get performs observation.
            return existing
        plan, native_request = self.store.get_plan(parameters['plan_id'], parameters['plan_sha256'])
        if expired(plan['expires_at']):
            _error('stale_plan', 'This unclaimed plan has expired')
        expected = {w['id'] for w in plan['warnings'] if w['acknowledgement_required']}
        supplied = parameters['accepted_warning_ids']
        available = {w['id'] for w in plan['warnings']}
        if len(set(supplied)) != len(supplied) or not set(supplied).issubset(available) or not expected.issubset(supplied):
            _error('warning_acknowledgement_required', 'Acknowledge exactly the known required warnings', required=sorted(expected))
        for ref in plan['recipes']:
            if self.store.get(ref)['archived']:
                _error('archived_recipe', 'An archived source recipe blocks this unclaimed plan')
        snapshot = await self.snapshot(plan['binding'], sorted(plan['values']))
        if self.fingerprint(plan['binding']['scope'], snapshot['definitions']) != plan['schema_fingerprint']:
            _error('stale_plan', 'The writable native schema changed since planning')
        no_op = all(snapshot['values'][key]['own_value'] == value for key, value in plan['values'].items())
        operation = {'operation_id': str(uuid.uuid4()), 'plan_id': plan['plan_id'], 'plan_sha256': plan['plan_sha256'],
            'status': 'succeeded' if no_op else 'dispatching', 'native_request_id': plan['native_request_id'],
            'native_job_id': None, 'native_state': None, 'verification': 'unchanged' if no_op else 'pending',
            'error': None, 'recovery': {'action': 'none', 'message': ''} if no_op else _recovery()}
        operation, new = self.store.claim_apply(request_id, parameters, operation)
        if not new or no_op:
            return operation
        operation_id = operation['operation_id']
        rpc_started = False
        try:
            try:
                selected = self.client.select(Target(**plan['binding']['target']))
                params = {key: value for key, value in native_request['params'].items() if key != 'target'}
                rpc_started = True
                ack = await self.client.rpc(selected, native_request['method'], params, native_request['id'])
                job_id = ack.get('job_id')
                if not isinstance(job_id, str) or not re.fullmatch(re.escape(plan['binding']['target']['window_id']) + r':[1-9][0-9]*', job_id):
                    raise ClientError('invalid_response', 'Native admission returned an invalid job ID')
                operation = self._update(operation_id, {'status': 'observing', 'native_job_id': job_id,
                    'native_state': 'queued', 'recovery': _recovery(True)})
            except ClientError as error:
                definite = not rpc_started or error.kind in (
                    'unauthorized', 'invalid_request', 'invalid_params', 'request_too_large',
                    'method_not_found', 'request_conflict', 'busy')
                return self._update(operation_id, {'status': 'failed' if definite else 'outcome_unknown',
                    'native_state': None if definite else 'unknown', 'verification': 'unchanged' if definite else 'unavailable',
                    'error': error.as_dict(), 'recovery': {'action': 'none', 'message': ''} if definite else _recovery()})
            return await self.observe(operation_id, 1)
        finally:
            # Cancellation and unexpected failures cannot leave a live writer's
            # claim stuck forever. Persist uncertainty synchronously, without a
            # second native attempt; a known job remains resumable observation.
            current, version, _, _ = self.store.operation_snapshot(operation_id)
            if current['status'] == 'dispatching' and current['native_job_id'] is None:
                self.store.update_operation(operation_id, version, {
                    'status': 'outcome_unknown', 'native_state': 'unknown',
                    'verification': 'unavailable', 'recovery': _recovery(),
                    'error': {'kind': 'dispatch_interrupted',
                              'message': 'Dispatch ended before an admission receipt was durably saved.'}})

    def _update(self, operation_id, updates):
        _, version, _, _ = self.store.operation_snapshot(operation_id)
        return self.store.update_operation(operation_id, version, updates)

    async def observe(self, operation_id, wait_seconds=0):
        operation, version, plan, _ = self.store.operation_snapshot(operation_id)
        if operation['native_job_id'] is None:
            # Store lifetime locks distinguish a dead dispatcher from another
            # live MCP process. An observer never guesses from local state.
            return operation
        if operation['status'] in ('succeeded', 'failed'):
            return operation
        job = await self.client.job(Target(**plan['binding']['target']), operation['native_job_id'], wait_seconds)
        state = job.get('state', 'unknown')
        updates = {'native_state': state if state in ('queued', 'running', 'succeeded', 'failed', 'cancelled') else 'unknown'}
        if state in ('queued', 'running'):
            updates.update(status='observing', verification='pending', recovery=_recovery(True))
        elif state in ('succeeded', 'failed', 'cancelled'):
            token = job.get('result', {}).get('state_token') if state == 'succeeded' else plan['binding']['state_token']
            try:
                if not token:
                    _error('observation_unavailable', 'Native completion did not supply a state token')
                rows = await self.values(plan['binding'], sorted(plan['values']), token)
                # Exact immutable revisions retain each key's checked native type;
                # do not infer types from current values or mutable recipe heads.
                types = {}
                for reference in plan['recipes']:
                    definitions = self.store.get(reference)['record']['payload']['native_contract']['definitions']
                    types.update({key: definition['native_type'] for key, definition in definitions.items()})
                matched = all(_same_value(value, rows[key]['own_value'], types[key]) and
                              _same_value(value, rows[key]['value'], types[key])
                              for key, value in plan['values'].items())
                unchanged = all(rows[row['key']]['own_value'] == row['before_own'] and
                    rows[row['key']]['value'] == row['before_effective'] for row in plan['diff'])
                if state == 'succeeded' and matched:
                    updates.update(status='succeeded', verification='matched', error=None,
                                   recovery={'action': 'none', 'message': ''})
                elif state in ('failed', 'cancelled') and unchanged:
                    updates.update(status='failed', verification='unchanged', error=_job_error(job),
                                   recovery={'action': 'none', 'message': ''})
                else:
                    updates.update(status='outcome_unknown', verification='not_matched', error=_job_error(job), recovery=_recovery(True))
            except RecipeError as error:
                updates.update(status='outcome_unknown', verification='superseded' if error.kind in ('stale_plan', 'stale_target') else 'unavailable',
                               error={'kind': error.kind, 'message': error.message}, recovery=_recovery(True))
        else:
            updates.update(status='outcome_unknown', verification='unavailable',
                           error=_job_error(job), recovery=_recovery(True))
        return self.store.update_operation(operation_id, version, updates)
