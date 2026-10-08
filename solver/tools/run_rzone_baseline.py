#!/usr/bin/env python3
"""Run the 4/8/16/32 baseline matrix sequentially with fresh caches and proof checks."""
import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[2]
MODES = {
    # exact TT, TT symmetry, RZone, RZone symmetry, active pruning
    'DFS': (False, False, False, False, False),
    'DFS_TT': (True, False, False, False, False),
    'DFS_TT_sym': (True, True, False, False, False),
    'DFS_RZ': (False, False, True, False, False),
    'DFS_RZ_sym': (False, False, True, True, False),
    'DFS_RZ_sym_prune': (False, False, True, True, True),
    'DFS_TT_sym_RZ_sym_prune': (True, True, True, True, True),
}
FIELDS = [
    'target', 'mode', 'status', 'outcome', 'root_action', 'visited_nodes',
    'player_nodes', 'environment_nodes', 'node_limit_cutoffs', 'tt_hits', 'tt_records',
    'search_seconds', 'solve_wall_seconds', 'solve_user_seconds',
    'solve_system_seconds', 'solve_peak_rss_kib', 'verify_wall_seconds',
    'verify_user_seconds', 'verify_system_seconds', 'verify_peak_rss_kib',
    'full_board_fallbacks', 'trim_attempts',
    'trim_cells_removed', 'active_pruned_children', 'rz_records',
    'sgf_bytes', 'tt_bytes', 'rz_bytes', 'started_utc', 'finished_utc',
    'solve_exit_code', 'verify_exit_code', 'error',
]


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def boolean(value):
    return 'true' if value else 'false'


def configuration(args, target, mode, prefix):
    tt, tt_symmetry, rz, symmetry, pruning = MODES[mode]
    return {
        'winning_tile': target, 'node_limit': args.node_limit,
        'progress_interval': args.progress_interval,
        'use_transposition_table': boolean(tt), 'use_tt_symmetry': boolean(tt_symmetry),
        'load_tt': 'false', 'save_tt': boolean(tt), 'tt_path': str(prefix) + '.tt',
        'rzone_mode': 'lookup' if rz else 'disabled', 'rzone_lookup_policy': 'smallest_first',
        'use_rzone_symmetry': boolean(symmetry), 'rzone_max_cells': 16,
        'rzone_shadow_exact_check': 'false', 'rzone_active_pruning': boolean(pruning),
        'rzone_trimming': boolean(args.trimming),
        'rzone_full_board_placement_fallback': 'true',
        'load_rz': 'false', 'save_rz': boolean(rz), 'rz_path': str(prefix) + '.rz',
        'opening': '2@0,2@1', 'network': args.network, 'model_path': str(args.model),
        'sgf_path': str(prefix) + '.sgf',
    }


def log_fields(path):
    fields = {}
    with path.open(errors='replace') as stream:
        for line in stream:
            key, separator, value = line.strip().partition(': ')
            if separator and key != 'progress':
                fields[key] = value
    return fields


def stop_on_signal(signum, frame):
    raise KeyboardInterrupt('received signal ' + str(signum))


def collect_usage(timing, state, phase):
    if not timing.exists():
        return
    usage = log_fields(timing)
    for key, label, convert in [
        ('user_seconds', 'User time (seconds)', float),
        ('system_seconds', 'System time (seconds)', float),
        ('peak_rss_kib', 'Maximum resident set size (kbytes)', int),
    ]:
        if label in usage:
            state[phase + '_' + key] = convert(usage[label])


def run_timed(command, log, timing, state, phase, save):
    timed = ['/usr/bin/time', '-v', '-o', str(timing), 'stdbuf', '-oL', '-eL'] + command
    state[phase + '_command'] = timed
    state[phase + '_started_utc'] = now()
    state['status'] = phase.upper() + '_RUNNING'
    save()
    start = time.monotonic()
    with log.open('x') as stream:
        process = subprocess.Popen(timed, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                   env={**os.environ, 'LC_ALL': 'C'}, start_new_session=True)
        state[phase + '_pid'] = process.pid
        save()
        try:
            code = process.wait()
        except BaseException:
            # /usr/bin/time, stdbuf and the solver share this process group.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
        finally:
            state[phase + '_wall_seconds'] = time.monotonic() - start
            state[phase + '_finished_utc'] = now()
            state[phase + '_exit_code'] = process.returncode
            collect_usage(timing, state, phase)
            save()
    if code:
        raise RuntimeError(phase + ' exited with code ' + str(code) + '; see ' + str(log))


def save_summary(out, records):
    write_json(out / 'summary.json', records)
    temporary = out / 'summary.csv.tmp'
    with temporary.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(records)
    temporary.replace(out / 'summary.csv')


def prepare_binaries(args, out, metadata):
    binary_dir = out / 'bin'
    binary_dir.mkdir()
    if args.binaries_dir:
        for name in ('solver2048', 'proof_checker'):
            shutil.copy2(args.binaries_dir / name, binary_dir / name)
        metadata['binary_origin'] = str(args.binaries_dir)
    else:
        command = ['make', 'solver', 'proof-checker', '-j2',
                   'SOLVER_OUTPUT=' + str(binary_dir / 'solver2048'),
                   'PROOF_CHECKER_OUTPUT=' + str(binary_dir / 'proof_checker')]
        metadata['build_command'] = command
        with (out / 'build.log').open('x') as stream:
            subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)
    metadata['binary_sha256'] = {name: digest(binary_dir / name)
                                 for name in ('solver2048', 'proof_checker')}
    # Preserve the working sources, including untracked C++ headers used by trimming.
    files = subprocess.check_output(
        ['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT)
    with tarfile.open(out / 'source.tar.gz', 'w:gz') as archive:
        for name in sorted(set(os.fsdecode(p) for p in files.split(b'\0') if p)):
            path = Path(name)
            if (path.parts[0] != 'result' and (ROOT / path).is_file() and
                    (path.suffix in ('.cpp', '.h', '.hpp', '.cc', '.inl') or name == 'makefile')):
                archive.add(ROOT / path, arcname=name)
    shutil.copy2(Path(__file__), out / 'runner_source.py')
    metadata['source_archive_sha256'] = digest(out / 'source.tar.gz')
    metadata['source_note'] = ('Source archive captures the current working tree; '
                               'with --binaries-dir, binary provenance belongs to that earlier build.')
    return binary_dir


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--targets', nargs='+', type=int, default=[4, 8, 16, 32])
    parser.add_argument('--modes', nargs='+', choices=list(MODES), default=list(MODES))
    parser.add_argument('--output', type=Path, default=ROOT / 'result/baselinev2')
    parser.add_argument('--node-limit', type=int, default=100000000,
                        help='per-case visited-node limit (default: 100000000); 0 means unlimited')
    parser.add_argument('--progress-interval', type=int, default=1000000)
    parser.add_argument('--network', choices=['4x6patt', '8x6patt'], default='4x6patt')
    parser.add_argument('--model', type=Path,
                        help='model path (default: models/<network>.w)')
    parser.add_argument('--binaries-dir', type=Path, help='copy existing solver2048/proof_checker; skip build')
    parser.add_argument('--trimming', action='store_true', help='enable trimming for a later comparison run')
    parser.add_argument('--dry-run', action='store_true', help='print plan without building or running')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.model = (args.model or ROOT / 'models' / (args.network + '.w')).resolve()
    if args.binaries_dir:
        args.binaries_dir = args.binaries_dir.resolve()
    if (any(t < 4 or t > 32768 or t & (t - 1) for t in args.targets) or
            args.node_limit < 0 or args.progress_interval < 0):
        parser.error('targets must be powers of two in [4, 32768]; limits must be nonnegative')
    if len(set(args.targets)) != len(args.targets) or len(set(args.modes)) != len(args.modes):
        parser.error('duplicate targets/modes are not allowed')
    cases = [(t, mode) for t in args.targets for mode in args.modes]
    if args.dry_run:
        print(json.dumps({'output': str(args.output), 'case_count': len(cases), 'cases': cases,
                          'configurations': [configuration(args, t, mode, args.output / (str(t) + '_' + mode))
                                             for t, mode in cases],
                          'build': not bool(args.binaries_dir), 'verify': True}, indent=2))
        return 0
    for program in ('git', 'stdbuf', '/usr/bin/time') + (() if args.binaries_dir else ('make',)):
        if not shutil.which(program):
            parser.error('missing program: ' + program)
    if not args.model.is_file():
        parser.error('missing model: ' + str(args.model))
    if args.binaries_dir:
        for name in ('solver2048', 'proof_checker'):
            if not os.access(args.binaries_dir / name, os.X_OK):
                parser.error('missing executable: ' + str(args.binaries_dir / name))
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error('output directory must be empty; existing results are never overwritten: ' + str(out))
    # Exclusive creation prevents two runners from starting in the same directory.
    with (out / 'run.lock').open('x') as lock:
        lock.write(str(os.getpid()) + '\n')
    signal.signal(signal.SIGTERM, stop_on_signal)
    records = []
    metadata = {'status': 'PREPARING', 'started_utc': now(), 'runner_pid': os.getpid(),
                'command': sys.argv, 'host': platform.node(), 'platform': platform.platform(),
                'targets': args.targets, 'modes': args.modes, 'case_count': len(cases),
                'node_limit': args.node_limit, 'progress_interval': args.progress_interval,
                'trimming': args.trimming, 'full_board_fallback': True,
                'network': args.network,
                'execution': 'sequential, one fresh process and empty TT/RZ per case'}
    started = time.monotonic()

    def save():
        write_json(out / 'metadata.json', metadata)
        save_summary(out, records)

    def report(message):
        line = now() + ' ' + message
        print(line, flush=True)
        with (out / 'runner.log').open('a') as log:
            log.write(line + '\n')

    save()
    try:
        report('Preparing binaries and provenance')
        metadata['git_head'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        metadata['git_status'] = subprocess.check_output(['git', 'status', '--short'], cwd=ROOT, text=True)
        with (out / 'working-tree.patch').open('wb') as stream:
            subprocess.run(['git', 'diff', 'HEAD', '--binary'], cwd=ROOT, stdout=stream, check=True)
        metadata['model'] = str(args.model)
        metadata['model_sha256'] = digest(args.model)
        binaries = prepare_binaries(args, out, metadata)
        save()
        for target, mode in cases:
            prefix = out / (str(target) + '_' + mode)
            state = {'target': target, 'mode': mode, 'status': 'PREPARING', 'started_utc': now()}
            records.append(state)
            metadata['status'] = 'RUNNING'
            metadata['current_case'] = prefix.name

            def save_case():
                write_json(Path(str(prefix) + '.status.json'), state)
                save()

            try:
                config = configuration(args, target, mode, prefix)
                state['config'] = config
                cfg = Path(str(prefix) + '.cfg')
                cfg.write_text(''.join(str(k) + '=' + str(v) + '\n' for k, v in config.items()))
                report('Solving ' + prefix.name)
                run_timed([str(binaries / 'solver2048'), '--config', str(cfg)],
                          Path(str(prefix) + '.log'), Path(str(prefix) + '.time.txt'), state, 'solve', save_case)
                metrics = log_fields(Path(str(prefix) + '.log'))
                state['solver_metrics'] = metrics
                state['outcome'] = metrics['root result']
                state['root_action'] = metrics.get('root action', '')
                state['search_seconds'] = float(metrics['search elapsed'].rstrip('s'))
                for key, label in [
                    ('visited_nodes', 'combined nodes visited'),
                    ('player_nodes', 'player nodes visited'),
                    ('environment_nodes', 'environment nodes visited'),
                    ('node_limit_cutoffs', 'node-limit cutoffs'),
                ]:
                    state[key] = int(metrics[label])
                for key, label in [
                    ('full_board_fallbacks', 'RZone full-board placement certificates'),
                    ('trim_attempts', 'RZone trim attempts'),
                    ('trim_cells_removed', 'RZone trim cells removed'),
                    ('active_pruned_children', 'RZone active-pruning full lookups avoided'),
                    ('rz_records', 'RZone new certificates'),
                ]:
                    state[key] = int(metrics[label]) if MODES[mode][2] else 0
                state['tt_hits'] = sum(int(metrics.get(phase + ' TT hits', 0))
                                       for phase in ('player', 'environment'))
                state['tt_records'] = sum(int(metrics.get(phase + ' TT entries', 0))
                                          for phase in ('player', 'environment'))
                expected = 'enabled' if args.trimming else 'disabled'
                if (metrics.get('RZone trimming') != expected or
                        metrics.get('RZone full-board placement fallback') != 'enabled' or
                        (not args.trimming and state['trim_attempts'] != 0)):
                    raise RuntimeError('solver did not honor the requested RZone switches')
                if state['outcome'] == 'UNKNOWN':
                    state['status'] = 'INCOMPLETE'
                    state['error'] = 'UNKNOWN: incomplete solve, not a verified baseline'
                    report(prefix.name + ' INCOMPLETE; continuing to next case')
                    continue
                if state['outcome'] not in ('WIN', 'LOSS'):
                    raise RuntimeError('unexpected solver outcome: ' + state['outcome'])
                verify = [str(binaries / 'proof_checker'), '--sgf', str(prefix) + '.sgf']
                if MODES[mode][2]:
                    verify += ['--rz', str(prefix) + '.rz']
                if MODES[mode][0]:
                    verify += ['--tt', str(prefix) + '.tt']
                report('Verifying ' + prefix.name)
                run_timed(verify, Path(str(prefix) + '.proof_check.log'),
                          Path(str(prefix) + '.proof_check_time.txt'), state, 'verify', save_case)
                if log_fields(Path(str(prefix) + '.proof_check.log')).get('Proof status') != 'VERIFIED':
                    raise RuntimeError('checker did not report VERIFIED')
                state['status'] = 'VERIFIED'
                report(prefix.name + ' VERIFIED')
            except KeyboardInterrupt as error:
                state['status'] = 'INTERRUPTED'
                state['error'] = str(error)
                raise
            except Exception as error:
                state['status'] = 'FAILED'
                state['error'] = str(error)
                report(prefix.name + ' FAILED: ' + str(error) + '; continuing to next case')
            finally:
                state['finished_utc'] = now()
                for extension in ('sgf', 'tt', 'rz'):
                    artifact = Path(str(prefix) + '.' + extension)
                    state[extension + '_bytes'] = artifact.stat().st_size if artifact.exists() else 0
                save_case()
        metadata['status'] = ('COMPLETE' if all(r['status'] == 'VERIFIED' for r in records)
                              else 'COMPLETED_WITH_ERRORS')
    except (Exception, KeyboardInterrupt) as error:
        metadata['status'] = 'INTERRUPTED' if isinstance(error, KeyboardInterrupt) else 'FAILED'
        metadata['error'] = str(error)
        report(metadata['status'] + ': ' + str(error))
    finally:
        metadata['finished_utc'] = now()
        metadata['total_wall_seconds'] = time.monotonic() - started
        metadata['verified_cases'] = sum(r['status'] == 'VERIFIED' for r in records)
        metadata['failed_cases'] = sum(r['status'] == 'FAILED' for r in records)
        metadata['incomplete_cases'] = sum(r['status'] == 'INCOMPLETE' for r in records)
        save()
    report(metadata['status'] + ': ' + str(out / 'summary.csv'))
    return 0 if metadata['status'] == 'COMPLETE' else 1


if __name__ == '__main__':
    sys.exit(main())
