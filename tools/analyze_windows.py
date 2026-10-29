import argparse,collections,json,statistics
from pathlib import Path
parser=argparse.ArgumentParser(description="Summarize genuine receiver frame counters and rolling latency windows.")
parser.add_argument("input",type=Path);parser.add_argument("--output",type=Path)
args=parser.parse_args()
rows=[json.loads(line.lstrip("\ufeff"))for line in args.input.read_text(encoding="utf-8-sig").splitlines()if line.strip()]
rows=[row for row in rows if isinstance(row,dict)and "pid"in row and "presented_frames"in row and row.get("elapsed_s",0)>=5]
if not rows:raise SystemExit("No valid receiver samples.")
pid=collections.Counter(row["pid"]for row in rows).most_common(1)[0][0]
rows=sorted({(row["pid"],row["sampled_utc_ms"]):row for row in rows if row["pid"]==pid}.values(),key=lambda row:row["elapsed_s"])
if len(rows)<2:raise SystemExit("Need two distinct live samples.")
first,last=rows[0],rows[-1];seconds=last["elapsed_s"]-first["elapsed_s"]
valid=[row for row in rows if row.get("display_latency_window_samples",0)>0 and row.get("last_latency_sample_age_ms",5000)<1000]
report=dict(file=args.input.name,pid=pid,samples=len(rows),seconds=seconds,
 actualFPS=(last["presented_frames"]-first["presented_frames"])/seconds,
 latencyRollingP50Median=statistics.median(row["source_to_display_p50_ms"]for row in valid)if valid else None,
 latencyRollingP95Median=statistics.median(row["source_to_display_p95_ms"]for row in valid)if valid else None,
 worstRollingP95=max((row["source_to_display_p95_ms"]for row in valid),default=None),
 worstPresentGap=max(row["present_gap_max_ms_1s"]for row in rows),
 windowsWithGapAbove20ms=sum(row["present_gap_max_ms_1s"]>20 for row in rows),
 windowsWithGapAbove50ms=sum(row["present_gap_max_ms_1s"]>50 for row in rows),
 incompletePackets=last["incomplete_packets"]-first["incomplete_packets"],
 freshnessDrops=last["freshness_dropped_frames"]-first["freshness_dropped_frames"],
 rendererVendor=last["renderer_vendor"],
 clockBoundMin=min(row["clock_error_bound_ms"]for row in valid)if valid else None,
 clockBoundMax=max(row["clock_error_bound_ms"]for row in valid)if valid else None,
 note="Rolling window summaries, not pooled frame quantiles; FPS uses monotonic receiver elapsed time.")
text=json.dumps(report,ensure_ascii=False,indent=2);print(text)
if args.output:args.output.write_text(text,encoding="utf-8")
