import json, statistics
from pathlib import Path
root=Path(__file__).resolve().parents[1]
rows=json.loads((root/'results/constant-cadence-samples.json').read_text(encoding='utf-8-sig'))
def pct(xs,p):
    xs=sorted(xs)
    return xs[int((len(xs)-1)*p)]
summary=[]
for target in sorted({r['Target'] for r in rows}):
    for phase in ('scroll','idle'):
        group=[r for r in rows if r['Target']==target and r['Phase']==phase]
        if not group: raise RuntimeError('Missing test phase')
        item={'target':target,'phase':phase,'samples':len(group)}
        first,last=group[0]['Receiver'],group[-1]['Receiver']
        span=last['elapsed_s']-first['elapsed_s']
        item['counter_span_s']=round(span,3)
        item['counter_present_fps']=round((last['presented_frames']-first['presented_frames'])/span,3)
        item['counter_receive_fps']=round((last['complete_frames']-first['complete_frames'])/span,3)
        for name,side,key in [('capture_fps','Sender','capture_fps'),('send_fps','Sender','udp_send_fps'),('present_fps','Receiver','present_fps_1s'),('submit_latency_ms','Receiver','hud_latency_ms'),('present_gap_p95_ms','Receiver','present_gap_p95_ms_1s'),('repeat_fps','Sender','repeated_capture_fps'),('wire_mbps','Sender','estimated_wire_mbps')]:
            values=[r[side][key] for r in group]
            item[name]={'mean':round(statistics.mean(values),3),'p05':pct(values,.05),'p50':pct(values,.5),'p95':pct(values,.95),'min':min(values),'max':max(values)}
        item['hud_available_samples']=sum(r['Receiver']['hud_latency_available'] for r in group)
        item['sender_dropped_delta']=group[-1]['Sender']['network_queue_dropped_frames']-group[0]['Sender']['network_queue_dropped_frames']
        item['receiver_freshness_dropped_delta']=group[-1]['Receiver']['freshness_dropped_frames']-group[0]['Receiver']['freshness_dropped_frames']
        summary.append(item)
(root/'results/constant-cadence-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
for r in summary:
    print(r['target'],r['phase'], 'capture',r['capture_fps']['mean'],'send',r['send_fps']['mean'],'present',r['present_fps']['mean'],'latency',r['submit_latency_ms']['p50'],r['submit_latency_ms']['p95'],'hud',r['hud_available_samples'])
