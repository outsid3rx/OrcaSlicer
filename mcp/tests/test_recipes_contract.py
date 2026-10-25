"""Portable canonical hashes and untrusted dump validation boundaries."""
import copy
import json
from pathlib import Path
import unittest

from orca_slicer_mcp.recipes_contract import (RecipeError, canonical, digest,
    parse_dump, validate, validate_record, validate_request, normalize_definition)


EXAMPLES = Path(__file__).resolve().parents[1] / 'docs/contracts/recipes-v1/examples'


class RecipeContractTests(unittest.TestCase):
    def setUp(self):
        self.record = json.loads((EXAMPLES / 'recipe.json').read_text())
        self.dump = json.loads((EXAMPLES / 'library-dump.json').read_text())

    def test_portable_example_digest_and_whitespace(self):
        validate_record(self.record)
        restored = parse_dump(json.dumps(self.dump, ensure_ascii=False, indent=4))
        self.assertEqual(restored['recipes'][0]['sha256'], self.record['sha256'])

    def test_rfc8785_numbers_and_utf16_key_order(self):
        self.assertEqual(canonical({'numbers': [333333333.33333329, 1e30, 4.50, 2e-3, 1e-27]}),
            b'{"numbers":[333333333.3333333,1e+30,4.5,0.002,1e-27]}')
        # Astral keys sort before a high BMP codepoint by UTF-16, unlike Python sort_keys.
        self.assertEqual(canonical({'\ue000': 1, '\U0001f600': 2}).decode(), '{"😀":2,"\ue000":1}')
        self.assertEqual(canonical(-0.0), b'0')

    def test_nonfinite_unsafe_integer_and_surrogates_fail_closed(self):
        for value in [float('nan'), float('inf'), 9007199254740992, '\ud800', {'\ud800': 'x'}]:
            with self.subTest(type=type(value).__name__), self.assertRaises(RecipeError):
                canonical(value)

    def test_dump_limits_duplicate_keys_and_parent_identity(self):
        for text in ['{"format": "orca.recipe-library", "format": "x"}', '{"a":NaN}', '[' * 26 + '0' + ']' * 26,
                     ' ' * (8 * 1024 * 1024 + 1)]:
            with self.subTest(length=len(text)), self.assertRaises(RecipeError):
                parse_dump(text)
        value = copy.deepcopy(self.dump)
        value['recipes'][0]['payload']['values']['enable_support'] = '0'
        with self.assertRaises(RecipeError) as raised:
            parse_dump(json.dumps(value))
        self.assertEqual(raised.exception.kind, 'digest_mismatch')

    def test_contract_key_set_and_cross_field_constraints(self):
        record = copy.deepcopy(self.record)
        record['payload']['native_contract']['definitions'].pop('enable_support')
        record['sha256'] = digest(record['payload'])
        with self.assertRaises(RecipeError):
            validate_record(record)
        args = json.loads((EXAMPLES / 'plan-request.json').read_text())
        validate_request(args['tool'], args['arguments'])
        args['arguments']['parameters']['binding']['volume_id'] = 'unexpected'
        with self.assertRaises(RecipeError):
            validate_request(args['tool'], args['arguments'])

    def test_raw_dump_request_preserves_duplicate_key_detection(self):
        request = {'dump': json.dumps(self.dump)}
        validate_request('orca_recipe_library', 'import.prepare', request, '40000000-0000-4000-8000-000000000001')
        with self.assertRaises(RecipeError):
            validate_request('orca_recipe_library', 'import.prepare', {'dump': self.dump}, '40000000-0000-4000-8000-000000000001')

    def test_definition_signature_ignores_localized_labels_only(self):
        one = {'type': 2, 'min': 1, 'max': 10, 'label': 'Wall loops', 'category': 'Strength', 'units': ''}
        two = {**one, 'label': 'Периметры', 'category': 'Прочность'}
        self.assertEqual(digest(normalize_definition('object', 'wall_loops', one)), digest(normalize_definition('object', 'wall_loops', two)))
        two['max'] = 11
        self.assertNotEqual(digest(normalize_definition('object', 'wall_loops', one)), digest(normalize_definition('object', 'wall_loops', two)))

    def test_filtered_discovery_and_public_errors_match_contract(self):
        from jsonschema import Draft202012Validator
        from orca_slicer_mcp.recipes_contract import request_schema, public_error
        schema = request_schema('orca_recipe_apply', 'plan')
        request = json.loads((EXAMPLES / 'plan-request.json').read_text())
        Draft202012Validator.check_schema(schema)
        Draft202012Validator(schema).validate(request)
        self.assertEqual(len(schema['oneOf']), 1)
        self.assertNotIn('Dump', schema['$defs'])
        validate_request('orca_recipes', 'describe', {'tool': 'orca_recipe_apply', 'action': 'plan'})
        for kind in ['invalid_value', 'transport_error', 'busy', 'request_too_large']:
            validate('Error', public_error(RecipeError(kind, 'test')))
        with self.assertRaises(RecipeError):
            request_schema('orca_recipes', 'missing')

    def test_unknown_outcome_schema_requires_recovery(self):
        op = {'operation_id': '50000000-0000-4000-8000-000000000001',
            'plan_id': '50000000-0000-4000-8000-000000000002', 'plan_sha256': 'a' * 64,
            'status': 'outcome_unknown', 'native_request_id': '50000000-0000-4000-8000-000000000003',
            'native_job_id': None, 'native_state': 'unknown', 'verification': 'unavailable', 'error': None,
            'recovery': {'action': 'none', 'message': ''}}
        with self.assertRaises(RecipeError):
            validate('Operation', op)
        op['recovery'] = {'action': 'inspect_scene_do_not_resend', 'message': 'Inspect without resending'}
        validate('Operation', op)


if __name__ == '__main__':
    unittest.main()
