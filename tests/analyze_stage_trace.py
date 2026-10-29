"""Correlate the same video frame across two native stage traces.

Local durations use each process's precise UTC readings; clock discontinuities
are retained as negative values, never clamped. Cross-PC durations use the
receiver's contemporaneous TLS calibration and retain its uncertainty bound.
T6 is asynchronous CPU observation of a completed GPU fence, not a calibrated
GPU instant. T7 CPU submission can precede T6 due to GPU-side synchronization.
"""
import json,math,sys,csv,bisect
from pathlib import Path
from collections import Counter
root=Path(__file__).resolve().parent.parent
phase=sys.argv[1]
out=root/'results'
def load(path):
 rows=[];bad=0
 for line in path.read_text(encoding='utf-8-sig').splitlines():
  if not line:continue
  try:rows.append(json.loads(line))
  except json.JSONDecodeError:bad+=1
 return rows,bad
send,bad_s=load(out/f'{phase}-sender.jsonl')
recv,bad_r=load(out/f'{phase}-receiver.jsonl')
stats,_=load(out/f'bandwidth-{phase}-0-900.jsonl')
low=min(v['sampled_utc_ms'] for v in stats)*1000
high=max(v['sampled_utc_ms'] for v in stats)*1000
by_send={v['frame']:v for v in send if v['type']=='sender'}
by_recv={v['frame']:v for v in recv if v['type']=='receiver' and low<=v['t4_complete_us']<=high}
by_display={v['frame']:v for v in recv if v['type']=='display'}
by_fence={v['frame']:v for v in recv if v['type']=='decode_fence'}
by_render_fence={v['frame']:v for v in recv if v['type']=='render_fence'}
by_gpu={v['frame']:v for v in recv if v['type']=='gpu' and v['valid']}
metrics={};counts=Counter();paired=[]
etw_file=out/f'{phase}-presentmon.csv'
etw=[];etw_qpc=[];etw_modes=Counter();frequency=0
if etw_file.exists():
 metadata=json.loads((out/f'{phase}-etw-metadata.json').read_text(encoding='utf-8-sig'));frequency=metadata['QpcFrequency']
 with etw_file.open(encoding='utf-8-sig',newline='') as f:
  for row in csv.DictReader(f):
   if int(row['ProcessID'])==metadata['ReceiverPid']:etw.append(row)
 etw.sort(key=lambda r:int(r['QPCTime']));etw_qpc=[int(r['QPCTime']) for r in etw]
def add(name,value):metrics.setdefault(name,[]).append(value/1000.0)
for frame,r in by_recv.items():
 s=by_send.get(frame)
 if not s:counts['missing_sender']+=1;continue
 counts['matched']+=1;paired.append((s,r))
 for name,a,b in [('source_to_capture','source_us','t0_us'),('capture_to_encode_submit','t0_us','t1_us'),('encode_call_cpu','t1_us','t1_return_us'),('encode_submit_to_result_ready','t1_us','t2_ready_us'),('result_ready_to_packetized','t2_ready_us','packetized_us'),('sender_pending_queue','published_us','dequeued_us'),('encoded_to_first_udp_submit','t2_ready_us','t3_first_submit_us'),('udp_submit_span','t3_first_submit_us','t3_last_submit_us'),('udp_send_completion_span','t3_first_submit_us','t3_completion_us')]:add(name,s[b]-s[a])
 add('capture_acquire_wait',s['acquire_wait_us']);add('capture_pacing_wait',s['pacing_wait_us'])
 if s['accumulated']>1:counts['capture_accumulated_gt1']+=1
 if s['dequeued_us']-s['published_us']>=1e6/165:counts['sender_pending_ge_one_period']+=1
 for name,a,b in [('frame_receive_assembly','t4_first_us','t4_complete_us'),('receiver_mailbox_queue','t4_complete_us','selected_us'),('pre_decode_submission_cpu','push_start_us','t5_us'),('decode_submit_cpu','t5_us','t5_return_us'),('render_submit_cpu','t7_submit_us','t8_call_us'),('present_call_block','t8_call_us','t8_return_us')]:add(name,r[b]-r[a])
 add('complete_receive_to_decode_submit',r['t5_us']-r['t4_complete_us'])
 if r['selected_us']-r['t4_complete_us']>=1e6/165:counts['receiver_queue_ge_one_period']+=1
 f=by_fence.get(frame)
 rf=by_render_fence.get(frame)
 if rf:
  counts['render_fence_matched']+=1
  add('present_to_render_fence_observation_signed',rf['t6_observed_us']-r['t8_call_us'])
 if f:
  counts['fence_matched']+=1
  add('decode_submit_to_fence_observation_upper_bound',f['t6_observed_us']-r['t5_us'])
  add('render_cpu_submit_minus_fence_observation_signed',r['t7_submit_us']-f['t6_observed_us'])
  if r['t7_submit_us']<f['t6_observed_us']:counts['render_submitted_before_fence_observation']+=1
 g=by_gpu.get(frame)
 if g:
  counts['gpu_query_matched']+=1
  add('gpu_bridge_decode_span',g['bridge_decode_us']);add('gpu_render_span',g['render_us'])
 if r['calibrated']:
  off=r['clock_offset_us'];counts['calibrated_matched']+=1
  add('first_udp_submit_to_first_receive_signed',r['t4_first_us']-off-s['t3_first_submit_us'])
  add('first_udp_submit_to_complete_receive',r['t4_complete_us']-off-s['t3_first_submit_us'])
  add('capture_to_present_return',r['t8_return_us']-off-s['t0_us'])
  add('source_update_to_present_return',r['t8_return_us']-off-s['source_us'])
 d=by_display.get(frame)
 if d:
  counts['display_matched']+=1
  add('present_call_to_display_feedback',d['display_us']-r['t8_call_us'])
  add('present_return_to_display_feedback_signed',d['display_us']-r['t8_return_us'])
  if d['calibrated']:
   add('capture_to_display_feedback',d['display_us']-d['clock_offset_us']-s['t0_us'])
   add('source_update_to_display_feedback',d['display_us']-d['clock_offset_us']-s['source_us'])
  if f:add('dxgi_display_minus_fence_observation_signed',d['display_us']-f['t6_observed_us'])
 if etw and 't8_qpc' in r:
  index=bisect.bisect_left(etw_qpc,r['t8_qpc'])
  candidates=range(max(0,index-1),min(len(etw),index+2))
  match=min(candidates,key=lambda j:abs(etw_qpc[j]-r['t8_qpc']))
  delta=(etw_qpc[match]-r['t8_qpc'])*1e6/frequency
  if abs(delta)>500:counts['etw_unmatched']+=1;continue
  e=etw[match];counts['etw_matched']+=1;etw_modes[e['PresentMode']]+=1
  add('etw_match_qpc_delta',delta)
  add('etw_present_api',float(e['msInPresentAPI'])*1000)
  if e['Dropped']!='0':counts['etw_dropped']+=1;continue
  if e['msUntilDisplayed'] in ('NA',''):counts['etw_display_unavailable']+=1;continue
  displayed=r['t8_call_us']+delta+float(e['msUntilDisplayed'])*1000
  counts['etw_display_matched']+=1
  add('present_to_etw_display',float(e['msUntilDisplayed'])*1000)
  if e['msUntilRenderComplete'] not in ('NA',''):add('present_to_etw_render_complete',float(e['msUntilRenderComplete'])*1000)
  if r['calibrated']:
   add('capture_to_etw_display',displayed-r['clock_offset_us']-s['t0_us'])
   add('source_update_to_etw_display',displayed-r['clock_offset_us']-s['source_us'])
  if f:
   add('etw_display_minus_fence_observation_signed',displayed-f['t6_observed_us'])
   if displayed-f['t6_observed_us'] < -1000:counts['etw_display_before_observed_fence_gt1ms']+=1
  if rf:
   add('render_fence_observation_to_etw_display_signed',displayed-rf['t6_observed_us'])
def summary(values):
 v=sorted(values)
 return {'n':len(v),'p50_ms':v[math.ceil(len(v)*.50)-1],'p95_ms':v[math.ceil(len(v)*.95)-1],'p99_ms':v[math.ceil(len(v)*.99)-1],'min_ms':v[0],'max_ms':v[-1],'negative':sum(x<0 for x in v)}
result={'phase':phase,'counts':dict(counts),'clock_bound_max_ms':max(r['clock_bound_us'] for _,r in paired)/1000,'malformed_lines':[bad_s,bad_r],'trace_dropped_max':max(v.get('trace_dropped',0) for v in send+recv),'metrics':{k:summary(v) for k,v in metrics.items()},'receiver_types':dict(Counter(v['type'] for v in recv)),'receiver_gpu_profiles':[v for v in recv if v['type']=='gpu_profile'][-16:],'sender_gpu_profiles':[v for v in send if v['type']=='gpu_profile'][-16:],'present_flags':stats[-1].get('present_flags'),'caveats':['T2 is CPU observation that the encoder result is readable, including GPU staging copy.','T3 is Winsock submission/completion, not a hardware wire timestamp.','T4 is application receive/completed assembly, not NIC arrival.','T6 fence observation is an upper bound including observer scheduling delay.','GPU bridge span includes D3D/Vulkan synchronization, not isolated compute shader time.','CPU rendering submission overlaps decode; the T6/T7 signed difference is not a serial stage.','Cross-PC timing uncertainty is reported, not clamped away.','DXGI feedback excludes physical LCD response.','Per-stage percentile sums do not equal the full-chain percentile.']}
result['etw_present_modes']=dict(etw_modes)
result['dxgi_feedback_eligible_for_latency']=result['present_flags']==0
if result['present_flags']:
 result['caveats'].append('Refresh-counter-based DXGI extrapolation is invalid with vsync-off in this experiment; do not use its display latency values.')
(out/f'{phase}-summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
