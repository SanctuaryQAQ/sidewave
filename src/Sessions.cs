using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;

namespace PyroScreen {
class ClientService:IDisposable {
 readonly Settings settings;readonly X509Certificate2 cert;
 readonly TcpListener listener=new TcpListener(IPAddress.Any,55166);
 readonly SemaphoreSlim pending=new SemaphoreSlim(4);
 readonly object gate=new object();
 readonly Engine receiver=new Engine("PyroScreen-Receiver.exe","receiver");
 Discovery discovery;volatile bool stop;bool occupied;TcpClient active;
 string activeHostId="";
 DateTime codeExpires;string code;
 int failed;DateTime nextPairAllowed;
 public event Action<string> Status;
 public string Code{get{lock(gate)return code;}}
 public string ActiveHostId{get{lock(gate)return activeHostId;}}
 public int ReceiverPid{get{return receiver.Pid;}}
 public bool Streaming{get{return receiver.Alive;}}
 void Say(string s){Paths.Log("client",s);var h=Status;if(h!=null)h(s);}
 public ClientService(Settings config){
  settings=config;cert=Secrets.Certificate();RenewCode();
 }
 public void RenewCode(){
  lock(gate){code=Secrets.PairCode();codeExpires=DateTime.UtcNow.AddMinutes(10);failed=0;nextPairAllowed=DateTime.MinValue;
   File.WriteAllBytes(Path.Combine(Paths.Root,"pair-code.dpapi"),System.Security.Cryptography.ProtectedData.Protect(Encoding.UTF8.GetBytes(code),null,System.Security.Cryptography.DataProtectionScope.CurrentUser));
  }
 }
 public void ForgetPairings(){lock(gate){settings.ForgetTokens("client");if(active!=null)active.Close();}}
 public void Start(){
  FullChainLatency.Start();
  listener.Start(4);
  discovery=new Discovery(settings.DeviceId,()=>new{type="client",protocol=1,id=settings.DeviceId,name=Environment.MachineName,
    width=ScreenInfo.Width(),height=ScreenInfo.Height(),hz=ScreenInfo.Hz(),busy=occupied});
  new Thread(Accept){IsBackground=true,Name="Control accept"}.Start();Say("等待局域网主机连接");
 }
 void Accept(){
  while(!stop)try{
   TcpClient t=listener.AcceptTcpClient();
   if(!pending.Wait(0)){t.Close();continue;}
   new Thread(()=>{try{Serve(t);}catch(Exception e){if(!stop)Say("连接已结束："+e.Message);}finally{pending.Release();}}){IsBackground=true}.Start();
  }catch(SocketException){if(stop)break;}
 }
 void Serve(TcpClient t){
  bool owns=false;
  try{
   using(var wire=new Wire(t,true,cert,null)){
    var hello=wire.Read();
    if(Json.S(hello,"type")!="hello"||Json.I(hello,"protocol")!=1)throw new IOException("协议版本不匹配。");
    string hostId=Json.S(hello,"id");Guid id;if(!Guid.TryParse(hostId,out id))throw new IOException("设备身份无效。");
    string hostNonce=Json.S(hello,"nonce");if(hostNonce.Length!=44)throw new IOException("Invalid nonce.");
    string nonce=Convert.ToBase64String(Secrets.Random(32)),pin=Secrets.Pin(cert);
    byte[] key;lock(gate)key=settings.Token(hostId);bool paired=key!=null&&Json.S(hello,"pair")!="True";
    if(!paired){lock(gate){if(DateTime.UtcNow>codeExpires)throw new IOException("配对码已过期，请刷新客户端配对码。");if(DateTime.UtcNow<nextPairAllowed)throw new IOException("配对尝试过多，请稍后重试。");key=Secrets.CodeKey(code);}}
    string transcript="PyroScreen|1|"+hostId+"|"+settings.DeviceId+"|"+hostNonce+"|"+nonce+"|"+pin;
    wire.Send(new{type="challenge",protocol=1,id=settings.DeviceId,nonce=nonce,paired=paired,
      supportsPlainVideo=true,supportsTearingMode=true,supportsPartialFrames=true,supportsClientCursor=true,supportsSnapshotCadence=true,supportsRioReceive=true,supportsReceiveAffinity=true,proof=Secrets.Mac(key,"server|"+transcript),width=ScreenInfo.Width(),height=ScreenInfo.Height(),hz=ScreenInfo.Hz(),linkMbps=NetworkLink.Mbps(((IPEndPoint)t.Client.LocalEndPoint).Address)});
    var proof=wire.Read();
    if(Json.S(proof,"type")!="proof"||!Secrets.Equal(Json.S(proof,"proof"),Secrets.Mac(key,"host|"+transcript))){
     if(!paired)lock(gate){failed++;nextPairAllowed=DateTime.UtcNow.AddSeconds(Math.Min(30,failed*2));}
     throw new IOException("配对码或设备凭据不匹配。");
    }
    lock(gate){
     if(occupied){
      // A network reset can leave the old TLS socket alive. Only the same
      // authenticated, already paired host may release its previous lease.
      if(!paired||activeHostId!=hostId){wire.Send(new{type="error",message="客户端正被另一台主机使用。"});return;}
      if(active!=null)active.Close();
      DateTime deadline=DateTime.UtcNow.AddSeconds(5);
      while(occupied&&!stop&&DateTime.UtcNow<deadline)Monitor.Wait(gate,100);
      if(occupied||stop){wire.Send(new{type="error",message="Previous connection is still closing. Retry shortly."});return;}
     }
     if(stop)throw new IOException("Receiver service is stopping.");
     occupied=true;owns=true;active=t;activeHostId=hostId;
     if(!paired){key=Secrets.Random(32);settings.SaveToken(hostId,key);settings.Save("client");}
    }
    string connectionId=Guid.NewGuid().ToString("N");
    wire.Send(new{type="ready",connection=connectionId,token=paired?"":Convert.ToBase64String(key),name=Environment.MachineName});
    wire.SetReceiveTimeout(65000); // Both TLS and TCP must cover virtual-display creation (up to 35 s).
    Say("已配对并连接 · "+((IPEndPoint)t.Client.RemoteEndPoint).Address);
    while(!stop){
     var request=wire.Read();long received=Native.Now();string type=Json.S(request,"type");
     try{
      if(type=="clock"){wire.Send(new{type="clock",t1=Json.L(request,"t1"),t2=received,t3=Native.Now()});}
      else if(type=="calibrate"){
       long offset=Json.L(request,"offset"),bound=Json.L(request,"bound");
       if(bound<0||bound>1000)throw new IOException("Invalid clock bound.");
       Paths.Atomic(Path.Combine(Paths.Runtime,"clock-calibration.txt"),offset+" "+bound+" "+Native.Now()+" tls_ntp");
       wire.Send(new{type="ok"});
      }
      else if(type=="start"){
       int w=Json.I(request,"width"),h=Json.I(request,"height"),fps=Json.I(request,"fps");VideoSecret.ValidateMode(w,h,fps);
       object encryption;bool encrypted=true;if(request.TryGetValue("encryptedVideo",out encryption)){if(!(encryption is bool))throw new IOException("Invalid video mode.");encrypted=(bool)encryption;}
       object tearingValue;bool tearing=false;if(request.TryGetValue("allowTearing",out tearingValue)){if(!(tearingValue is bool))throw new IOException("Invalid presentation mode.");tearing=(bool)tearingValue;}
       object cursorValue;bool clientCursor=false;if(request.TryGetValue("clientCursor",out cursorValue)){if(!(cursorValue is bool))throw new IOException("Invalid cursor mode.");clientCursor=(bool)cursorValue;}
       object rioValue;bool receiveRio=false;if(request.TryGetValue("rioReceive",out rioValue)){if(!(rioValue is bool))throw new IOException("Invalid receive transport.");receiveRio=(bool)rioValue;}
       int receiveCpu=Json.I(request,"receiveCpu",-1);if(receiveCpu<-1||receiveCpu>63)throw new IOException("Invalid receiver processor index.");
       byte[] secret=Convert.FromBase64String(Json.S(request,"secret"));string file=VideoSecret.Save(secret,"receiver");
       string peer=((IPEndPoint)t.Client.RemoteEndPoint).Address.ToString();
       receiver.Stop();File.Delete(Path.Combine(Paths.Runtime,"receiver-stats.json"));
       if(settings.RendererVendor!=0&&settings.RendererVendor!=0x1002&&settings.RendererVendor!=0x10de&&settings.RendererVendor!=0x8086)throw new IOException("不支持的显示适配器设置。");
       bool snapshot=Json.S(request,"captureTimestampKind","desktop_update")=="snapshot";
       receiver.Start("--port 55165 --duration 0 --width "+w+" --height "+h+" --peer "+peer+" --secret-file "+Engine.Q(file)+" --chroma444"+(receiveRio?" --receive-rio":"")+" --receive-cpu "+receiveCpu+(snapshot?" --capture-snapshot-timestamps":"")+(encrypted?"":" --unencrypted-video")+(settings.RendererVendor==0?"":" --renderer-vendor "+settings.RendererVendor.ToString("x"))+(settings.WaitablePresent?" --waitable-present":"")+(tearing?" --allow-tearing":"")+(clientCursor?" --client-cursor":""));
       Thread.Sleep(400);receiver.Ensure();
       wire.Send(new{type="started",connection=connectionId,width=w,height=h,receiverPid=receiver.Pid,panelHz=ScreenInfo.Hz(),encryptedVideo=encrypted,allowTearing=tearing,clientCursor=clientCursor,rioReceive=receiveRio});Say("副屏运行 · "+w+" × "+h+" · 目标 "+fps+" FPS");
      }
      else if(type=="stats"){
       object data=null;
       string path=Path.Combine(Paths.Runtime,"receiver-stats.json");
       if(receiver.Alive&&File.Exists(path))try{data=Json.Read(File.ReadAllText(path));}catch{}
       bool alive=receiver.Alive;wire.Send(new{type="stats",connection=connectionId,alive=alive,data=data,exitCode=alive?null:receiver.ExitCode,exitDiagnostic=alive?"":receiver.ExitDiagnostic});
      }
      else if(type=="stop"){receiver.Stop();wire.Send(new{type="stopped",connection=connectionId});Say("保持连接 · 视频已停止");}
      else if(type=="disconnect"){wire.Send(new{type="bye"});break;}
      else throw new IOException("Unknown control operation.");
     }catch(Exception e){wire.Send(new{type="error",message=e.Message});}
    }
   }
  }finally{
   try{t.Close();}catch{}
   if(owns){receiver.Stop();lock(gate){if(Object.ReferenceEquals(active,t)){occupied=false;active=null;activeHostId="";Monitor.PulseAll(gate);}}Say("等待主机 · 上次会话已释放");}
  }
 }
 public void Dispose(){
  FullChainLatency.Stop();
  stop=true;listener.Stop();if(discovery!=null)discovery.Dispose();lock(gate){if(active!=null)active.Close();Monitor.PulseAll(gate);}
  receiver.Dispose();cert.Dispose();
 }
}
class HostLink:IDisposable {
 readonly Settings settings;
 Wire wire;TcpClient tcp;DisplayLease display;
 readonly Engine sender=new Engine("PyroScreen-Sender.exe","sender");
 bool appliedProtection,appliedSegmentation,appliedEncryption,plainSupported,appliedTearing,tearingSupported,appliedClientCursor,clientCursorSupported,snapshotSupported,rioReceiveSupported,receiveAffinitySupported,partialSupported;
 int appliedProtectionBytes,appliedPacing;
 bool appliedBackpressure,appliedPipeline,appliedSkipImmediate,appliedPartialFrames,appliedCaptureOnUpdate,appliedRio,appliedConnected,appliedRioReceive;int appliedSendBuffer,appliedReceiveCpu,appliedSendWindow;
 public string PeerId,Ip,Name,ConnectionId;public int NativeWidth,NativeHeight,PanelHz,VirtualHz;
 public int ReceiverPid,EffectiveFps,LinkBudgetMbps;int localLinkMbps,remoteLinkMbps;
 // Reported adapter speeds from connection time; 0 means unknown.
 public int LocalLinkMbps{get{return localLinkMbps;}}
 public int RemoteLinkMbps{get{return remoteLinkMbps;}}
 public bool Connected{get{return wire!=null;}}
 public string DisplayName{get{return display==null?"":display.Display;}}
 public bool Streaming{get{return sender.Alive;}}
 public bool VideoRequested{get;private set;}
 public int SenderPid{get{return sender.Pid;}}
 public string SenderExitDiagnostic{get{return sender.ExitDiagnostic;}}
 public bool HasDisplay{get{return display!=null&&display.Alive;}}
 public event Action<string> Status;
 void Say(string s){Paths.Log("host",s);var h=Status;if(h!=null)h(s);}
 public HostLink(Settings s){settings=s;}
 public void Connect(string address,string code=""){
  DropConnection(true);IPAddress ip;if(!IPAddress.TryParse(address,out ip)||ip.AddressFamily!=AddressFamily.InterNetwork)throw new Exception("请输入有效的 IPv4 地址。");
  tcp=new TcpClient();var result=tcp.BeginConnect(ip,55166,null,null);
  if(!result.AsyncWaitHandle.WaitOne(5000)){tcp.Close();throw new Exception("连接超时，请检查副机软件与专用网络防火墙。");}
  tcp.EndConnect(result);localLinkMbps=NetworkLink.Mbps(((IPEndPoint)tcp.Client.LocalEndPoint).Address);
  try{
   string expected=address==settings.LastIp?settings.LastPin:"";
   // On DHCP changes the identity and saved certificate are checked after hello.
   wire=new Wire(tcp,false,null,expected);
   string nonce=Convert.ToBase64String(Secrets.Random(32));
   bool pairRequested=!String.IsNullOrWhiteSpace(code);
   wire.Send(new{type="hello",protocol=1,id=settings.DeviceId,nonce=nonce,pair=pairRequested});
   var challenge=wire.Read();if(Json.S(challenge,"type")!="challenge"||Json.I(challenge,"protocol")!=1)throw new Exception("对方不是兼容的 SideWave 客户端。");
   PeerId=Json.S(challenge,"id");Guid id;if(!Guid.TryParse(PeerId,out id))throw new Exception("设备身份无效。");
   byte[] key=settings.Token(PeerId);
   if(key!=null&&!String.IsNullOrEmpty(settings.LastPin)&&PeerId==settings.LastId&&!Secrets.Equal(wire.Pin,settings.LastPin))throw new Exception("设备证书已改变。请忘记旧配对后重新确认副机。");
   if(key==null||Json.S(challenge,"paired")!="True")key=Secrets.CodeKey(code);
   string transcript="PyroScreen|1|"+settings.DeviceId+"|"+PeerId+"|"+nonce+"|"+Json.S(challenge,"nonce")+"|"+wire.Pin;
   if(!Secrets.Equal(Json.S(challenge,"proof"),Secrets.Mac(key,"server|"+transcript)))throw new Exception("配对码错误或设备身份不匹配。");
   wire.Send(new{type="proof",proof=Secrets.Mac(key,"host|"+transcript)});
   var ready=wire.Read();if(Json.S(ready,"type")!="ready")throw new Exception(Json.S(ready,"message","配对失败。"));
   string token=Json.S(ready,"token");if(token!="")settings.SaveToken(PeerId,Convert.FromBase64String(token));
   settings.LastId=PeerId;settings.LastIp=address;settings.LastPin=wire.Pin;settings.Save("host");
   receiveAffinitySupported=Json.S(challenge,"supportsReceiveAffinity")=="True";rioReceiveSupported=Json.S(challenge,"supportsRioReceive")=="True";snapshotSupported=Json.S(challenge,"supportsSnapshotCadence")=="True";plainSupported=Json.S(challenge,"supportsPlainVideo")=="True";tearingSupported=Json.S(challenge,"supportsTearingMode")=="True";partialSupported=Json.S(challenge,"supportsPartialFrames")=="True";clientCursorSupported=Json.S(challenge,"supportsClientCursor")=="True";NativeWidth=Json.I(challenge,"width");NativeHeight=Json.I(challenge,"height");PanelHz=Json.I(challenge,"hz");remoteLinkMbps=Json.I(challenge,"linkMbps");
   VideoSecret.FitMode(ref NativeWidth,ref NativeHeight);VideoSecret.ValidateMode(NativeWidth,NativeHeight,120);
   Name=Json.S(ready,"name");ConnectionId=Json.S(ready,"connection");Ip=address;
   Calibrate();Paths.Log("host","Control session "+ConnectionId+" peer "+PeerId);Say("已连接 "+Name);
  }catch{DropConnection(true);throw;}
 }
 public void Calibrate(){
  if(wire==null)return;
  double best=Double.MaxValue,offset=0;
  for(int i=0;i<8;++i){
   long t1=Native.Now();var r=wire.Call(new{type="clock",t1=t1});long t4=Native.Now(),t2=Json.L(r,"t2"),t3=Json.L(r,"t3");
   double rtt=(t4-t1)-(t3-t2);
   if(rtt>=0&&rtt<best){best=rtt;offset=((double)t2-t1+(double)t3-t4)/2;}
  }
  if(best==Double.MaxValue)throw new IOException("Clock calibration unavailable.");
  wire.Call(new{type="calibrate",offset=(long)Math.Round(offset),bound=(long)Math.Ceiling(best/2+1)});
 }
 public void Apply(int w,int h,int fps,int budget){
  if(wire==null)throw new Exception("请先连接副机。");VideoSecret.ValidateMode(w,h,fps);
  if(Streaming&&HasDisplay&&settings.Width==w&&settings.Height==h&&settings.Fps==fps&&settings.BudgetMbps==budget&&appliedProtection==settings.PacketProtection&&appliedProtectionBytes==settings.ProtectionPacketBytes&&appliedPacing==settings.SendPacingMbps&&appliedBackpressure==settings.CaptureBackpressure&&appliedPipeline==settings.PipelinedSend&&appliedSkipImmediate==settings.SkipImmediateSend&&appliedPartialFrames==settings.PartialFrames&&appliedCaptureOnUpdate==settings.CaptureOnUpdate&&appliedRio==settings.RioSend&&appliedRioReceive==settings.RioReceive&&appliedReceiveCpu==settings.ReceiveCpu&&appliedConnected==settings.ConnectedUdp&&appliedSendBuffer==settings.SendBufferBytes&&appliedSendWindow==settings.SendWindow&&appliedSegmentation==settings.UdpSegmentation&&appliedEncryption==settings.VideoEncryption&&appliedTearing==settings.AllowTearing&&appliedClientCursor==settings.ClientCursor){Say("画面设置已是当前值。");return;}
  if(!settings.VideoEncryption&&!plainSupported)throw new IOException("副机需要升级才能使用明文视频。Receiver update required for plaintext video.");
  if(settings.AllowTearing&&!tearingSupported)throw new IOException("副机需要升级才能使用低延迟显示。Receiver update required for low latency presentation.");
  if(settings.ClientCursor&&!clientCursorSupported)throw new IOException("副机需要升级才能使用本地硬件光标。Receiver update required for client cursor.");
  if(!snapshotSupported)throw new IOException("Receiver update required for constant frame cadence.");
  if(settings.RioReceive&&!rioReceiveSupported)throw new IOException("Receiver update required for registered receive transport.");
  if(settings.ReceiveCpu>=0&&!receiveAffinitySupported)throw new IOException("Receiver update required for receive affinity diagnostics.");
  // Keep both the authenticated control connection and virtual-device lease.
  // Encoder/decoder resources change as a transaction with a new video key.
  StopVideo();
  try{
   if(display==null)display=new DisplayLease(settings.DeviceId);display.Apply(w,h,fps,settings.Backend);VirtualHz=display.Hz;
   byte[] secret=Secrets.Random(44);string secretFile=VideoSecret.Save(secret,"sender");
   Calibrate();var started=wire.Call(new{type="start",width=w,height=h,fps=fps,captureTimestampKind="desktop_update",encryptedVideo=settings.VideoEncryption,allowTearing=settings.AllowTearing,clientCursor=settings.ClientCursor,rioReceive=settings.RioReceive,receiveCpu=settings.ReceiveCpu,secret=Convert.ToBase64String(secret)});ReceiverPid=Json.I(started,"receiverPid");
   if(Json.S(started,"encryptedVideo","True")!=(settings.VideoEncryption?"True":"False"))throw new IOException("Video mode negotiation failed.");
   if(Json.S(started,"allowTearing","False")!=(settings.AllowTearing?"True":"False"))throw new IOException("Presentation mode negotiation failed.");
   if(Json.S(started,"clientCursor","False")!=(settings.ClientCursor?"True":"False"))throw new IOException("Cursor mode negotiation failed; update the receiver.");
   if(Json.S(started,"rioReceive","False")!=(settings.RioReceive?"True":"False"))throw new IOException("Receive transport negotiation failed; update the receiver.");
   if(budget<50||budget>2500)throw new IOException("带宽预算必须在 50–2500 Mbps 范围内。");
   // The selected rate targets the transport/renderer. Physical panel refresh
   // is reported separately and must not silently reduce the sender cadence.
   EffectiveFps=fps;
   int linkSpeed=localLinkMbps>0?localLinkMbps:1000;
   if(remoteLinkMbps>0)linkSpeed=Math.Min(linkSpeed,remoteLinkMbps);
   LinkBudgetMbps=Math.Max(10,(int)(linkSpeed*0.90));
   if(settings.PartialFrames&&!partialSupported)Say("副机版本不支持只传输变化区域，已使用整帧传输。");
   int bytes=Math.Min(4*1048576,Math.Max(65536,(int)(budget*1000000.0/8/EffectiveFps)));
   File.Delete(Path.Combine(Paths.Runtime,"sender-live.json"));
   sender.Start("--capture-display "+Engine.Q(display.Display)+" --send-host "+Ip+" --send-bind-host "+((System.Net.IPEndPoint)tcp.Client.LocalEndPoint).Address.ToString()+" --port 55165 --width "+w+" --height "+h+
    " --target-fps "+fps+" --capture-fps "+EffectiveFps+" --constant-frame-rate"+(settings.CaptureBackpressure?" --capture-backpressure":"")+" --link-budget-mbps "+LinkBudgetMbps+" --measure-seconds 0 --max-frame-bytes "+bytes+
    " --packet-bytes "+(settings.PacketProtection?settings.ProtectionPacketBytes:32768)+" --send-pacing-mbps "+Math.Min(settings.SendPacingMbps,LinkBudgetMbps)+" --send-window "+settings.SendWindow+" --send-buffer-bytes "+settings.SendBufferBytes+(settings.RioSend?" --send-rio":" --send-iocp")+(settings.ConnectedUdp?" --send-connected":"")+(settings.PipelinedSend?" --send-pipelined":"")+(settings.SkipImmediateSend?" --send-skip-immediate":"")+(settings.PartialFrames&&partialSupported?" --partial-frames":"")+(settings.CaptureOnUpdate?" --capture-on-update":"")+(settings.ClientCursor?" --client-cursor":"")+" --chroma444"+(settings.UdpSegmentation?" --udp-segmented":"")+(settings.PacketProtection?" --fec --send-interleaved":"")+(settings.VideoEncryption?"":" --unencrypted-video")+" --secret-file "+Engine.Q(secretFile));
   Thread.Sleep(700);sender.Ensure();
   VideoRequested=true;
   settings.Width=w;settings.Height=h;settings.Fps=fps;settings.BudgetMbps=budget;settings.Save("host");
   appliedEncryption=settings.VideoEncryption;appliedProtection=settings.PacketProtection;appliedSegmentation=settings.UdpSegmentation;appliedTearing=settings.AllowTearing;appliedClientCursor=settings.ClientCursor;
   appliedProtectionBytes=settings.ProtectionPacketBytes;
   appliedPacing=settings.SendPacingMbps;
   appliedBackpressure=settings.CaptureBackpressure;
   appliedPipeline=settings.PipelinedSend;appliedSkipImmediate=settings.SkipImmediateSend;appliedPartialFrames=settings.PartialFrames;appliedCaptureOnUpdate=settings.CaptureOnUpdate;appliedRio=settings.RioSend;appliedConnected=settings.ConnectedUdp;appliedSendWindow=settings.SendWindow;appliedSendBuffer=settings.SendBufferBytes;appliedRioReceive=settings.RioReceive;appliedReceiveCpu=settings.ReceiveCpu;
   Paths.Atomic(Path.Combine(Paths.Runtime,"host-session.json"),Json.Write(new{connection=ConnectionId,peer=PeerId,ip=Ip,display=display.Display,width=w,height=h,encryptedVideo=settings.VideoEncryption,requestedFps=fps,virtualHz=VirtualHz,panelHz=PanelHz}));
   Say(w+" × "+h+" · 目标 "+fps+" FPS · 虚拟屏 "+VirtualHz+" Hz · 副机 "+PanelHz+" Hz");
  }catch{try{StopVideo();}catch{}throw;}
 }
 public Dictionary<string,object> Stats(){if(wire==null)return null;return wire.Call(new{type="stats"});}
 public void StopVideo(){
  VideoRequested=false;
  ReceiverPid=0;sender.Stop();if(wire!=null)wire.Call(new{type="stop"});
 }
 public void RecoverVideo(){
  // Apply preserves the TLS session and display lease. Preserve intent across
  // a failed recovery too, so the watchdog can spend its remaining retries.
  try{Apply(settings.Width,settings.Height,settings.Fps,settings.BudgetMbps);}
  finally{VideoRequested=true;}
 }
 public void Disconnect(){
  CloseConnection(false,true);
 }
 public void DropConnection(bool retainDisplay){
  CloseConnection(retainDisplay,false);
 }
 void CloseConnection(bool retainDisplay,bool graceful){
  VideoRequested=false;
  ReceiverPid=0;sender.Stop();if(wire!=null){if(graceful)try{wire.Call(new{type="disconnect"});}catch{}wire.Dispose();wire=null;}
  if(tcp!=null){tcp.Close();tcp=null;}if(display!=null&&!retainDisplay){display.Dispose();display=null;}
  try{File.Delete(Path.Combine(Paths.Runtime,"host-session.json"));}catch{}
 }
 public void Dispose(){Disconnect();sender.Dispose();}
}
static class ScreenInfo {
 // Current physical mode in the user's interactive desktop, not WinRM Session 0.
 [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential,CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
 struct Mode {
  [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr,SizeConst=32)]public string device;
  public ushort spec,driver,size,extra;public uint fields;public int x,y;public uint orientation,fixedOutput;
  public short color,duplex,yres,tt,collate;
  [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr,SizeConst=32)]public string form;
  public ushort logPixels;public uint bits,width,height,flags,frequency,icm,icmIntent,media,dither,reserved1,reserved2,panningW,panningH;
 }
 [System.Runtime.InteropServices.DllImport("user32.dll",CharSet=System.Runtime.InteropServices.CharSet.Unicode)]static extern bool EnumDisplaySettings(string name,int mode,ref Mode data);
 public static int Hz(){Mode m=new Mode();m.size=(ushort)System.Runtime.InteropServices.Marshal.SizeOf(m);return EnumDisplaySettings(null,-1,ref m)?(int)m.frequency:0;}
 public static int Width(){Mode m=new Mode();m.size=(ushort)System.Runtime.InteropServices.Marshal.SizeOf(m);return EnumDisplaySettings(null,-1,ref m)?(int)m.width:Native.GetSystemMetrics(0);}
 public static int Height(){Mode m=new Mode();m.size=(ushort)System.Runtime.InteropServices.Marshal.SizeOf(m);return EnumDisplaySettings(null,-1,ref m)?(int)m.height:Native.GetSystemMetrics(1);}
}
}
