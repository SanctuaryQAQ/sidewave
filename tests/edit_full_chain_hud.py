from pathlib import Path
root=Path(__file__).resolve().parents[1]
def edit(name,pairs):
    p=root/name;s=p.read_text(encoding='utf-8-sig')
    for a,b in pairs:
        if a not in s: raise RuntimeError(f'{name}: missing {a}')
        s=s.replace(a,b)
    p.write_text(s,encoding='utf-8')
edit('native/pyrowave_2k165_gpu_bench.cpp',[
 ('constantFrameRate?"snapshot":"desktop_update"','"desktop_update"'),
 ('// Constant cadence timestamps each sampling attempt, including acquisition\n   // wait. Repeated snapshots must not inherit an old desktop-update time.','// Preserve the actual source update time in repeated frames. System\n   // display feedback counts latency once per distinct displayed source.')])
edit('native/pyrowave_udp_client.cpp',[
 ('auto lastValidLatency = Clock::time_point{};','auto lastValidLatency = Clock::time_point{};auto lastDisplayLatency = Clock::time_point{};'),
 ('(unsigned long long)stats.displaySamples,displayedFps,displayP50','(unsigned long long)(etwFeedback.valid?etwFeedback.count:stats.displaySamples),displayedFps,displayP50'),
 ('const auto latencyAge=lastValidLatency==Clock::time_point{}?uint64_t(60000):uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-lastValidLatency).count());','const auto latencyAge=etwFeedback.valid?uint64_t(etwFeedback.delayMs):(lastDisplayLatency==Clock::time_point{}?uint64_t(60000):uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-lastDisplayLatency).count()));'),
 ('(unsigned long long)stats.displayLatency.size(),encryptedVideo','(unsigned long long)(etwFeedback.valid?etwFeedback.window:stats.displayLatency.size()),encryptedVideo'),
 ('haveClockCalibration&&latencyAge<1000&&!stats.recentLatency.empty()?"true":"false",liveLatencyMs,(!renderer.PresentFlags()&&haveDisplayFeedback)?"system_display":"present_return"','haveClockCalibration&&haveDisplayFeedback&&latencyAge<2500?"true":"false",liveLatencyMs,"system_display"'),
 ('liveStatsWriter.Publish(json,size_t(length));','''length-=2;length+=snprintf(json+length,sizeof(json)-length,",\\\"display_feedback_kind\\\":\\\"%s\\\",\\\"display_latency_endpoint\\\":\\\"source_desktop_update_to_system_display\\\",\\\"display_feedback_delay_ms\\\":%.3f,\\\"etw_unmatched_presents\\\":%llu,\\\"etw_dropped_presents\\\":%llu,\\\"etw_repeated_sources_ignored\\\":%llu}\\n",etwFeedback.valid?"etw":haveDisplayFeedback?"dxgi":"unavailable",etwFeedback.delayMs,(unsigned long long)etwFeedback.unmatched,(unsigned long long)etwFeedback.dropped,(unsigned long long)etwFeedback.repeats);
        if(length<=0||length>=int(sizeof(json)))return;
        liveStatsWriter.Publish(json,size_t(length));'''),
 ('const auto now = Clock::now();\n\t\tDXGI_FRAME_STATISTICS','const auto now = Clock::now();\n        etwFeedback=displayTiming.Poll();\n\t\tDXGI_FRAME_STATISTICS'),
 ('++stats.displaySamples;++liveDisplayed;haveDisplayFeedback=true;lastValidLatency=now;','++stats.displaySamples;++liveDisplayed;haveDisplayFeedback=true;lastDisplayLatency=now;'),
 ('const double overlayFps=haveDisplayFeedback?displayedFps:instantaneousFps;','''if(etwFeedback.valid&&haveClockCalibration){haveDisplayFeedback=true;displayedFps=etwFeedback.fps;}
            else haveDisplayFeedback=!renderer.PresentFlags()&&!stats.displayLatency.empty()&&lastDisplayLatency!=Clock::time_point{}&&now-lastDisplayLatency<std::chrono::seconds(1);
            const double overlayFps=haveDisplayFeedback?displayedFps:instantaneousFps;'''),
 ('''const bool displayEndpoint=!renderer.PresentFlags()&&haveDisplayFeedback&&!stats.displayLatency.empty();
\t\t\tconst auto& latencySamples=displayEndpoint?stats.displayLatency:stats.recentLatency;
\t\t\tif (haveClockCalibration && lastValidLatency != Clock::time_point{} && now - lastValidLatency < std::chrono::seconds(1) && !latencySamples.empty())''','''const auto& latencySamples=stats.displayLatency;
            if(etwFeedback.valid&&haveClockCalibration){
                liveLatencyMs=etwFeedback.mean;
                overlayText=L"FPS "+std::to_wstring(int(overlayFps+0.5))+L"   LAT "+std::to_wstring(int(liveLatencyMs+0.5))+L" ms";
            }
            else if (haveClockCalibration && haveDisplayFeedback && !latencySamples.empty())'''),
 ('(displayEndpoint?L"   LAT ":L"   LAT-P ")','L"   LAT "'),
 ('stats.recentLatency.clear();stats.displayLatency.clear();lastValidLatency=Clock::time_point{};','stats.recentLatency.clear();stats.displayLatency.clear();lastValidLatency=Clock::time_point{};lastDisplayLatency=Clock::time_point{};'),
 ('renderer.PresentVideo();','displayTiming.Present(currentFrame.frame,currentFrame.captureUtcMs,hostOffsetMs,clockErrorMs,haveClockCalibration,snapshotCaptureTimestamps);\n\t\t\t\trenderer.PresentVideo();')])
edit('src/App.cs',[
 ('(displayEndpoint?" · 显示延迟 ":" · 提交延迟 ")','" · 全链路延迟 "'),
 ('Language.Tr(displayEndpoint?"采集到系统显示反馈；不包含面板响应时间。":"LAT-P：台式机采集快照到笔记本 Present 返回；不包含后续扫描输出及面板响应时间。")','Language.Tr("台式机桌面内容更新到笔记本系统显示反馈，包含渲染和显示等待；不包含液晶面板响应时间。")')])
