import json,statistics,sys
from pathlib import Path
root=Path(__file__).resolve().parent.parent
rows=[]
for pattern in sys.argv[1:]:
 for f in sorted((root/'results').glob(pattern)):
  if f.name.endswith('-sender.jsonl'):continue
  data=list({(v:=json.loads(x))['sampled_utc_ms']:v for x in f.read_text(encoding='utf-8-sig').splitlines() if x}.values())
  if len(data)<2:continue
  a,b=data[0],data[-1]
  valid=[x for x in data if x.get('display_feedback_available') and x.get('display_latency_window_samples',0)>0]
  row={'file':f.name,'samples':len(data),'seconds':b['elapsed_s']-a['elapsed_s'],'actual_fps':(b['presented_frames']-a['presented_frames'])/(b['elapsed_s']-a['elapsed_s']),'rolling_p50_median_ms':statistics.median(x['source_to_display_p50_ms'] for x in valid) if valid else None,'rolling_p95_median_ms':statistics.median(x['source_to_display_p95_ms'] for x in valid) if valid else None,'max_gap_ms':max(x['present_gap_max_ms_1s'] for x in data),'clock_bound_ms':max(x['clock_error_bound_ms'] for x in data),'waitable_present':b['waitable_present'],'freshness_drops':b['freshness_dropped_frames']-a['freshness_dropped_frames']}
  if 'receive_transport' in b:row['receive_transport']=b['receive_transport']
  if 'receive_coalescing_enabled' in b:
   row['receive_coalescing_enabled']=b['receive_coalescing_enabled']
   row['coalesced_messages']=b['coalesced_messages']-a['coalesced_messages']
   row['coalesced_datagrams']=b['coalesced_datagrams']-a['coalesced_datagrams']
   row['max_receive_batch']=max(x['max_receive_batch'] for x in data)
  sf=f.with_name(f.stem+'-sender.jsonl')
  if sf.exists():
   sender=[json.loads(x) for x in sf.read_text(encoding='utf-8-sig').splitlines() if x]
   row.update({k+'_median':statistics.median(x[k] for x in sender) for k in ['capture_fps','udp_send_fps','udp_mbps','gpu_readback_p95_ms','udp_send_p95_ms','frame_to_udp_p95_ms']})
   row['network_queue_drops']=sender[-1]['network_queue_dropped_frames']-sender[0]['network_queue_dropped_frames']
   row['send_connected']=sender[-1]['send_connected']
  rows.append(row)
print(json.dumps(rows,ensure_ascii=False,indent=2))
