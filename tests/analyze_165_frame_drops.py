"""Analyze cumulative drop counters; never treat incomplete packet blocks as frames."""
import json
import statistics
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rows = json.loads((root / 'results/165-frame-drops-samples.json').read_text(encoding='utf-8-sig'))
validation = json.loads((root / 'results/165-frame-drops-validation.json').read_text(encoding='utf-8-sig'))
assert validation['Passed']
reports = []
for phase in sorted({r['Phase'] for r in rows}):
    samples = [r for r in rows if r['Phase'] == phase]
    f, l = samples[0], samples[-1]
    rx_s = l['Receiver']['elapsed_s'] - f['Receiver']['elapsed_s']
    tx_s = l['Sender']['elapsed_s'] - f['Sender']['elapsed_s']
    assert rx_s > 0 and tx_s > 0
    assert len({r['Receiver']['pid'] for r in samples}) == 1
    assert len({r['Sender']['pid'] for r in samples}) == 1

    def delta(side, key):
        value = l[side][key] - f[side][key]
        assert value >= 0, (side, key)
        return value

    def median(side, key):
        return statistics.median(r[side][key] for r in samples)

    started = delta('Receiver', 'network_started_frames')
    incomplete = delta('Receiver', 'network_incomplete_frames')
    complete = delta('Receiver', 'complete_frames')
    fresh = delta('Receiver', 'freshness_dropped_frames')
    selected = delta('Receiver', 'decoder_selected_frames')
    invalid = delta('Receiver', 'decoder_invalid_frames')
    not_ready = delta('Receiver', 'decoder_not_ready_frames')
    failed = delta('Receiver', 'decoder_failed_frames')
    presented = delta('Receiver', 'presented_frames')
    encoded = delta('Sender', 'encoded_frames_total')
    sent = delta('Sender', 'udp_sent_frames_total')
    sender_drops = delta('Sender', 'network_queue_dropped_frames')
    gpu_rejects = invalid + not_ready + failed
    # Adjacent stages can have one or two in-flight frames at sample boundaries.
    assembly_residual = started - complete - incomplete
    consumer_residual = complete - fresh - gpu_rejects - presented
    assert abs(assembly_residual) <= 2, assembly_residual
    assert abs(consumer_residual) <= 2, consumer_residual
    valid = [r['Receiver'] for r in samples if r['Receiver']['hud_latency_available'] and r['Receiver']['display_feedback_kind'] == 'etw']
    reports.append({
        'phase': phase, 'budget_mbps': f['BudgetMbps'], 'target_fps': 165,
        'samples': len(samples), 'receiver_seconds': rx_s, 'sender_seconds': tx_s,
        'rx_started_frames': started, 'rx_complete_frames': complete,
        'rx_incomplete_frames_abandoned': incomplete,
        'rx_incomplete_pct_of_started': incomplete * 100 / max(1, started),
        'rx_ready_mailbox_overwritten': fresh,
        'decoder_invalid_frames': invalid, 'decoder_not_ready_frames': not_ready,
        'decoder_failed_frames': failed, 'presented_frames': presented,
        'rx_frame_drop_events': incomplete + fresh + gpu_rejects,
        'rx_present_fps': presented / rx_s, 'rx_complete_fps': complete / rx_s,
        'rx_assembly_boundary_residual_frames': assembly_residual,
        'rx_consumer_boundary_residual_frames': consumer_residual,
        'sender_encoded_frames': encoded, 'sender_completed_send_frames': sent,
        'sender_mailbox_overwritten': sender_drops,
        'sender_encoded_fps': encoded / tx_s, 'sender_completed_send_fps': sent / tx_s,
        'sender_drop_pct_of_encoded': sender_drops * 100 / max(1, encoded),
        'actual_encoded_frame_mean_bytes': delta('Sender', 'encoded_bytes_total') / max(1, encoded),
        'actual_encoded_frame_p95_rolling_median_bytes': median('Sender', 'encoded_frame_bytes_p95'),
        'actual_encoded_frame_max_observed_bytes': max(r['Sender']['encoded_frame_bytes_max'] for r in samples),
        'configured_frame_ceiling_bytes': f['Sender']['max_frame_bytes'],
        'adaptive_frame_ceiling_median_bytes': median('Sender', 'effective_frame_bytes'),
        'udp_send_p50_rolling_median_ms': median('Sender', 'udp_send_p50_ms'),
        'udp_send_p95_rolling_median_ms': median('Sender', 'udp_send_p95_ms'),
        'gpu_ready_wait_p95_rolling_median_ms': median('Sender', 'gpu_readback_p95_ms'),
        'present_gap_p95_rolling_median_ms': median('Receiver', 'present_gap_p95_ms_1s'),
        'present_gap_max_observed_ms': max(r['Receiver']['present_gap_max_ms_1s'] for r in samples),
        'etw_reported_dropped_presents_delta': delta('Receiver', 'etw_dropped_presents'),
        'etw_unmatched_presents_delta': delta('Receiver', 'etw_unmatched_presents'),
        'hud_valid_samples': len(valid),
        'hud_rolling_mean_average_ms': statistics.mean(r['hud_latency_ms'] for r in valid) if valid else None,
        'display_p95_rolling_median_ms': statistics.median(r['source_to_display_p95_ms'] for r in valid) if valid else None,
        'clock_bound_max_ms': max(r['clock_error_bound_ms'] for r in valid) if valid else None,
    })

result = {'passed': True, 'phases': reports,
          'limitations': ['Drop counters count events in each sampled interval; in-flight boundaries are reported separately.',
                          'Incomplete frames may reflect loss, reordering or scheduling; this does not prove physical wire packet loss.',
                          'Completely unseen frames are not identified by receiver started-frame counters.',
                          'Sender and receiver counter windows have separate local durations; their totals must not be directly subtracted.',
                          'ETW drop counts arrive asynchronously and do not prove full panel scanout or pixel response.',
                          'Changing the bandwidth preset changes the codec frame ceiling while preserving 165 FPS and link budget.',
                          'Image quality was not visually validated in the lower-budget case.']}
(root / 'results/165-frame-drops-summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
doc = root / 'docs/165-frame-drops.md'
marker = '\n## 2026-10-06 对照结果\n'
text = doc.read_text(encoding='utf-8-sig').split(marker)[0]
text += marker + '\n以下是每轮实际计数窗口，未收齐包含乱序或调度影响，不能直接称为物理网络丢包率。\n\n'
text += '| 轮次 / 画质预算 | 时长 | 未收齐帧 | 最新帧覆盖 | 解码未就绪 | Present FPS | 实际帧均值 | 发送 P95 中位数 | 全链路浮窗均值 |\n'
text += '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n'
for p in reports:
    text += (f"| {p['phase'] + 1} / {p['budget_mbps']} Mbps | {p['receiver_seconds']:.1f} s | "
             f"{p['rx_incomplete_frames_abandoned']} | {p['rx_ready_mailbox_overwritten']} | "
             f"{p['decoder_not_ready_frames']} | {p['rx_present_fps']:.2f} | "
             f"{p['actual_encoded_frame_mean_bytes']/1024:.1f} KiB | "
             f"{p['udp_send_p95_rolling_median_ms']:.3f} ms | {p['hud_rolling_mean_average_ms']:.3f} ms |\n")
text += '\n编码数据块拒收、解码失败和 ETW dropped Present 的数量详见本机 summary。\n'
text += '浮窗均值是逐秒滚动统计的平均值，非所有帧汇总的延迟中位数；校时界限独立报告。\n'
text += '较低预算未验证视觉质量。测试结束恢复原画质预算，未采用新的固定单帧限制。\n'
doc.write_text(text, encoding='utf-8-sig')
print(json.dumps(result, ensure_ascii=False, indent=2))
