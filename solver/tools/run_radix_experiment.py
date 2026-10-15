#!/usr/bin/env python3
"""Run one reproducible, fresh-cache RZone index experiment without overwriting results."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def digest(path, normalize_sgf=False):
    result = hashlib.sha256()
    ignored = re.compile(rb'^(?:tt_file|rz_file|config_file|elapsed_seconds|rzone_lookup_policy|'
                         rb'rzone_lookup_masks_tested|rzone_lookup_candidates|rzone_pattern_matches|'
                         rb'rzone_lookup_early_exits|rzone_radix_\w+) = ')
    with path.open('rb') as stream:
        if normalize_sgf:
            for line in stream:
                if not ignored.match(line):
                    result.update(line)
        else:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                result.update(block)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--target', type=int, required=True)
    parser.add_argument('--node-limit', type=int, default=0)
    parser.add_argument('--checker', type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    settings = dict(winning_tile=args.target, node_limit=args.node_limit, progress_interval=1000000,
                    use_transposition_table='true', use_tt_symmetry='true', load_tt='false', save_tt='true',
                    tt_path=out/'proof.tt', use_rzone='true',
                    use_rzone_symmetry='true', rzone_max_cells=16,
                    rzone_active_pruning='true', rzone_trimming='true', rzone_full_board_placement_fallback='true',
                    load_rz='false', save_rz='true', rz_path=out/'proof.rz', opening='2@0,2@1',
                    network='4x6patt', model_path=args.model.resolve(), sgf_path=out/'proof.sgf')
    (out/'solver.cfg').write_text(''.join(f'{k}={v}\n' for k,v in settings.items()))
    state = dict(state='RUNNING', target=args.target, policy='radix_tree', node_limit=args.node_limit,
                 binary=str(args.binary.resolve()), binary_sha256=digest(args.binary),
                 model_sha256=digest(args.model), started_unix=time.time())

    def save():
        temporary = out/'status.json.tmp'
        temporary.write_text(json.dumps(state, indent=2)+'\n')
        temporary.replace(out/'status.json')

    save()
    command = ['/usr/bin/time', '-v', '-o', str(out/'time.txt'), 'stdbuf', '-oL', '-eL',
               str(args.binary.resolve()), '--config', str(out/'solver.cfg')]
    with (out/'solve.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        state['pid'] = process.pid
        state['command'] = command
        save()
        state['exit_code'] = process.wait()
    state['solve_wall_seconds'] = time.time()-state['started_unix']
    state['state'] = 'FAILED' if state['exit_code'] else 'SOLVED'
    fields = dict(line.split(': ', 1) for line in (out/'solve.log').read_text().splitlines()
                  if ': ' in line and not line.startswith('progress:'))
    state['metrics'] = fields
    save()
    if state['exit_code']:
        raise SystemExit(state['exit_code'])
    state['search_seconds'] = float(fields['search elapsed'].rstrip('s'))
    state['outcome'] = fields['root result']
    state['nodes'] = int(fields['combined nodes visited'])
    state['artifact_hashes'] = {suffix:digest(out/('proof.'+suffix), suffix=='sgf')
                              for suffix in ['tt','rz','sgf']}
    if args.checker:
        started = time.monotonic()
        with (out/'verify.log').open('w') as log:
            code = subprocess.call([str(args.checker.resolve()), '--sgf', str(out/'proof.sgf'),
                                    '--tt', str(out/'proof.tt'), '--rz', str(out/'proof.rz')],
                                   stdout=log, stderr=subprocess.STDOUT)
        state['verify_seconds'] = time.monotonic()-started
        state['verify_exit_code'] = code
        state['state'] = 'FAILED' if code else 'VERIFIED'
        save()
        if code:
            raise SystemExit(code)
    else:
        state['state'] = 'COMPLETE'
    save()
    print(json.dumps({k:state[k] for k in ['state','target','policy','outcome','nodes','search_seconds']}), flush=True)


if __name__ == '__main__':
    main()
