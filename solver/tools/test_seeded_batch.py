#!/usr/bin/env python3
"""Integration checks for batch continuation and its complete proof sequence."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--solver', type=Path, default=ROOT / 'solver/solver2048')
parser.add_argument('--checker', type=Path, default=ROOT / 'solver/proof_checker')
args = parser.parse_args()
solver = args.solver.resolve()
checker = args.checker.resolve()

with tempfile.TemporaryDirectory(prefix='solving2048-seed-test-') as name:
    work = Path(name)
    settings = dict(line.split('=', 1) for line in (ROOT/'solver/solver.cfg.example').read_text().splitlines()
                    if line and not line.startswith('#'))
    settings.update(winning_tile='8', node_limit='1000000', use_transposition_table='true',
                    use_tt_symmetry='true', load_tt='false', save_tt='true', load_rz='false',
                    save_rz='true', use_rzone='true', rzone_active_pruning='true',
                    rzone_trimming='true',
                    rzone_full_board_placement_fallback='true', network='4x6patt',
                    model_path=str(ROOT/'models/4x6patt.w'))
    cfg = work/'solver.cfg'
    cfg.write_text(''.join(f'{k}={v}\n' for k,v in settings.items()))

    def run(output, openings, seed=None, success=True, error=None, extra=()):
        command = [str(solver), '--config', str(cfg), '--output-dir', str(output), '--verify']
        for opening in openings:
            command += ['--opening', opening]
        if seed:
            command += ['--seed-sequence', str(seed)]
        command += list(extra)
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        if success:
            assert result.returncode == 0, result.stdout + result.stderr
            assert 'batch proof status: VERIFIED' in result.stdout
            for sgf in (output/'sgf').glob('*.sgf'):
                assert 'rzone_lookup_policy = radix_tree' in sgf.read_text()
        else:
            assert result.returncode != 0, result.stdout
            assert error in result.stderr, result.stderr
        return result

    def verify(sequence, success=True, error=None):
        result = subprocess.run([str(checker), '--sequence', str(sequence)],
                                cwd=ROOT, capture_output=True, text=True)
        if success:
            assert result.returncode == 0, result.stdout + result.stderr
            assert 'Proof status: VERIFIED' in result.stdout
        else:
            assert result.returncode != 0 and error in result.stderr, result.stdout + result.stderr
        return result

    seed = work/'seed'
    run(seed, ['2@0,2@1'])
    originals = [seed/'proof.tt', seed/'proof.rz', seed/'sgf/000001.sgf', seed/'proof_sequence.txt']
    hashes = {p:hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}
    continuation = work/'continued'
    result = run(continuation, ['2@0,4@1','2@0,2@2'], seed/'proof_sequence.txt')
    assert 'seed TT loaded:' in result.stdout and 'seed RZone loaded:' in result.stdout
    sequence = (continuation/'proof_sequence.txt').read_text()
    assert sequence.count('sgf=') == 3
    first = (continuation/'sgf/000001.sgf').read_text()
    assert 'persistent_tt_load = enabled' in first and 'persistent_rz_load = enabled' in first
    assert 'player_tt_loaded_entries = 0\n' not in first
    assert 'rzone_loaded_certificates = 0\n' not in first
    run(work/'continued_again', ['4@2,4@3'], continuation/'proof_sequence.txt')
    run(work/'duplicate', ['2@0,2@1'], seed/'proof_sequence.txt', False,
        'already present in the seed')
    run(work/'wrong_target', ['2@0,4@1'], seed/'proof_sequence.txt', False,
        'matching target', ['--winning-tile','16'])
    bad_sgf = work/'wrong_binding.sgf'
    text, count = re.subn(r'tt_payload_checksum = [^\n\]]+', 'tt_payload_checksum = 0',
                         (seed/'sgf/000001.sgf').read_text())
    assert count == 1
    bad_sgf.write_text(text)
    bad_manifest = work/'bad_seed.txt'
    bad_manifest.write_text(f'format_version=1\ntt={seed}/proof.tt\nrz={seed}/proof.rz\nsgf={bad_sgf}\n')
    run(work/'wrong_binding', ['2@0,4@1'], bad_manifest, False, 'does not match its final SGF')
    no_rz = ['--no-rzone']
    plain_seed = work/'tt_only_seed'
    run(plain_seed, ['2@0,2@1'], extra=no_rz)
    assert not (plain_seed/'proof.rz').exists()
    plain_text = (plain_seed/'sgf/000001.sgf').read_text()
    assert 'rzone_mode = disabled' in plain_text
    assert 'rzone_active_pruning = disabled' in plain_text
    assert 'persistent_rz_save = disabled' in plain_text
    run(work/'tt_only_continued', ['2@0,4@1'], plain_seed/'proof_sequence.txt', extra=no_rz)
    verify(continuation/'proof_sequence.txt')
    verify(work/'tt_only_continued/proof_sequence.txt')
    missing_tt = work/'missing_tt.txt'
    missing_tt.write_text(f'format_version=1\nrz={seed}/proof.rz\nsgf={seed}/sgf/000001.sgf\n')
    verify(missing_tt, False, '--tt is required')

    result = subprocess.run([str(solver),'--config',str(cfg),'--seed-sequence',str(seed/'proof_sequence.txt')],
                            cwd=ROOT,capture_output=True,text=True)
    assert result.returncode != 0 and 'requires batch openings' in result.stderr

    # Changing only the master switch must work despite the old TT flags/path.
    settings['use_transposition_table'] = 'false'
    cfg.write_text(''.join(f'{k}={v}\n' for k,v in settings.items()))
    rz_seed = work/'rz_only_seed'
    run(rz_seed, ['2@0,2@1', '2@0,2@2'])
    rz_originals = [rz_seed/'proof.rz', rz_seed/'proof_sequence.txt',
                    *sorted((rz_seed/'sgf').glob('*.sgf'))]
    rz_hashes = {p:hashlib.sha256(p.read_bytes()).hexdigest() for p in rz_originals}
    rz_continuation = work/'rz_only_continued'
    result = run(rz_continuation, ['2@0,4@1', '4@2,4@3'], rz_seed/'proof_sequence.txt')
    assert 'seed RZone loaded:' in result.stdout and 'seed TT loaded:' not in result.stdout
    run(work/'rz_only_chained', ['2@4,2@5'], rz_continuation/'proof_sequence.txt')
    for output in [rz_seed, rz_continuation, work/'rz_only_chained']:
        assert not (output/'proof.tt').exists()
        assert not re.search(r'^tt=', (output/'proof_sequence.txt').read_text(), re.M)
        checked = verify(output/'proof_sequence.txt')
        assert 'TT integrity: NOT SUPPLIED' in checked.stdout
        for sgf in (output/'sgf').glob('*.sgf'):
            contents = sgf.read_text()
            assert 'transposition_table = disabled' in contents
            assert 'persistent_tt_load = disabled' in contents
            assert 'persistent_tt_save = disabled' in contents
    assert rz_hashes == {p:hashlib.sha256(p.read_bytes()).hexdigest() for p in rz_originals}
    bad_rz_sgf = work/'bad_rz_binding.sgf'
    contents, count = re.subn(r'rz_prefix_checksum = [^\n\]]+', 'rz_prefix_checksum = 0',
                             (rz_seed/'sgf/000002.sgf').read_text())
    assert count == 1
    bad_rz_sgf.write_text(contents)
    bad_rz_manifest = work/'bad_rz_seed.txt'
    bad_rz_manifest.write_text(f'format_version=1\nrz={rz_seed}/proof.rz\n'
                               f'sgf={rz_seed}/sgf/000001.sgf\nsgf={bad_rz_sgf}\n')
    run(work/'bad_rz_seed', ['4@2,4@3'], bad_rz_manifest, False,
        'seed RZone snapshot does not match')
    verify(bad_rz_manifest, False, 'checksum')
    run(work/'tt_seed_with_tt_off', ['4@2,4@3'], seed/'proof_sequence.txt', False,
        'seed TT presence must match')
    run(work/'rz_seed_with_tt_on', ['4@2,4@3'], rz_seed/'proof_sequence.txt', False,
        'seed TT presence must match', ['--tt'])

    # A TT-disabled batch also permits omitting its subordinate config keys.
    for key in ['use_tt_symmetry', 'load_tt', 'save_tt', 'tt_path']:
        settings.pop(key)
    cfg.write_text(''.join(f'{k}={v}\n' for k,v in settings.items()))
    run(work/'rz_no_tt_keys', ['2@0,2@1'])
    failed = work/'limited_batch'
    run(failed, ['2@0,2@1'], success=False, error='did not produce WIN',
        extra=['--node-limit', '1'])
    assert not (failed/'proof_sequence.txt').exists()
    assert not (failed/'proof.tt').exists() and not (failed/'proof.rz').exists()
    trace_only = work/'trace_only'
    no_tables = no_rz + ['--winning-tile', '4']
    run(trace_only, ['2@0,2@1', '2@0,4@1'], extra=no_tables)
    verify(trace_only/'proof_sequence.txt')
    run(work/'trace_continued', ['4@2,4@3'], trace_only/'proof_sequence.txt', extra=no_tables)
    verify(work/'trace_continued/proof_sequence.txt')
    assert not (trace_only/'proof.tt').exists() and not (trace_only/'proof.rz').exists()
    settings['use_rzone'] = 'false'
    cfg.write_text(''.join(f'{k}={v}\n' for k,v in settings.items()))
    cfg_disabled = work/'cfg_rzone_disabled'
    run(cfg_disabled, ['2@0,2@1', '2@0,4@1'], extra=['--winning-tile', '4'])
    assert not (cfg_disabled/'proof.rz').exists()
    verify(cfg_disabled/'proof_sequence.txt')
    run(work/'disabled_with_rz_seed', ['4@2,4@3'], rz_seed/'proof_sequence.txt',
        success=False, error='seed RZone presence must match')
    reenabled = work/'cli_rzone_enabled'
    run(reenabled, ['2@0,2@1'], extra=['--rzone', '--rzone-active-pruning'])
    assert (reenabled/'proof.rz').exists()
    verify(reenabled/'proof_sequence.txt')
    assert hashes == {p:hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}
    print('PASS: seeded RZ/TT continuation, chained sequences, proof verification, loaded counters,')
    print('      duplicate/target/binding rejection, TT-only/RZ-only/trace-only seeds,')
    print('      TT/RZone master switches, fixed radix policy, standalone checker,')
    print('      mode mismatch rejection, immutable inputs.')
