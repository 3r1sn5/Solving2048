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
args = parser.parse_args()
solver = args.solver.resolve()

with tempfile.TemporaryDirectory(prefix='solving2048-seed-test-') as name:
    work = Path(name)
    settings = dict(line.split('=', 1) for line in (ROOT/'solver/solver.cfg.example').read_text().splitlines()
                    if line and not line.startswith('#'))
    settings.update(winning_tile='8', node_limit='1000000', use_transposition_table='true',
                    use_tt_symmetry='true', load_tt='false', save_tt='true', load_rz='false',
                    save_rz='true', rzone_mode='lookup', rzone_active_pruning='true',
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
        else:
            assert result.returncode != 0, result.stdout
            assert error in result.stderr, result.stderr
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
    no_rz = ['--rzone-mode','disabled','--no-rzone-active-pruning','--no-load-rz','--no-save-rz']
    plain_seed = work/'tt_only_seed'
    run(plain_seed, ['2@0,2@1'], extra=no_rz)
    run(work/'tt_only_continued', ['2@0,4@1'], plain_seed/'proof_sequence.txt', extra=no_rz)
    result = subprocess.run([str(solver),'--config',str(cfg),'--seed-sequence',str(seed/'proof_sequence.txt')],
                            cwd=ROOT,capture_output=True,text=True)
    assert result.returncode != 0 and 'requires batch openings' in result.stderr
    assert hashes == {p:hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}
    print('PASS: seeded RZ/TT continuation, chained sequences, proof verification, loaded counters,')
    print('      duplicate/target/binding rejection, TT-only seed, CLI validation, immutable inputs.')
