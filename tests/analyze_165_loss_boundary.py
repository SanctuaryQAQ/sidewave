"""Compare packet identities and local arrival order; no cross-PC latency claim."""
import ast
import json
import statistics
from collections import defaultdict
from pathlib import Path

root = Path(__file__).resolve().parents[1]
folder = Path((root / 'results/165-loss-boundary-latest.txt').read_text(encoding='utf-8-sig'))
# Reuse the reviewed pcapng/parser functions without executing the historical
# timing analysis or opening its older experiment files.
parser_path = root / 'tests/analyze_udp_nic_boundary.py'
tree = ast.parse(parser_path.read_text(encoding='utf-8-sig'))
defs = [n for n in tree.body if isinstance(n, (ast.Import, ast.ImportFrom, ast.FunctionDef))]
scope = {'__file__': str(parser_path), 'OUT': folder, 'NETCX': False}
exec(compile(ast.Module(body=defs, type_ignores=[]), str(parser_path), 'exec'), scope)
tx, _, tx_counts = scope['load'](folder / 'packets.pcapng')
rx, _, rx_counts = scope['load'](folder / 'laptop-packets.pcapng')
common = tx.keys() & rx.keys()
assert len(common) > 1000, 'Too few matching packet identities'
# Offset is only used to exclude capture edges, not interpreted as latency.
offset = statistics.median(rx[k]['ns'] - tx[k]['ns'] for k in common)
tx_lo, tx_hi = min(v['ns'] for v in tx.values()), max(v['ns'] for v in tx.values())
rx_lo, rx_hi = min(v['ns'] for v in rx.values()), max(v['ns'] for v in rx.values())
lo, hi = max(tx_lo, rx_lo - offset) + 500_000_000, min(tx_hi, rx_hi - offset) - 500_000_000
assert hi > lo
groups = defaultdict(dict)
rx_first = defaultdict(dict)
for k, v in tx.items():
    groups[(k[0], v['frame'])][k] = v
for k, v in rx.items():
    cookie, frame = k[0], v['frame']
    rx_first[cookie][frame] = min(v['ns'], rx_first[cookie].get(frame, v['ns']))
future = {}
for cookie, frames in rx_first.items():
    earliest = None
    for frame in sorted(frames, reverse=True):
        future[(cookie, frame)] = earliest
        earliest = frames[frame] if earliest is None else min(earliest, frames[frame])

rows = []
for (cookie, frame), packets in groups.items():
    times = [v['ns'] for v in packets.values()]
    if min(times) < lo or max(times) > hi:
        continue
    by_packet = defaultdict(dict)
    for k, v in packets.items():
        by_packet[v['index']][v['fragment']] = (k, v)
    count = next(iter(packets.values()))['count']
    if len(by_packet) != count or any(
            len([i for i in p if i < next(iter(p.values()))[1]['fragments']]) != next(iter(p.values()))[1]['fragments']
            for p in by_packet.values()):
        continue  # Incomplete sender capture is not a missing receive frame.
    missing_data = missing_parity = 0
    complete_times = []
    failed_packets = 0
    for packet in by_packet.values():
        fragments = next(iter(packet.values()))[1]['fragments']
        data = sorted(rx[k]['ns'] for i, (k, _) in packet.items() if i < fragments and k in rx)
        missing_data += fragments - len(data)
        parity_key = packet.get(fragments, (None, None))[0]
        parity_time = rx[parity_key]['ns'] if parity_key in rx else None
        missing_parity += int(parity_key is not None and parity_time is None)
        full_time = max(data) if len(data) == fragments else None
        fec_time = max(parity_time, data[fragments - 2] if fragments > 1 else 0) if parity_time is not None and len(data) >= fragments - 1 else None
        options = [t for t in (full_time, fec_time) if t is not None]
        if options:
            complete_times.append(min(options))
        else:
            failed_packets += 1
    possible = max(complete_times) if not failed_packets else None
    runs = []
    run = 0
    for k, v in sorted(packets.items(), key=lambda item: item[0][1]):
        if k not in rx:
            run += 1
        elif run:
            runs.append(run)
            run = 0
    if run:
        runs.append(run)
    next_start = future.get((cookie, frame))
    overtaken = possible is not None and next_start is not None and possible > next_start
    rows.append({'frame': frame, 'missing_data_packets_at_rx_tap': missing_data,
                 'missing_datagram_run_lengths': runs,
                 'missing_parity_packets_at_rx_tap': missing_parity,
                 'unrecoverable_codec_packets_at_rx_tap': failed_packets,
                 'recoverable_complete_at_rx_tap': possible is not None,
                 'completion_after_newer_frame_first_packet': overtaken,
                 'late_completion_after_newer_start_ms': (possible - next_start)/1e6 if overtaken else 0})
assert rows, 'No complete sender frames within both capture windows'
before = json.loads((folder / 'app-before.json').read_text(encoding='utf-8-sig'))
after = json.loads((folder / 'app-after.json').read_text(encoding='utf-8-sig'))
result = {
    'valid_capture': True, 'folder': str(folder), 'analyzed_frames': len(rows),
    'unique_tx_datagrams': len(tx), 'unique_rx_datagrams': len(rx),
    'frame_data_packets_missing_at_rx_tap': sum(r['missing_data_packets_at_rx_tap'] for r in rows),
    'missing_datagram_run_lengths': [n for r in rows for n in r['missing_datagram_run_lengths']],
    'frames_with_any_data_missing_at_rx_tap': sum(r['missing_data_packets_at_rx_tap'] > 0 for r in rows),
    'frames_not_recoverable_at_rx_tap': sum(not r['recoverable_complete_at_rx_tap'] for r in rows),
    'frames_complete_after_newer_start_at_rx_tap': sum(r['completion_after_newer_frame_first_packet'] for r in rows),
    'late_completion_max_ms': max(r['late_completion_after_newer_start_ms'] for r in rows),
    'tx_event_counts': dict(tx_counts), 'rx_event_counts': dict(rx_counts),
    'receiver_stats_same_pid': before['receiverPid'] == after['receiverPid'],
    'app_counters_delta': {k: after['stats'][k] - before['stats'][k] for k in
                           ('network_incomplete_frames', 'freshness_dropped_frames', 'decoder_not_ready_frames', 'presented_frames')},
    'limits': ['These are miniport software taps, not physical-wire or application recvfrom records.',
               'Capture drop counters/logs must be checked before interpreting missing packets.',
               'Application counter interval is longer than the paired packet capture window.',
               'FEC reconstruction here checks fragment presence, not the decoded image.']}
(folder / 'loss-summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
(folder / 'frame-loss-rows.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
print(json.dumps(result, ensure_ascii=False, indent=2))
