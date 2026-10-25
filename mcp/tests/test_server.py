"""Validate adapter boundary rules independently of native geometry/settings."""
import base64
import unittest
from orca_slicer_mcp.server import scope_params, tool_result


class ServerBoundaryTests(unittest.TestCase):
    def test_override_selectors_preserve_the_native_scope(self):
        self.assertEqual(scope_params('volume', 'obj', volume_id='part'),
                         {'scope': 'volume', 'object_id': 'obj', 'volume_id': 'part'})
        self.assertEqual(scope_params('height_range', 'obj', height_range=[2, 4]),
                         {'scope': 'height_range', 'object_id': 'obj', 'range': [2, 4]})
        self.assertEqual(scope_params('plate', None, plate_id='plate:1:2'),
                         {'scope': 'plate', 'plate_id': 'plate:1:2'})
        for args in [('plate', None), ('process', 'obj'), ('volume', 'obj'), ('object', None)]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                scope_params(*args)

    def test_native_preview_becomes_an_image_without_duplicate_payload(self):
        encoded = base64.b64encode(b'\x89PNG\r\n\x1a\nfixture').decode()
        value = {'state': 'succeeded', 'result': {'mime_type': 'image/png', 'data': encoded}}
        result = tool_result(value)
        self.assertFalse(result.isError)
        self.assertEqual(result.content[1].data, encoded)
        self.assertNotIn('data', result.structuredContent['result'])
        self.assertIn('data', value['result'])  # Never mutate a cached native receipt.

    def test_invalid_and_oversized_native_previews_fail_closed(self):
        for encoded in ['%', base64.b64encode(b'not PNG').decode(), 'A' * 2800001]:
            with self.subTest(length=len(encoded)):
                result = tool_result({'state': 'succeeded', 'result': {'mime_type': 'image/png', 'data': encoded}})
                self.assertTrue(result.isError)
                self.assertEqual(result.structuredContent['error']['kind'], 'invalid_preview')
                self.assertEqual(len(result.content), 1)

    def test_uncertain_durable_receipts_remain_intact_and_cannot_look_successful(self):
        outcomes = [
            {'outcome': {'state': 'outcome_unknown', 'native_code': -1}},
            {'journal': {'intent': {}, 'outcome': {'state': 'outcome_unknown'}}},
            {'outcome': {'state': 'already_consumed', 'journal': {'intent': {}, 'outcome': None}}},
            {'outcome': {'intent': {}, 'outcome': {'state': 'outcome_unknown'}}},
            {'journal': {'intent': {}}}, {'journal': None}, {},
            {'outcome': {'state': ['corrupt']}},
        ]
        for receipt in outcomes:
            with self.subTest(receipt=receipt):
                native = {'state': 'succeeded', 'result': {'operation_id': 'op', **receipt}}
                converted = tool_result(native)
                self.assertTrue(converted.isError)
                self.assertEqual(converted.structuredContent['state'], 'succeeded')
                self.assertTrue(converted.structuredContent['outcome_uncertain'])
                self.assertEqual(converted.structuredContent['operation_outcome'], 'outcome_unknown')
                self.assertEqual(converted.structuredContent['result'], native['result'])
                self.assertNotIn('outcome_uncertain', native)
                self.assertIn('never automatically resubmit', converted.structuredContent['recovery'])

    def test_known_receipts_and_unsubmitted_plans_are_not_uncertain(self):
        for state in ['uploaded', 'submission_accepted', 'command_accepted']:
            for receipt in [{'outcome': {'state': state}},
                            {'outcome': {'state': 'already_consumed', 'journal': {'outcome': {'state': state}}}},
                            {'journal': {'intent': {}, 'outcome': {'state': state}}}]:
                converted = tool_result({'state': 'succeeded', 'result': {'operation_id': 'op', **receipt}})
                self.assertFalse(converted.isError)
                self.assertFalse(converted.structuredContent['outcome_uncertain'])
                self.assertEqual(converted.structuredContent['operation_outcome'], state)
        for result in [{'operation_id': 'op', 'plan_id': 'op'}, {'objects': [{'state': 'outcome_unknown'}]}]:
            converted = tool_result({'state': 'succeeded', 'result': result})
            self.assertFalse(converted.isError)
            self.assertNotIn('outcome_uncertain', converted.structuredContent)
