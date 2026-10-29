from pathlib import Path
root=Path(__file__).resolve().parents[1]
def edit(name,pairs):
    p=root/name
    s=p.read_text(encoding='utf-8-sig')
    for a,b in pairs:
        if a not in s: raise RuntimeError(f'{name}: missing {a}')
        s=s.replace(a,b)
    p.write_text(s,encoding='utf-8')
edit('native/pyrowave_udp_client.cpp',[
    (r'\"latency_endpoint\":\"source_desktop_update_to_present_return\"',r'\"latency_endpoint\":\"%s\"'),
    ('screenWidth, screenHeight, (unsigned long)screenMode.dmDisplayFrequency);','screenWidth, screenHeight, (unsigned long)screenMode.dmDisplayFrequency, snapshotCaptureTimestamps?"capture_snapshot_to_present_return":"source_desktop_update_to_present_return");')])
edit('src/Sessions.cs',[
    ('supportsPlainVideo=true,supportsTearingMode=true','supportsPlainVideo=true,supportsTearingMode=true,supportsSnapshotCadence=true'),
    ('appliedTearing,tearingSupported;','appliedTearing,tearingSupported,snapshotSupported;'),
    ('plainSupported=Json.S(challenge','snapshotSupported=Json.S(challenge,"supportsSnapshotCadence")=="True";plainSupported=Json.S(challenge'),
    ('// Keep both the authenticated','if(!snapshotSupported)throw new IOException("Receiver update required for constant frame cadence.");\n  // Keep both the authenticated'),
    (' · 上限 ',' · 目标 ')])
old_tip='减少呈现等待；可能出现画面撕裂。此模式的全链路延迟需要系统级采样，浮窗显示 --。点应用后生效。'
tip='减少呈现等待；可能出现画面撕裂。LAT-P 显示采集到提交显示的延迟，不包含后续扫描输出与面板响应。点应用后生效。'
old_note='带宽优先用于画质，帧率受两端屏幕限制。'
note='按所选帧率持续发送；屏幕实际刷新由硬件决定。'
edit('src/App.cs',[('有效上限 ','目标 '),(old_note,note)])
edit('src/MainWindow.xaml',[('帧率上限','目标帧率'),(old_note,note),(old_tip,tip),('实际帧率取决于画面更新与两端屏幕。','静止画面也持续发送；实际刷新受屏幕硬件限制。')])
edit('src/Localization.cs',[
    (old_tip,tip),
    ('Reduce presentation waits; tearing is possible. Full-chain latency needs system tracing in this mode, so the HUD shows --. Click Apply to use it.','Reduce presentation waits; tearing is possible. LAT-P measures capture to Present return, excluding scanout and panel response. Click Apply to use it.'),
    ('帧率上限','目标帧率'),('Frame rate limit','Target frame rate'),(old_note,note),
    ('Bandwidth improves quality; both displays limit FPS.','Send continuously at the selected rate; physical refresh depends on the panel.'),
    ('有效上限 ','目标 '),('Effective limit ','Target '),(' · 上限 ',' · 目标 '),(' · Limit ',' · Target '),
    ('实际帧率取决于画面更新与两端屏幕。','静止画面也持续发送；实际刷新受屏幕硬件限制。'),
    ('Actual FPS depends on desktop updates and both displays.','Static frames are sent continuously; physical refresh depends on the panel.'),
    ('{"原生 ","Native "}', '{" · 提交延迟 "," · Submit latency "},{" · 显示延迟 "," · Display latency "},\n  {"采集到系统显示反馈；不包含面板响应时间。","Capture to system display feedback; excludes panel response."},\n  {"LAT-P：台式机采集快照到笔记本 Present 返回；不包含后续扫描输出及面板响应时间。","LAT-P: desktop capture snapshot to notebook Present return; excludes scanout and panel response."},\n  {"原生 ","Native "}')])
