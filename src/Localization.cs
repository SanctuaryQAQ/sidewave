using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Automation;

namespace PyroScreen {
// UI language is local to each computer and independent of video settings.
static class Language {
 public static string Code="zh";
 public static void Load(){
  // Without a saved choice the UI follows the Windows display language.
  string system=System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName=="zh"?"zh":"en";
  string file=Path.Combine(Paths.Root,"ui.json");
  try{Code=File.Exists(file)?(Json.S(Json.Read(File.ReadAllText(file)),"language",system)=="en"?"en":"zh"):system;}catch{Code=system;}
 }
 public static void Select(string code){
  string next=code=="en"?"en":"zh";
  Paths.Atomic(Path.Combine(Paths.Root,"ui.json"),Json.Write(new{language=next}));Code=next;
 }
 static readonly Dictionary<string,string> English=new Dictionary<string,string>{
  {"检查网络类型与防火墙规则，修复连接配置。","Check network profiles and firewall rules; repair connection setup."},
  {"缺少配置组件：","Missing setup component: "},{"检查或配置超时，请查看诊断日志。","Check or setup timed out. See diagnostic logs."},
  {"网络连接","Network connections"},{"未检测到网络连接。","No network connection detected."},
  {"没有专用网络，专用网络规则不会生效。请打开 Windows 网络设置，仅将可信局域网设为专用。","No private network is active. Open Windows network settings and mark only a trusted LAN as Private."},
  {"防火墙规则（本软件路径 · 专用网络 · 本地子网）","Firewall rules (this executable · Private network · local subnet)"},
  {"已验证","Verified"},{"缺失或配置不匹配","Missing or mismatched"},
  {"规则缺失或路径变化时，点击“修复网络配置”。两台电脑都需要检查。","Click Repair network setup for missing rules or a changed app path. Check both computers."},
  {"显示延迟采样：","Display latency sampler: "},{"已安装","Installed"},{"未安装","Not installed"},
  {"虚拟显示驱动","Virtual display driver"},{"当前发送后端：","Sender backend: "},
  {"签名 VDD 驱动包未安装。","The signed VDD driver package is not installed."},
  {"LanScreen 驱动包已安装。","LanScreen driver package is installed."},{"未找到 LanScreen 驱动包。","LanScreen driver package was not found."},
  {"安装按钮配置签名 VDD 驱动；使用 LanScreen 后端时需保留现有驱动。","The install button sets up the signed VDD driver. Retain the existing driver when using the LanScreen backend."},
  {"驱动包已安装。连接副机时才创建虚拟屏，空闲时没有设备属于正常状态。","Driver package is installed. A virtual display is created on connection; no device while idle is normal."},
  {"未找到虚拟显示驱动包，请点击“安装 / 修复驱动”。","Virtual display driver package was not found. Click Install / repair driver."},
  {"接收模式无需安装虚拟显示驱动。","Receive mode does not require a virtual display driver."},
  {"当前运行状态","Current runtime state"},{"模式：","Mode: "},{"连接：","Connection: "},{"视频：","Video: "},{"运行中","Running"},{"未运行","Stopped"},{"状态：","Status: "},
  {"遥测距今（ms）：","Telemetry age (ms): "},{"当前没有新鲜视频遥测，下方历史值不能用于判断当前画面。","No fresh video telemetry is available. Historical values below do not describe the current video."},
  {"视频遥测：","Video telemetry: "},{"程序路径：","Executable: "},{"日志路径：","Logs: "},{"检查未完成：","Check incomplete: "},
  {"正在检查…","Checking…"},{"重新检查","Check again"},{"修复网络配置","Repair network setup"},{"Windows 网络设置","Windows network settings"},
  {"安装 / 修复驱动","Install / repair driver"},{"显示设置","Display settings"},{"导出报告","Export report"},{"打开日志","Open logs"},{"关闭","Close"},{"报告已保存：","Report saved: "},
  {"已取消 Windows 权限请求。","Windows permission request was canceled."},
  {"已验证本软件的三条专用网络防火墙规则。公共网络仍需在 Windows 设置中改为专用网络。","Verified three Private-network firewall rules for this app. Public networks must still be changed to Private in Windows settings."},
  {"显示延迟采样已安装。","Display latency sampler installed."},{"防火墙已配置；显示延迟采样安装失败：","Firewall configured; display latency sampler setup failed: "},
  {"已验证 Windows 驱动库中的虚拟显示驱动。连接副机时创建副屏，断开后释放。","Verified the virtual display driver in the Windows driver store. A display is created on connection and released on disconnect."},
  {"Windows 要求重新启动后才能使用驱动。","Windows requires a restart before the driver can be used."},
  {"请使用当前登录用户的管理员权限安装显示采样；其他账户不能为当前用户配置采样。","Install display sampling with the current user's administrator permission; another account cannot configure this user's sampler."},
  {"发送器意外退出，正在准备恢复画面…","Sender exited unexpectedly; preparing video recovery…"},
  {"发送器连续退出，点击应用可重试。","Sender keeps exiting. Click Apply to retry."},
  {"正在恢复画面（","Recovering video ("},{"/3）…","/3)…"},
  {"发送器已重启 · 保持原连接和虚拟屏","Sender restarted · Connection and virtual display retained"},
  {"画面恢复失败，点击应用可重试：","Video recovery failed. Click Apply to retry: "},
  {"画面恢复失败，稍后重试：","Video recovery failed; retrying shortly: "},
  {"副机视频组件已退出，点击应用可重试：","Receiver video component exited. Click Apply to retry: "},
  {"低延迟显示（允许撕裂）","Low latency display (allow tearing)"},
  {"副机硬件光标（实验）","Receiver hardware cursor (experimental)"},
  {"主机鼠标位置和形状单独发送，副机用自己的系统硬件光标显示，不再等待视频编码和解码；画面里不再合成主机光标。副机切到其他程序时不移动其鼠标。需要副机也升级。点应用后生效。","Sends the sender's pointer position and shape separately so the receiver draws it with its own hardware cursor, without waiting for video encode and decode; the pointer is no longer composited into the video. The receiver's mouse is not moved while another app is in front. The receiver must also be updated. Takes effect after Apply."},
  {"只传输变化区域（降低延迟）","Send changed regions only (lower latency)"},
  {"桌面只有局部变化时只编码并发送变化区域，减少传输时间和带宽；全屏运动时自动发送整帧。需要副机也升级到支持此功能的版本。点应用后生效。","When only part of the desktop changes, encode and send just that region to cut transfer time and bandwidth; full-screen motion still sends whole frames. The receiver must also be updated. Takes effect after Apply."},
  {"变化区域传输设置已更改，点击“应用画面设置”生效。","Changed-region setting updated. Click Apply display settings."},
  {"副机版本不支持只传输变化区域，已使用整帧传输。","The receiver does not support changed-region frames; sending whole frames."},
  {"减少呈现等待；可能出现画面撕裂。LAT 使用系统显示采样，包含渲染和显示等待；不包含面板响应。点应用后生效。","Reduce presentation waits; tearing is possible. LAT uses system display feedback, including rendering and display waits; excludes panel response. Click Apply to use it."},
  {"配置专用网络与全链路延迟采样，需要 Windows 权限。","Configure private-network access and end-to-end latency sampling; requires Windows permission."},
  {"低延迟显示设置已更改，点击“应用画面设置”生效。","Low latency display preference changed. Click Apply display settings to use it."},
  {"视频加密","Video encryption"},{"关闭后使用明文 UDP 视频，减少逐包加解密开销；点应用后生效。","Disable for plaintext UDP video without per-packet encryption. Click Apply to use it."},
  {"视频加密设置已更改，点击“应用画面设置”生效。","Video encryption preference changed. Click Apply display settings to use it."},
  {"TLS 控制 · UDP 视频（加密可选）","TLS control · UDP video (optional encryption)"},
  {"发送模式","Send"},{"接收模式","Receive"},
  {"可用副机","Available receivers"},{"刷新","Refresh"},{"搜索中…","Searching…"},
  {"暂无设备","No devices found"},{"副机需处于接收模式，并已允许专用网络。","The receiver must be in Receive mode with the private network allowed."},{"传输选项","Transport options"},
  {"IP 地址","IP address"},{"副机IP地址","Receiver IP address"},{"首次连接的配对码","Pairing code for first connection"},{"配对码（首次连接）","Pairing code (first time)"},
  {"配对码","Pairing code"},{"也可以从设备列表选择","Or select a device from the list"},
  {"输入副机显示的配对码；已配对设备留空","Enter the receiver's code; leave blank for a paired device"},
  {"连接副机","Connect"},{"断开","Disconnect"},{"画面设置","Display settings"},
  {"分辨率","Resolution"},{"副机原生分辨率","Receiver native resolution"},
  {"目标帧率","Target frame rate"},{"帧率","Frame rate"},{"画质 / 带宽预算","Quality / bandwidth budget"},
  {"均衡 · 400 Mbps","Balanced · 400 Mbps"},{"清晰 · 600 Mbps","Clear · 600 Mbps"},{"高画质 · 750 Mbps","High quality · 750 Mbps"},{"极高画质 · 900 Mbps","Ultra quality · 900 Mbps"},
  {"按所选帧率持续发送；屏幕实际刷新由硬件决定。","Send continuously at the selected rate; physical refresh depends on the panel."},
  {"目标 ","Target "},{" FPS · 线路预算 "," FPS · Link budget "},
  {"网卡连接低于千兆（本机 ","Network link below gigabit (local "},{" / 副机 "," / receiver "},{" Mbps），帧率和画质会受限。请检查网线后重新连接。"," Mbps); frame rate and quality are limited. Check the cable, then reconnect."},
  {"带宽预算必须在 50–2500 Mbps 范围内。","Bandwidth budget must be between 50 and 2500 Mbps."},
  {"应用画面设置","Apply display settings"},
  {"切换参数保留控制连接，画面会短暂停顿。\n静止画面也持续发送；实际刷新受屏幕硬件限制。","Settings keep the control connection; video briefly pauses.\nStatic frames are sent continuously; physical refresh depends on the panel."},
  {"启动时自动连接上次副机","Auto-connect last receiver"},
  {"丢包保护（增加少量流量）","Packet loss protection (FEC)"},{"丢包保护（FEC）","Packet loss protection (FEC)"},
  {"每个视频数据包增加一片校验数据，可恢复一片丢失；点应用后生效。","Adds one XOR parity fragment per codec packet, recovering one missing fragment. Click Apply to enable."},
  {"让这台电脑成为副屏","Use this computer as a receiver"},{"本机地址：","Local addresses: "},
  {"首次配对码 · 10 分钟有效","First pairing code · Valid for 10 minutes"},{"客户端配对码","Receiver pairing code"},
  {"复制配对码","Copy code"},{"刷新配对码","New code"},
  {"配对后等待主机连接，即可显示真实扩展桌面。\n主机停止或断开后返回本机桌面。窗口关闭后仍在托盘等待。\n本机鼠标默认隐藏，移动后显示，两秒静止后隐藏。","Pair with a sender to display its extended desktop.\nReturns to the local desktop when the sender stops or disconnects. Closing this window leaves the app in the tray.\nThe local cursor appears on movement and hides after two seconds of inactivity."},
  {"随 Windows 启动（使用上次模式）","Start with Windows (last mode)"},
  {"登录后在托盘运行上次模式；发送模式使用已保存的自动连接设置。","Start in the tray after sign-in using the last mode and saved auto-connect preference."},
  {"网络设置","Network setup"},{"虚拟屏驱动","Display driver"},{"诊断","Diagnostics"},{"忘记配对","Forget pairing"},{"退出","Exit"},{"打开","Open"},
  {"正在初始化…","Initializing…"},{"FPS --  ·  延迟 -- ms","FPS --  ·  Latency -- ms"},{"FPS -- · 延迟 -- ms","FPS -- · Latency -- ms"},
  {"TLS 1.2 控制 · AES-256-GCM 视频","TLS 1.2 control · AES-256-GCM video"},
  {"正在打开诊断文件夹…","Opening diagnostics…"},
  {"删除已配对设备，下一次连接需要重新输入配对码。","Remove saved pairings? The next connection will require a pairing code."},
  {"配对已删除","Pairings removed"},{"软件继续在托盘运行。使用“退出”结束副屏。","SideWave is still running in the tray. Choose Exit to stop it."},
  {"配对码已复制。","Pairing code copied."},{"配对码已刷新，10 分钟内有效。","New pairing code generated; valid for 10 minutes."},
  {"已开启随 Windows 启动，使用上次模式。","Windows startup enabled, using the last mode."},{"已关闭随 Windows 启动。","Windows startup disabled."},
  {"无法修改自启动：","Could not change Windows startup: "},
  {"已开启启动时自动连接上次副机。","Auto-connect enabled."},{"已关闭自动连接。","Auto-connect disabled."},
  {"丢包保护设置已更改，点击“应用画面设置”生效。","FEC preference changed. Click Apply display settings to use it."},
  {"正在搜索局域网副机…","Searching for LAN receivers…"},
  {"未找到副机，请确认副机处于接收模式并允许专用网络。","No receivers found. Check receiver mode and Private network firewall access."},
  {"已找到 ","Found "},{" 台副机。"," receiver(s)."},
  {"正在安全连接副机…","Connecting securely…"},{"正在断开副机…","Disconnecting…"},{"已断开","Disconnected"},
  {"正在应用画面设置…","Applying display settings…"},{"选择局域网副机，或输入 IP 地址","Select a LAN receiver or enter an IP address"},
  {"切换失败：","Settings change failed: "},{" 正在恢复原模式。"," Restoring previous settings."},{"恢复失败：","Restore failed: "},
  {"正在连接…","Connecting…"},{"正在断开…","Disconnecting…"},{"正在应用…","Applying…"},
  {"副机已返回本地桌面，点击应用可继续。","The receiver returned to its local desktop. Click Apply to resume."},
  {"连接中断，正在等待副机：","Connection lost; waiting for the receiver: "},
  {"正在打开配置，请完成 Windows 权限提示…","Opening setup; complete the Windows permission prompt…"},
  {"配置完成。","Setup completed."},{"配置未完成，请查看诊断。","Setup did not complete. Check diagnostics."},{"配置未启动：","Could not start setup: "},
  {"正在切换到接收模式…","Switching to Receive mode…"},{"正在切换到发送模式…","Switching to Send mode…"},
  {"切换模式失败","Mode switch failed"},{"不支持的虚拟屏驱动。","Unsupported virtual display driver."},
  {"SideWave 已在运行：","SideWave is already running: "},{"SideWave 启动失败","SideWave startup failed"},
  {"等待局域网主机连接","Waiting for a LAN sender"},{"连接已结束：","Connection ended: "},
  {"协议版本不匹配。","Protocol version mismatch."},{"设备身份无效。","Invalid device identity."},
  {"配对码已过期，请刷新客户端配对码。","Pairing code expired. Generate a new code on the receiver."},
  {"配对尝试过多，请稍后重试。","Too many pairing attempts. Try again later."},
  {"配对码或设备凭据不匹配。","Pairing code or device credentials do not match."},
  {"客户端正被另一台主机使用。","The receiver is in use by another sender."},
  {"已配对并连接 · ","Paired and connected · "},{"不支持的显示适配器设置。","Unsupported renderer adapter setting."},
  {"副屏运行 · ","Extended display running · "},{"保持连接 · 视频已停止","Connected · Video stopped"},
  {"等待主机 · 上次会话已释放","Waiting for a sender · Previous session released"},
  {"请输入有效的 IPv4 地址。","Enter a valid IPv4 address."},
  {"连接超时，请检查副机软件与专用网络防火墙。","Connection timed out. Check the receiver and Private network firewall."},
  {"对方不是兼容的 SideWave 客户端。","The remote device is not a compatible SideWave receiver."},
  {"设备证书已改变。请忘记旧配对后重新确认副机。","The device certificate changed. Forget its old pairing and pair again."},
  {"配对码错误或设备身份不匹配。","Incorrect pairing code or mismatched device identity."},{"配对失败。","Pairing failed."},
  {"已连接 ","Connected to "},{"请先连接副机。","Connect to a receiver first."},{"画面设置已是当前值。","These display settings are already active."},
  {" · 目标 "," · Target "},{" FPS · 虚拟屏 "," FPS · Virtual display "},{" Hz · 副机 "," Hz · Receiver "},
  {" · 提交延迟 "," · Submit latency "},{" · 显示延迟 "," · Display latency "},
  {" · 全链路延迟 "," · End-to-end latency "},
  {"台式机桌面内容更新到笔记本系统显示反馈，包含渲染和显示等待；不包含液晶面板响应时间。","Desktop source update to receiver system display feedback, including rendering and display waits; excludes physical panel response."},
  {"采集到系统显示反馈；不包含面板响应时间。","Capture to system display feedback; excludes panel response."},
  {"LAT-P：台式机采集快照到笔记本 Present 返回；不包含后续扫描输出及面板响应时间。","LAT-P: desktop capture snapshot to notebook Present return; excludes scanout and panel response."},
  {"原生 ","Native "},{" · 延迟 "," · Latency "},{" ms · 时钟 ±"," ms · Clock ±"},
  {"无法更新配置或诊断文件：","Could not update the configuration or diagnostic file: "},
  {"配置内容为空。","Configuration is empty."},{"无法读取 ","Could not read "},
  {" 配置，请打开诊断。原有配对不会被覆盖。"," configuration. Open diagnostics. Existing pairings are preserved."},
  {"配对码应为客户端显示的 16 位字符。","Enter the 16-character code shown on the receiver."},
  {"无法启动视频引擎，请打开诊断。","Could not start the video engine. Open diagnostics."},
  {"视频引擎已退出，请查看诊断日志。","The video engine exited. Check diagnostics."},
  {"无法创建当前用户专用临时文件。","Could not create the user-private temporary file."},
  {"虚拟屏驱动未启动：","Virtual display driver did not start: "},
  {"缺少视频参数","Missing video parameters"},{"无法创建虚拟屏，请检查管理员权限。","Could not create the virtual display. Check administrator permissions."},
  {"虚拟屏驱动已退出。","The virtual display driver exited."},
  {"等待虚拟屏驱动超时。","Virtual display driver timed out."},{"虚拟屏模式不可用","Virtual display mode unavailable"},
  {"请安装驱动或选择驱动支持的模式。","Install the driver or choose a supported resolution and refresh rate."},
  {"。请打开诊断查看 setup.log。",". Open diagnostics and check setup.log."},{"配置失败，退出码 ","Setup failed, exit code "},
  {"已允许 SideWave 在 Windows 专用网络中连接本地子网。\n请确认两台电脑的网络类型为“专用”。","SideWave can now connect within the local subnet on Windows Private networks.\nSet both computers' network profiles to Private."},
  {"已安装已签名的虚拟显示驱动。\n连接时才创建 SideWave 副屏，退出时释放。","The signed virtual display driver is installed.\nSideWave creates its display on connection and releases it on exit."},
  {"网络配置失败","Network setup failed"},{"驱动安装失败","Driver installation failed"},
  {"语言已切换为中文。","Language changed to Chinese."},{"语言已切换为英文。","Language changed to English."},
  {"无法保存语言设置：","Could not save the language preference: "},
  {"是否可用取决于虚拟屏驱动；失败时恢复原模式。","Availability depends on the virtual display driver. A failed change restores the previous mode."}
 };
 static readonly KeyValuePair<string,string>[] Fragments=English.OrderByDescending(p=>p.Key.Length).ToArray();
 public static string Tr(string source){
  if(source==null||Code!="en")return source;
  string exact;if(English.TryGetValue(source,out exact))return exact;
  foreach(var item in Fragments)source=source.Replace(item.Key,item.Value);
  return source;
 }
 public static List<Action> Capture(DependencyObject root){
  var actions=new List<Action>();Capture(root,actions);return actions;
 }
 static void Capture(DependencyObject root,List<Action> actions){
  var element=root as FrameworkElement;
  bool dynamic=element!=null&&new[]{"Heading","Version","Status","Telemetry","ClientAddresses","ClientSpecs","BandwidthNote","Connect","Disconnect","Apply","Scan","LanguageChoice"}.Contains(element.Name);
  if(element!=null){
   string tip=element.ToolTip as string;if(tip!=null)actions.Add(()=>element.ToolTip=Tr(tip));
   string name=AutomationProperties.GetName(element);if(!String.IsNullOrEmpty(name))actions.Add(()=>AutomationProperties.SetName(element,Tr(name)));
  }
  if(!dynamic){
   var text=root as TextBlock;if(text!=null){string source=text.Text;actions.Add(()=>text.Text=Tr(source));}
   var content=root as ContentControl;
   if(content!=null&&content.Content is string){string source=(string)content.Content;actions.Add(()=>content.Content=Tr(source));}
  }
  foreach(object child in LogicalTreeHelper.GetChildren(root)){var dependency=child as DependencyObject;if(dependency!=null)Capture(dependency,actions);}
 }
}
}
