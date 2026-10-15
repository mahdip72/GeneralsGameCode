"""Counterexamples for actual workflow structure and actual required-check code."""
import copy
import itertools
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PRWorkflowPolicy import (LANES, OUTPUTS, PolicyError, evaluate_readiness, load_yaml,
                               validate_ctest_inventory, validate_policy)


ROOT = Path(os.environ.get('WORKFLOW_POLICY_SOURCE_ROOT', Path(__file__).resolve().parents[2]))


def readiness(**selected):
    outputs = {key: 'true' if selected.get(key, False) else 'false' for key in OUTPUTS}
    return {
        'detect-changes': {'result': 'success', 'outputs': outputs},
        'workflow-policy': {'result': 'success'},
        'native-generals': {'result': 'success' if selected.get('generals') or selected.get('shared') else 'skipped'},
        'native-zerohour': {'result': 'success' if selected.get('zerohour') or selected.get('shared') else 'skipped'},
        'rendering-tools': {'result': 'success' if selected.get('rendering_tools') else 'skipped'},
    }


class ReadinessTests(unittest.TestCase):
    def test_every_legitimate_path_combination(self):
        for flags in itertools.product((False, True), repeat=4):
            with self.subTest(flags=flags):
                evaluate_readiness(readiness(**dict(zip(OUTPUTS, flags))))

    def test_failed_cancelled_unknown_and_unexpected_skips(self):
        for selected in (False, True):
            for lane in ('detect-changes', 'workflow-policy') + LANES:
                for result in ('failure', 'cancelled', 'unknown', '', None):
                    fixture = readiness(**dict.fromkeys(OUTPUTS, selected))
                    fixture[lane]['result'] = result
                    with self.subTest(selected=selected, lane=lane, result=result):
                        with self.assertRaises(PolicyError):
                            evaluate_readiness(fixture)
        for lane in LANES:
            fixture = readiness(shared=True, rendering_tools=True)
            fixture[lane]['result'] = 'skipped'
            with self.assertRaises(PolicyError):
                evaluate_readiness(fixture)

    def test_missing_unknown_boolean_outputs_and_dependencies(self):
        for output in OUTPUTS:
            for value in (None, True, 'TRUE', 'unknown'):
                fixture = readiness()
                fixture['detect-changes']['outputs'][output] = value
                with self.assertRaises(PolicyError):
                    evaluate_readiness(fixture)
        fixture = readiness()
        del fixture['native-generals']
        with self.assertRaises(PolicyError):
            evaluate_readiness(fixture)

    def test_real_cli_exits_nonzero_for_failed_lane(self):
        for fixture, expected in ((readiness(), 0), (readiness(shared=True), 0)):
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('PRWorkflowPolicy.py')), '--readiness'],
                                    env={**os.environ, 'NEEDS_JSON': json.dumps(fixture)}, capture_output=True)
            self.assertEqual(result.returncode, expected, result.stderr)
        fixture['native-zerohour']['result'] = 'cancelled'
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('PRWorkflowPolicy.py')), '--readiness'],
                                env={**os.environ, 'NEEDS_JSON': json.dumps(fixture)}, capture_output=True)
        self.assertNotEqual(result.returncode, 0)


class CTestInventoryTests(unittest.TestCase):
    pattern = '^(core_threaded_render_device_tests|core_native_w3d_render_state_tests)$'

    def inventory(self):
        return {'tests': [
            {'name': name, 'properties': []}
            for name in (
                'core_threaded_render_device_tests',
                'core_native_w3d_render_state_tests',
                'unrelated_test',
            )
        ]}

    def test_configured_inventory_accepts_each_exact_focused_registration(self):
        validate_ctest_inventory(self.inventory(), self.pattern)

    def test_general_manual_regex_checks_each_configured_match(self):
        validate_ctest_inventory(
            self.inventory(),
            '^core_(threaded_render_device_tests|native_w3d_render_state_tests)$')

    def test_inventory_cli_accepts_configured_json(self):
        with tempfile.TemporaryDirectory() as directory:
            inventory_path = Path(directory) / 'ctest-inventory.json'
            inventory_path.write_text(json.dumps(self.inventory()), encoding='utf-8')
            result = subprocess.run([
                sys.executable, str(Path(__file__).with_name('PRWorkflowPolicy.py')),
                '--ctest-inventory', str(inventory_path),
                '--focused-test-regex', self.pattern,
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_commenting_out_registration_removes_it_from_configured_inventory(self):
        inventory = self.inventory()
        inventory['tests'] = [test for test in inventory['tests']
                              if test['name'] != 'core_threaded_render_device_tests']
        with self.assertRaisesRegex(PolicyError, 'missing from the configured CTest inventory'):
            validate_ctest_inventory(inventory, self.pattern)

    def test_disabled_registration_is_rejected(self):
        inventory = self.inventory()
        next(test for test in inventory['tests']
             if test['name'] == 'core_threaded_render_device_tests')['properties'].append(
                 {'name': 'DISABLED', 'value': True})
        with self.assertRaisesRegex(PolicyError, 'is disabled in the configured inventory'):
            validate_ctest_inventory(inventory, self.pattern)

    def test_will_fail_registration_is_rejected(self):
        inventory = self.inventory()
        next(test for test in inventory['tests']
             if test['name'] == 'core_threaded_render_device_tests')['properties'].append(
                 {'name': 'WILL_FAIL', 'value': True})
        with self.assertRaisesRegex(PolicyError, 'can mask a nonzero exit by WILL_FAIL'):
            validate_ctest_inventory(inventory, self.pattern)

    def test_nonempty_pass_regex_registration_is_rejected(self):
        inventory = self.inventory()
        next(test for test in inventory['tests']
             if test['name'] == 'core_native_w3d_render_state_tests')['properties'].append(
                 {'name': 'PASS_REGULAR_EXPRESSION', 'value': ['ready', 'done']})
        with self.assertRaisesRegex(PolicyError, 'can mask a nonzero exit by PASS_REGULAR_EXPRESSION'):
            validate_ctest_inventory(inventory, self.pattern)

    def test_empty_pass_regex_and_false_will_fail_remain_nonmasking(self):
        inventory = self.inventory()
        next(test for test in inventory['tests']
             if test['name'] == 'core_threaded_render_device_tests')['properties'].extend((
                 {'name': 'WILL_FAIL', 'value': False},
                 {'name': 'PASS_REGULAR_EXPRESSION', 'value': []},
             ))
        validate_ctest_inventory(inventory, self.pattern)

    def test_both_skip_capable_registration_properties_are_rejected(self):
        for property_name, value in (('SKIP_RETURN_CODE', 77),
                                     ('SKIP_REGULAR_EXPRESSION', ['not available'])):
            inventory = self.inventory()
            next(test for test in inventory['tests']
                 if test['name'] == 'core_native_w3d_render_state_tests')['properties'].append(
                     {'name': property_name, 'value': value})
            with self.subTest(property_name=property_name):
                with self.assertRaisesRegex(PolicyError, f'can be skipped by {property_name}'):
                    validate_ctest_inventory(inventory, self.pattern)


class WorkflowMutationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = ROOT / '.github/workflows'
        cls.pr = load_yaml((directory / 'pr-ci.yml').read_text(encoding='utf-8'))
        cls.extended = load_yaml((directory / 'ci.yml').read_text(encoding='utf-8'))
        cls.reusable = load_yaml((directory / 'build-toolchain.yml').read_text(encoding='utf-8'))

    def test_live_workflows(self):
        validate_policy(self.pr, self.extended, ROOT)

    def reject(self, mutate, expected):
        pr, extended = copy.deepcopy(self.pr), copy.deepcopy(self.extended)
        mutate(pr, extended)
        with self.assertRaisesRegex(PolicyError, expected):
            validate_policy(pr, extended, ROOT)

    def reject_reusable(self, mutate, expected):
        reusable = copy.deepcopy(self.reusable)
        mutate(reusable)
        with self.assertRaisesRegex(PolicyError, expected):
            validate_policy(copy.deepcopy(self.pr), copy.deepcopy(self.extended), ROOT, reusable)

    def test_duplicate_yaml_keys_fail(self):
        with self.assertRaisesRegex(PolicyError, 'duplicate'):
            load_yaml('on:\n  pull_request:\non:\n  workflow_dispatch:\n')

    def test_realistic_workflow_regressions(self):
        mutations = (
            (lambda p, e: p['on'].update(pull_request={'paths': ['Core/**']}), 'PR trigger'),
            (lambda p, e: e['on'].update(pull_request=None), 'manual'),
            (lambda p, e: p['concurrency'].update(group='${{ github.workflow }}'), 'concurrency'),
            (lambda p, e: p['jobs']['native-zerohour'].update(needs=['detect-changes', 'native-generals']), 'depend only'),
            (lambda p, e: p['jobs']['native-zerohour']['with'].update(preset='x64-asan-core'), 'Release'),
            (lambda p, e: p['jobs']['native-generals']['with'].update(focused_test_regex='core_'), 'anchored'),
            (lambda p, e: p['jobs']['native-generals']['with'].update(focused_test_regex=p['jobs']['native-generals']['with']['focused_test_regex'].replace('g_xfer_crc_snapshot_tests', 'g_not_registered_tests')), 'exact required 17 smoke tests'),
            (lambda p, e: p['jobs']['native-generals']['with'].update(focused_targets=p['jobs']['native-generals']['with']['focused_targets'].replace(',core_task_runtime_tests', '')), 'exact required 14 build targets'),
            (lambda p, e: p['jobs']['pr-readiness'].update(**{'if': '${{ success() }}'}), 'always'),
            (lambda p, e: p['jobs']['pr-readiness'].update(name='Optional smoke'), 'stable'),
            (lambda p, e: p['jobs']['pr-readiness']['needs'].remove('native-zerohour'), 'consume every'),
            (lambda p, e: p['jobs']['pr-readiness']['steps'][-1]['env'].update(NEEDS_JSON='{}'), 'actual needs'),
            (lambda p, e: p['jobs']['pr-readiness']['steps'][-1].update(run='python3 .github/workflows/PRWorkflowPolicy.py --readiness || true'), 'failure masking'),
            (lambda p, e: p['jobs']['pr-readiness']['steps'][-1].update(**{'if': '${{ false }}'}), 'bypass'),
            (lambda p, e: p['jobs']['native-generals'].update(**{'continue-on-error': True}), 'ignored'),
            (lambda p, e: e['jobs'].pop('stage5-combined-native-path-modes'), 'removed'),
        )
        for mutation, message in mutations:
            with self.subTest(message=message):
                self.reject(mutation, message)

    def test_shared_and_title_path_detection_cannot_be_bypassed(self):
        for path in ('Core/**', 'resources/**', 'Generals/**', 'GeneralsMD/**', 'vcpkg.json', 'vcpkg-lock.json'):
            def remove_path(p, e):
                step = next(s for s in p['jobs']['detect-changes']['steps'] if s.get('id') == 'filter')
                step['with']['filters'] = step['with']['filters'].replace("- '" + path + "'", "- 'docs/**'")
            self.reject(remove_path, 'path')

    def test_paired_smoke_test_and_target_removal_cannot_reduce_coverage(self):
        for lane in ('native-generals', 'native-zerohour'):
            def remove_pair(p, e):
                settings = p['jobs'][lane]['with']
                settings['focused_test_regex'] = settings['focused_test_regex'].replace('|core_task_runtime_tests', '')
                settings['focused_targets'] = settings['focused_targets'].replace(',core_task_runtime_tests', '')
            self.reject(remove_pair, 'exact required 17 smoke tests')

    def test_smoke_test_and_target_duplicates_are_rejected(self):
        for lane in ('native-generals', 'native-zerohour'):
            def duplicate_test(p, e):
                settings = p['jobs'][lane]['with']
                settings['focused_test_regex'] = settings['focused_test_regex'].replace(')$', '|core_task_runtime_tests)$')
            self.reject(duplicate_test, 'smoke suite must be distinct')
            def duplicate_target(p, e):
                p['jobs'][lane]['with']['focused_targets'] += ',core_task_runtime_tests'
            self.reject(duplicate_target, 'build targets must be distinct')

    def test_retained_authoring_matrix_requires_modern_and_vc6_per_title(self):
        for job in ('build-generals', 'build-generalsmd-win32'):
            self.reject(lambda p, e: e['jobs'][job]['strategy']['matrix']['include'].pop(1),
                        'authoring matrix')
        self.reject(lambda p, e: e['jobs']['build-generalsmd-win32']['with'].update(game='Generals'),
                    'GeneralsMD authoring job must pass its game')

    def test_retained_authoring_jobs_require_activation_dependency_and_reusable_routing(self):
        for job in ('build-generals', 'build-generalsmd-win32'):
            with self.subTest(job=job, mutation='disabled activation'):
                self.reject(lambda p, e: e['jobs'][job].update(**{'if': '${{ false }}'}),
                            'authoring job activation condition')
            with self.subTest(job=job, mutation='wrong dependency'):
                self.reject(lambda p, e: e['jobs'][job].update(needs='workflow_dispatch'),
                            'exact change-detection dependency')
            with self.subTest(job=job, mutation='retargeted reusable workflow'):
                self.reject(lambda p, e: e['jobs'][job].update(uses='./.github/workflows/other.yml'),
                            'route to the retained reusable build workflow')

    def test_focused_ctest_inventory_gate_cannot_be_removed_or_masked(self):
        self.reject_reusable(
            lambda r: r['jobs']['build']['steps'].remove(next(
                step for step in r['jobs']['build']['steps']
                if step.get('name') == 'Verify focused CTest inventory')),
            'configured CTest inventory guard')
        self.reject_reusable(
            lambda r: next(step for step in r['jobs']['build']['steps']
                           if step.get('name') == 'Verify focused CTest inventory').update(
                               **{'if': 'inputs.extras'}),
            'only for focused test lanes')
        self.reject_reusable(
            lambda r: next(step for step in r['jobs']['build']['steps']
                           if step.get('name') == 'Verify focused CTest inventory').update(
                               **{'continue-on-error': True}),
            'fail the build lane')

    def test_focused_ctest_gate_captures_exit_before_policy_cli(self):
        def move_capture_after_cli(reusable):
            step = next(step for step in reusable['jobs']['build']['steps']
                        if step.get('name') == 'Verify focused CTest inventory')
            lines = step['run'].splitlines()
            capture = '$ctestInventoryExitCode = $LASTEXITCODE'
            lines.remove(capture)
            cli = next(index for index, line in enumerate(lines)
                       if 'PRWorkflowPolicy.py --ctest-inventory' in line)
            lines.insert(cli + 1, capture)
            step['run'] = '\n'.join(lines)
        self.reject_reusable(move_capture_after_cli, 'exact fail-closed command sequence')

    def test_focused_ctest_gate_rejects_success_exit_before_enumeration(self):
        def insert_exit(reusable):
            step = next(step for step in reusable['jobs']['build']['steps']
                        if step.get('name') == 'Verify focused CTest inventory')
            lines = step['run'].splitlines()
            ctest = next(index for index, line in enumerate(lines)
                         if line.startswith('$inventoryJson = & ctest '))
            lines.insert(ctest, 'exit 0')
            step['run'] = '\n'.join(lines)
        self.reject_reusable(insert_exit, 'exact fail-closed command sequence')

    def test_focused_ctest_gate_rejects_success_exit_after_policy_cli(self):
        def insert_exit(reusable):
            step = next(step for step in reusable['jobs']['build']['steps']
                        if step.get('name') == 'Verify focused CTest inventory')
            lines = step['run'].splitlines()
            cli = next(index for index, line in enumerate(lines)
                       if 'PRWorkflowPolicy.py --ctest-inventory' in line)
            lines.insert(cli + 1, 'exit 0')
            step['run'] = '\n'.join(lines)
        self.reject_reusable(insert_exit, 'exact fail-closed command sequence')

    def test_focused_ctest_gate_rejects_exit_status_reset_after_policy_cli(self):
        def reset_status(reusable):
            step = next(step for step in reusable['jobs']['build']['steps']
                        if step.get('name') == 'Verify focused CTest inventory')
            lines = step['run'].splitlines()
            cli = next(index for index, line in enumerate(lines)
                       if 'PRWorkflowPolicy.py --ctest-inventory' in line)
            lines.insert(cli + 1, '$LASTEXITCODE = 0')
            step['run'] = '\n'.join(lines)
        self.reject_reusable(reset_status, 'exact fail-closed command sequence')

    def test_extended_focused_runtime_filter_cannot_drop_a_test_family(self):
        reduced_filters = (
            '^core_job_system_tests$',
            '^core_(deterministic_simulation(_.*)?test|job_system_tests)$',
        )
        for pattern in reduced_filters:
            with self.subTest(pattern=pattern):
                self.reject(
                    lambda p, e: e['jobs']['build-native-x64-focused-runtime']['with'].update(
                        focused_test_regex=pattern),
                    'extended x64 focused runtime must retain its exact test filter')

    def test_mandatory_policy_steps_cannot_be_disabled_or_masked(self):
        for index in (1, 2, 3):
            self.reject(lambda p, e: p['jobs']['workflow-policy']['steps'][index].update(**{'if': '${{ false }}'}),
                        'unconditionally')
            def mask_failure(p, e):
                p['jobs']['workflow-policy']['steps'][index]['run'] += ' || true'
            self.reject(mask_failure, 'failure masking')

    def test_manual_matrix_permissions_and_input_defaults_remain_preserved(self):
        self.reject(lambda p, e: e['permissions'].update(**{'pull-requests': 'write'}), 'read-only')
        for job in ('stage5-replaycheck-generalsmd-x64', 'stage5-replaycheck-generals-x64'):
            self.reject(lambda p, e: e['jobs'][job]['permissions'].pop('pull-requests'), 'permission contract')
            for artifact in ('build-generals-x64', 'build-generalsmd-x64'):
                self.reject(lambda p, e: e['jobs'][job]['needs'].remove(artifact), 'both downloaded title artifacts')
        for index in range(3):
            self.reject(lambda p, e: e['jobs']['build-native-x64-focused-runtime']['strategy']['matrix']['include'].pop(index),
                        'three presets')
        inputs = self.extended['on']['workflow_dispatch']['inputs']
        for name, spec in inputs.items():
            def enable_default(p, e):
                e['on']['workflow_dispatch']['inputs'][name]['default'] = True if spec['type'] == 'boolean' else 'enabled'
            self.reject(enable_default, 'empty/false default')

    def test_manual_qualification_guards_cannot_be_bypassed(self):
        for job in ('stage5-replaycheck-generalsmd-x64', 'stage5-replaycheck-generals-x64',
                    'stage5-lockstep-v2-qualification', 'stage5-performance-scaling-qualification'):
            def bypass_guard(p, e):
                original = e['jobs'][job]['if']
                e['jobs'][job]['if'] = original.replace('&&', '||', 1)
                self.assertNotEqual(e['jobs'][job]['if'], original)
            self.reject(bypass_guard, 'opt-in condition')
        self.reject(lambda p, e: e['jobs']['stage5-performance-scaling-qualification'].update(**{'runs-on': 'windows-2022'}),
                    'dedicated runner')


if __name__ == '__main__':
    unittest.main()
