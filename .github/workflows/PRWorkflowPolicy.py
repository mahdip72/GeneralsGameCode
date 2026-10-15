"""Focused ordinary-PR policy and the required check's fail-closed evaluator.

Full Stage 5 provenance contracts remain in the manually dispatched GenCI job.
PyYAML is needed only for policy validation, not for the readiness evaluator.
"""
import argparse
import json
import os
from pathlib import Path
import re


class PolicyError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PolicyError(message)


def load_yaml(text):
    import yaml

    class WorkflowLoader(yaml.SafeLoader):
        pass

    # GitHub uses YAML 1.2: 'on' is a mapping key, not YAML 1.1's True.
    WorkflowLoader.yaml_implicit_resolvers = {
        key: [(tag, pattern) for tag, pattern in rules
              if tag != 'tag:yaml.org,2002:bool']
        for key, rules in WorkflowLoader.yaml_implicit_resolvers.items()
    }
    WorkflowLoader.add_implicit_resolver('tag:yaml.org,2002:bool',
                                        re.compile(r'^(true|false)$'), list('tf'))

    def mapping(loader, node, deep=False):
        result = {}
        for key_node, value_node in node.value:
            key = loader.construct_object(key_node, deep=deep)
            require(key not in result, f'duplicate YAML key: {key}')
            result[key] = loader.construct_object(value_node, deep=deep)
        return result

    WorkflowLoader.add_constructor('tag:yaml.org,2002:map', mapping)
    return yaml.load(text, Loader=WorkflowLoader)


LANES = ('native-generals', 'native-zerohour', 'rendering-tools')
REQUIRED = ('detect-changes', 'workflow-policy')
OUTPUTS = ('generals', 'zerohour', 'shared', 'rendering_tools')
EXTENDED_FOCUSED_RUNTIME_REGEX = (
    r'^core_(deterministic_simulation(_.*)?test|job_system_tests|'
    r'pipeline_execution_policy_(parser|owner|compute|race)_test)$'
)
CORE_SMOKE_TESTS = frozenset((
    'core_threaded_render_device_tests',
    'core_threaded_render_device_native_tests',
    'core_threaded_render_pipeline_stall_trace_tests',
    'core_native_w3d_owner_queue_tests',
    'core_native_w3d_renderer_tests',
    'core_native_w3d_render_state_tests',
    'core_rigid_instancing_policy_tests',
    'core_game_rigid_draw_batch_tests',
    'core_native_mesh_task_lifetime_tests',
    'core_task_runtime_tests',
    'core_chunkio_value_read_tests',
    'core_replay_field_reader_tests',
    'core_deterministic_crc_runtime_adapter_tests',
))
CORE_SMOKE_TARGETS = frozenset((
    'core_threaded_render_device_tests',
    'core_threaded_render_device_native_tests',
    'core_native_w3d_owner_queue_tests',
    'core_native_w3d_renderer_tests',
    'core_native_w3d_render_state_tests',
    'core_rigid_instancing_policy_tests',
    'core_game_rigid_draw_batch_tests',
    'core_native_mesh_task_lifetime_tests',
    'core_task_runtime_tests',
    'core_chunkio_value_read_tests',
    'core_replay_field_reader_tests',
    'core_crc_runtime_tests',
))


def required_smoke_tests(prefix):
    return CORE_SMOKE_TESTS | frozenset(prefix + suffix for suffix in (
        '_skirmish_ai_runner_contract_tests', '_xfer_crc_snapshot_tests',
        '_texture_load_queue_contract_tests', '_skirmish_ai_replay_epoch_tests',
    ))


def required_smoke_targets(prefix):
    harness = 'g_skirmish_ai_runner_contract_tests' if prefix == 'g' else 'z_runtime_regression_tests'
    return CORE_SMOKE_TARGETS | {prefix + '_generals', harness}


def focused_test_names(pattern):
    require(isinstance(pattern, str) and pattern.startswith('^(') and pattern.endswith(')$'),
            'CTest filter must be anchored')
    names = pattern[2:-2].split('|')
    require(names and all(re.fullmatch(r'[a-z0-9_]+', name) is not None for name in names),
            'smoke filter must enumerate exact test names')
    require(len(set(names)) == len(names), 'smoke suite must be distinct')
    return names


def _ctest_property_is_true(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() not in ('', '0', 'false', 'off', 'no')
    return bool(value)


def _ctest_property_is_nonempty(value):
    if value is None:
        return False
    if isinstance(value, str):
        return value != ''
    if isinstance(value, (list, tuple, dict, set)):
        return len(value) > 0
    return True


def validate_ctest_inventory(document, pattern):
    require(isinstance(pattern, str) and pattern.startswith('^') and pattern.endswith('$'),
            'focused CTest regex must be anchored')
    try:
        re.compile(pattern)
    except re.error as error:
        raise PolicyError(f'invalid focused CTest regex: {error}') from error
    exact_names = None
    if pattern.startswith('^(') and pattern.endswith(')$'):
        alternatives = pattern[2:-2].split('|')
        if alternatives and all(re.fullmatch(r'[a-z0-9_]+', name) is not None for name in alternatives):
            exact_names = focused_test_names(pattern)
    require(isinstance(document, dict), 'configured CTest inventory must be a JSON object')
    tests = document.get('tests')
    require(isinstance(tests, list), 'configured CTest inventory must contain a tests array')
    by_name = {}
    for test in tests:
        require(isinstance(test, dict) and isinstance(test.get('name'), str),
                'configured CTest inventory contains a malformed test entry')
        name = test['name']
        require(name not in by_name, f'duplicate test in configured CTest inventory: {name}')
        properties = test.get('properties', [])
        require(isinstance(properties, list), f'CTest properties are malformed for {name}')
        property_map = {}
        for prop in properties:
            require(isinstance(prop, dict) and isinstance(prop.get('name'), str),
                    f'CTest property is malformed for {name}')
            property_name = prop['name']
            require(property_name not in property_map,
                    f'duplicate CTest property {property_name} for {name}')
            property_map[property_name] = prop.get('value')
        by_name[name] = property_map

    registered = {name for name in by_name if re.fullmatch(pattern, name)}
    if exact_names is not None:
        expected = set(exact_names)
        missing = sorted(expected - registered)
        require(not missing,
                'focused tests are missing from the configured CTest inventory (not registered): ' +
                ', '.join(missing))
        unexpected = sorted(registered - expected)
        require(not unexpected,
                'configured CTest inventory has unexpected focused tests: ' + ', '.join(unexpected))
        names = exact_names
    else:
        require(registered, 'focused CTest regex matched no tests in the configured inventory')
        names = sorted(registered)

    for name in names:
        properties = by_name[name]
        require(not _ctest_property_is_true(properties.get('DISABLED', False)),
                f'focused CTest is disabled in the configured inventory: {name}')
        require(not _ctest_property_is_true(properties.get('WILL_FAIL', False)),
                f'focused CTest can mask a nonzero exit by WILL_FAIL: {name}')
        require(not _ctest_property_is_nonempty(properties.get('PASS_REGULAR_EXPRESSION')),
                f'focused CTest can mask a nonzero exit by PASS_REGULAR_EXPRESSION: {name}')
        for skip_property in ('SKIP_RETURN_CODE', 'SKIP_REGULAR_EXPRESSION'):
            value = properties.get(skip_property)
            if _ctest_property_is_nonempty(value):
                raise PolicyError(f'focused CTest can be skipped by {skip_property}: {name}')


def validate_retained_authoring_matrices(extended):
    contracts = (
        ('build-generals', 'Generals', 'win32-generals-authoring'),
        ('build-generalsmd-win32', 'GeneralsMD', 'win32-zerohour-authoring'),
    )
    jobs = extended.get('jobs', {})
    for job_name, title, modern_preset in contracts:
        job = jobs.get(job_name)
        require(isinstance(job, dict), f'{title} authoring job is missing')
        expected_conditions = {
            'build-generals': "${{ github.event_name == 'workflow_dispatch' || needs.detect-changes.outputs.generals == 'true' || needs.detect-changes.outputs.shared == 'true' }}",
            'build-generalsmd-win32': "${{ github.event_name == 'workflow_dispatch' || needs.detect-changes.outputs.generalsmd == 'true' || needs.detect-changes.outputs.shared == 'true' }}",
        }
        require(' '.join(job.get('if', '').split()) == expected_conditions[job_name],
                f'{title} authoring job activation condition changed')
        require(needs_list(job.get('needs', [])) == ['detect-changes'],
                f'{title} authoring job must retain its exact change-detection dependency')
        require(job.get('uses') == './.github/workflows/build-toolchain.yml',
                f'{title} authoring job must route to the retained reusable build workflow')
        expected_matrix = {'include': [
            {'preset': modern_preset, 'tools': True, 'extras': False, 'product': False},
            {'preset': 'vc6', 'tools': True, 'extras': False, 'product': False},
        ]}
        matrix = job.get('strategy', {}).get('matrix')
        require(matrix == expected_matrix,
                f'{title} authoring matrix must retain exactly modern Win32 and VC6 entries')
        expected_with = {
            'game': title,
            'preset': '${{ matrix.preset }}',
            'tools': '${{ matrix.tools }}',
            'extras': '${{ matrix.extras }}',
            'product': '${{ matrix.product }}',
        }
        settings = job.get('with', {})
        require(all(settings.get(key) == value for key, value in expected_with.items()),
                f'{title} authoring job must pass its game, extras, and product matrix settings')


def validate_focused_ctest_gate(reusable):
    steps = reusable.get('jobs', {}).get('build', {}).get('steps', [])
    gates = [(index, step) for index, step in enumerate(steps)
             if step.get('name') == 'Verify focused CTest inventory']
    ctest_runs = [(index, step) for index, step in enumerate(steps)
                  if (step.get('name') or '').startswith('Run Core extras tests ')]
    require(len(gates) == 1, 'exactly one configured CTest inventory guard is required')
    require(len(ctest_runs) == 1, 'exactly one Core extras CTest step is required')
    gate_index, gate = gates[0]
    ctest_index, ctest_step = ctest_runs[0]
    require(gate_index < ctest_index, 'configured CTest inventory guard must precede focused CTest')
    require(gate.get('if') == "inputs.focused_test_regex != ''" and gate.get('shell') == 'pwsh',
            'configured CTest inventory guard must run only for focused test lanes')
    require('continue-on-error' not in gate and 'continue-on-error' not in ctest_step,
            'configured CTest inventory failures must fail the build lane')
    run = gate.get('run', '')
    expected_run = (
        "$inventoryPath = Join-Path $env:RUNNER_TEMP 'focused-ctest-inventory.json'",
        "$configuration = if ('${{ inputs.preset }}' -match 'debug|asan') { 'Debug' } else { 'Release' }",
        '$inventoryJson = & ctest --test-dir "build\\${{ inputs.preset }}" --show-only=json-v1 -C $configuration',
        '$ctestInventoryExitCode = $LASTEXITCODE',
        'if ($ctestInventoryExitCode -ne 0) { exit $ctestInventoryExitCode }',
        '$inventoryJson | Set-Content -LiteralPath $inventoryPath -Encoding utf8',
        "py -3 .github/workflows/PRWorkflowPolicy.py --ctest-inventory $inventoryPath --focused-test-regex '${{ inputs.focused_test_regex }}'",
        'exit $LASTEXITCODE',
    )
    run_lines = tuple(line.strip() for line in run.splitlines() if line.strip())
    require(run_lines == expected_run,
            'configured CTest inventory gate must preserve the exact fail-closed command sequence')
    ctest_run = ctest_step.get('run', '')
    require("$configuration = if ('${{ inputs.preset }}' -match 'debug|asan') { 'Debug' } else { 'Release' }" in ctest_run and
            "$arguments += @('-C', $configuration)" in ctest_run and
            "$arguments += @('-R', '${{ inputs.focused_test_regex }}')" in ctest_run and
            '& ctest @arguments' in ctest_run,
            'focused CTest execution must use the guarded regex without failure masking')


def evaluate_readiness(needs):
    require(isinstance(needs, dict), 'needs must be an object')
    require(set(needs) == set(REQUIRED + LANES), 'missing or unexpected readiness dependency')
    for job in REQUIRED:
        require(needs[job].get('result') == 'success', f'{job} did not succeed')
    outputs = needs['detect-changes'].get('outputs', {})
    require(all(outputs.get(key) in ('true', 'false') for key in OUTPUTS),
            'change detection must produce explicit true/false outputs')
    expected = {
        'native-generals': outputs['generals'] == 'true' or outputs['shared'] == 'true',
        'native-zerohour': outputs['zerohour'] == 'true' or outputs['shared'] == 'true',
        'rendering-tools': outputs['rendering_tools'] == 'true',
    }
    for lane, selected in expected.items():
        result = needs[lane].get('result')
        require(result == ('success' if selected else 'skipped'),
                f'{lane}: expected {"success" if selected else "skipped"}, got {result!r}')


def needs_list(value):
    return [value] if isinstance(value, str) else value


def validate_manual_policy(extended, root):
    require(extended.get('permissions') == {'contents': 'read'}, 'manual workflow permissions must remain read-only')
    inputs = extended['on']['workflow_dispatch']['inputs']
    boolean_inputs = {'stage5_lockstep_v2_qualification', 'stage5_external_performance_qualification'}
    string_inputs = {
        'stage5_fixture_manifest', 'stage5_performance_baseline',
        'stage5_expected_stage3_executable_sha256', 'stage5_generals_fixture_manifest',
        'stage5_acceptance_manifest', 'stage5_lockstep_v2_map_name',
        'stage5_lockstep_v2_generals_map_crc', 'stage5_lockstep_v2_zerohour_map_crc',
        'stage5_performance_scaling_fixture_manifest',
        'stage5_expected_performance_scaling_fixture_manifest_sha256',
        'stage5_performance_phase_baseline_profile',
        'stage5_expected_performance_phase_baseline_profile_sha256',
        'stage5_expected_stage3_performance_baseline_sha256', 'stage5_expected_stage3_source_commit',
    }
    require(set(inputs) == boolean_inputs | string_inputs, 'manual Stage5 input schema changed')
    for name in boolean_inputs | string_inputs:
        spec = inputs[name]
        expected_type, expected_default = ('boolean', False) if name in boolean_inputs else ('string', '')
        require(spec.get('type') == expected_type and spec.get('required') is False and
                type(spec.get('default')) is type(expected_default) and spec.get('default') == expected_default,
                f'manual Stage5 input must remain opt-in with empty/false default: {name}')
    jobs = extended['jobs']
    replay_permissions = load_yaml((root / '.github/workflows/check-replays.yml').read_text(encoding='utf-8'))['permissions']
    for name in ('stage5-replaycheck-generalsmd-x64', 'stage5-replaycheck-generals-x64'):
        require(jobs[name].get('permissions') == replay_permissions,
                f'manual replay caller must match reusable permission contract: {name}')
        require(set(needs_list(jobs[name].get('needs', []))) ==
                {'detect-changes', 'build-generals-x64', 'build-generalsmd-x64', 'stage5-execution-cohort'},
                f'manual replay must wait for both downloaded title artifacts: {name}')
    focused_runtime = jobs['build-native-x64-focused-runtime']
    matrix = focused_runtime['strategy']['matrix']
    require(matrix == {'include': [{'preset': preset} for preset in
                                   ('x64-debug-core', 'x64-profile-core', 'x64-asan-core')]},
            'manual Debug/Profile/ASAN matrix must retain exactly three presets')
    require(focused_runtime.get('with', {}).get('focused_test_regex') ==
            EXTENDED_FOCUSED_RUNTIME_REGEX,
            'extended x64 focused runtime must retain its exact test filter')
    manual = "github.event_name == 'workflow_dispatch'"
    conditions = {
        'stage5-replaycheck-generalsmd-x64': [manual, "inputs.stage5_fixture_manifest != ''", "inputs.stage5_acceptance_manifest != ''"],
        'stage5-replaycheck-generals-x64': [manual, "inputs.stage5_generals_fixture_manifest != ''", "inputs.stage5_acceptance_manifest != ''"],
        'stage5-lockstep-v2-qualification': [manual, 'inputs.stage5_lockstep_v2_qualification == true'],
        'stage5-performance-scaling-qualification': [manual,
            'inputs.stage5_lockstep_v2_qualification == true',
            'inputs.stage5_external_performance_qualification == true'] + [
                f"inputs.{name} != ''" for name in (
                    'stage5_performance_scaling_fixture_manifest',
                    'stage5_expected_performance_scaling_fixture_manifest_sha256',
                    'stage5_performance_phase_baseline_profile',
                    'stage5_expected_performance_phase_baseline_profile_sha256',
                    'stage5_performance_baseline', 'stage5_expected_stage3_performance_baseline_sha256',
                    'stage5_expected_stage3_executable_sha256', 'stage5_expected_stage3_source_commit')],
    }
    for name, clauses in conditions.items():
        expected = '${{ ' + ' && '.join(clauses) + ' }}'
        require(' '.join(jobs[name].get('if', '').split()) == expected,
                f'manual qualification opt-in condition changed: {name}')
    require(jobs['stage5-performance-scaling-qualification'].get('runs-on') ==
            ['self-hosted', 'windows', 'x64', 'stage5-16-physical-core'],
            'external performance must retain its dedicated runner labels')


def validate_policy(pr, extended, root, reusable=None):
    require(pr.get('name') == 'PR CI', 'ordinary workflow must retain its identity')
    triggers = pr.get('on', {})
    require(set(triggers) == {'pull_request', 'push'},
            'ordinary CI must run for every PR and main push')
    require(triggers['pull_request'] in (None, {}), 'PR trigger must not use path/branch filters')
    require(triggers['push'] == {'branches': ['main']}, 'push trigger must target main')
    require(set(extended.get('on', {})) == {'workflow_dispatch'},
            'extended qualification must remain manual')
    validate_retained_authoring_matrices(extended)
    validate_manual_policy(extended, root)
    concurrency = pr.get('concurrency', {})
    require('github.workflow' in concurrency.get('group', '') and
            'github.event.pull_request.number || github.ref' in concurrency.get('group', ''),
            'concurrency must isolate workflows and PRs/refs')
    require(concurrency.get('cancel-in-progress') == "${{ github.event_name == 'pull_request' }}",
            'superseded PR runs must cancel without cancelling main validation')
    require(pr.get('permissions') == {'contents': 'read'}, 'PR CI must use read-only permissions')
    if reusable is None:
        reusable = load_yaml((root / '.github/workflows/build-toolchain.yml').read_text(encoding='utf-8'))
    require(reusable.get('permissions') == {'contents': 'read'},
            'reusable PR build must not elevate caller permissions')
    reusable_inputs = reusable['on']['workflow_call']['inputs']
    for option in ('package', 'validation_scratch'):
        require(reusable_inputs[option].get('default') is True,
                f'manual extended builds must preserve {option} by default')
    validate_focused_ctest_gate(reusable)
    jobs = pr['jobs']
    require(set(jobs) == set(REQUIRED + LANES + ('pr-readiness',)), 'unexpected PR jobs')
    detector = jobs['detect-changes']
    require(set(detector['outputs']) == set(OUTPUTS), 'detector output schema changed')
    for output in OUTPUTS:
        require(detector['outputs'][output] == '${{ steps.filter.outputs.' + output + ' }}',
                f'path output must use actual detector result: {output}')
    filter_steps = [step for step in detector['steps'] if step.get('id') == 'filter']
    require(len(filter_steps) == 1, 'exactly one change detector is required')
    filters = load_yaml(filter_steps[0]['with']['filters'])
    require(set(filters) == set(OUTPUTS), 'path filter schema changed')
    require(filters['generals'] == ['Generals/**'] and filters['zerohour'] == ['GeneralsMD/**'],
            'title-specific paths must stay independent')
    for path in ('Core/**', 'Dependencies/**', 'resources/**', 'cmake/**', 'triplets/**',
                 'CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'vcpkg-lock.json', '.github/workflows/**'):
        require(path in filters['shared'], f'shared path is not covered: {path}')
    require('tools/rendering-benchmark/**' in filters['rendering_tools'],
            'portable rendering tools need a path lane')
    expected_conditions = {
        'native-generals': "${{ needs.detect-changes.outputs.generals == 'true' || needs.detect-changes.outputs.shared == 'true' }}",
        'native-zerohour': "${{ needs.detect-changes.outputs.zerohour == 'true' || needs.detect-changes.outputs.shared == 'true' }}",
        'rendering-tools': "${{ needs.detect-changes.outputs.rendering_tools == 'true' }}",
    }
    for lane, condition in expected_conditions.items():
        require(jobs[lane].get('if') == condition, f'{lane} selection condition changed')
        require(needs_list(jobs[lane].get('needs')) == ['detect-changes'],
                f'{lane} must depend only on change detection')
    for lane, title, prefix, preset in (
        ('native-generals', 'Generals', 'g', 'x64-generals-vcpkg-product'),
        ('native-zerohour', 'GeneralsMD', 'z', 'x64-zerohour-vcpkg-product'),
    ):
        job = jobs[lane]
        require(job.get('uses') == './.github/workflows/build-toolchain.yml',
                f'{lane} must use the supported native build lane')
        settings = job['with']
        require(settings.get('game') == title and settings.get('preset') == preset,
                f'{lane} must use its normal Release x64 product preset')
        require(settings.get('tools') is False and settings.get('extras') is True and
                settings.get('product') is True and
                settings.get('package') is False and settings.get('validation_scratch') is False,
                f'{lane} must use bounded extras without packaging/qualification scratch')
        pattern = settings.get('focused_test_regex', '')
        names = focused_test_names(pattern)
        for name in names:
            require(not re.search(r'acceptance|host_validation|combined_native|stage5_performance', name, re.I),
                    f'expensive qualification selected: {name}')
            require(name.startswith(('core_', prefix + '_')), f'wrong-title CTest selected: {name}')
        targets = settings.get('focused_targets', '').split(',')
        harness = 'g_skirmish_ai_runner_contract_tests' if prefix == 'g' else 'z_runtime_regression_tests'
        require(prefix + '_generals' in targets and harness in targets,
                f'{lane} must compile product and title runtime harness')
        require(len(set(targets)) == len(targets), f'{lane} build targets must be distinct')
        for target in targets:
            require(re.fullmatch(r'[a-z0-9_]+', target) is not None,
                    f'focused build target must have an exact name: {target}')
        require(set(names) == required_smoke_tests(prefix),
                f'{lane} must retain the exact required 17 smoke tests')
        require(set(targets) == required_smoke_targets(prefix),
                f'{lane} must retain the exact required 14 build targets')
    policy = jobs['workflow-policy']
    require('if' not in policy and 'needs' not in policy, 'workflow policy must run unconditionally')
    policy_commands = (
        'python3 -m pip install PyYAML==6.0.2',
        'python3 .github/workflows/PRWorkflowPolicy.py',
        'python3 -m unittest discover -s .github/workflows -p PRWorkflowPolicyTests.py',
    )
    policy_runs = [step for step in policy['steps'] if 'run' in step]
    require(tuple(step['run'].strip() for step in policy_runs) == policy_commands,
            'focused policy and mutation tests must execute exact commands without failure masking')
    require(all('if' not in step and 'continue-on-error' not in step for step in policy_runs),
            'focused policy and mutation tests must execute unconditionally')
    readiness = jobs['pr-readiness']
    require(readiness.get('name') == 'PR readiness', 'required check name must remain stable')
    require(readiness.get('if') == '${{ always() }}', 'required check must always run')
    require(set(needs_list(readiness.get('needs'))) == set(REQUIRED + LANES), 'required check must consume every lane')
    readiness_steps = [step for step in readiness['steps'] if '--readiness' in step.get('run', '')]
    require(len(readiness_steps) == 1 and
            readiness_steps[0].get('env', {}).get('NEEDS_JSON') == '${{ toJSON(needs) }}',
            'required check must execute the tested evaluator against actual needs')
    evaluator = readiness_steps[0]
    require(evaluator.get('run', '').strip() == 'python3 .github/workflows/PRWorkflowPolicy.py --readiness' and
            'if' not in evaluator, 'required-check evaluator must execute without bypass or failure masking')
    for job in jobs.values():
        require('continue-on-error' not in job, 'job failures must not be ignored')
        for step in job.get('steps', []):
            require('continue-on-error' not in step, 'step failures must not be ignored')
            if 'uses' in step:
                require(re.fullmatch(r'[^@]+@[0-9a-f]{40}', step['uses']) is not None,
                        'third-party actions must use exact commit pins')
    for legacy in ('build-generals', 'build-generalsmd-win32', 'stage5-workflow-contract',
                   'stage5-combined-native-path-modes', 'stage5-lockstep-v2-qualification',
                   'stage5-performance-scaling-qualification', 'stage5-replaycheck-generals-x64',
                   'stage5-replaycheck-generalsmd-x64', 'stage5-development-readiness'):
        require(legacy in extended['jobs'], f'manual qualification suite removed: {legacy}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--readiness', action='store_true')
    parser.add_argument('--ctest-inventory', type=Path)
    parser.add_argument('--focused-test-regex')
    args = parser.parse_args()
    if args.readiness:
        require(args.ctest_inventory is None and args.focused_test_regex is None,
                '--readiness cannot be combined with CTest inventory options')
        evaluate_readiness(json.loads(os.environ['NEEDS_JSON']))
        print('PR readiness passed.')
    elif args.ctest_inventory is not None:
        require(args.focused_test_regex is not None,
                '--ctest-inventory requires --focused-test-regex')
        inventory = json.loads(args.ctest_inventory.read_text(encoding='utf-8-sig'))
        validate_ctest_inventory(inventory, args.focused_test_regex)
        print('Focused CTest configured inventory passed.')
    else:
        require(args.focused_test_regex is None,
                '--focused-test-regex requires --ctest-inventory')
        directory = args.source_root / '.github/workflows'
        validate_policy(load_yaml((directory / 'pr-ci.yml').read_text(encoding='utf-8')),
                        load_yaml((directory / 'ci.yml').read_text(encoding='utf-8')), args.source_root)
        print('Focused PR workflow policy passed.')


if __name__ == '__main__':
    main()
