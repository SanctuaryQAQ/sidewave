using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Security;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Diagnostics;
using Microsoft.Win32.SafeHandles;

namespace PyroScreen {
static class Json {
 public static string Write(object o) { return new JavaScriptSerializer{MaxJsonLength=131072}.Serialize(o); }
 public static Dictionary<string,object> Read(string s) { return new JavaScriptSerializer{MaxJsonLength=131072,RecursionLimit=12}.Deserialize<Dictionary<string,object>>(s); }
 public static string S(Dictionary<string,object>d,string k,string v=""){object o;return d.TryGetValue(k,out o)&&o!=null?Convert.ToString(o):v;}
 public static int I(Dictionary<string,object>d,string k,int v=0){int n;return int.TryParse(S(d,k),out n)?n:v;}
 public static long L(Dictionary<string,object>d,string k){long n;return long.TryParse(S(d,k),out n)?n:0;}
}
static class Paths {
 public static readonly string Root=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"PyroScreen");
 public static readonly string Runtime=Path.Combine(Root,"sessions",Process.GetCurrentProcess().Id+"-"+Guid.NewGuid().ToString("N"));
 public static readonly string Bin=AppDomain.CurrentDomain.BaseDirectory;
 static readonly object atomicGate=new object();
 public static void Init(){Directory.CreateDirectory(Runtime);}
 public static void Atomic(string path,string value){lock(atomicGate){
  string temp=path+".tmp";File.WriteAllText(temp,value,new UTF8Encoding(false));
  for(int i=0;i<8;i++){if(Native.MoveFileEx(temp,path,1))return;Thread.Sleep(10);}
  try{File.Delete(temp);}catch{}throw new IOException("无法更新配置或诊断文件："+Path.GetFileName(path));
 }}
 public static void CleanupSession(){
  try{string parent=Path.GetFullPath(Path.Combine(Root,"sessions"))+Path.DirectorySeparatorChar;
   string current=Path.GetFullPath(Runtime);
   if(current.StartsWith(parent,StringComparison.OrdinalIgnoreCase)&&Directory.Exists(current))Directory.Delete(current,true);
  }catch(Exception e){Log("app","Session cleanup: "+e.Message);}
 }
 public static void Log(string role,string msg){
  try { lock(Root){string p=Path.Combine(Root,role+".log");if(File.Exists(p)&&new FileInfo(p).Length>2*1024*1024){if(File.Exists(p+".old"))File.Delete(p+".old");File.Move(p,p+".old");}File.AppendAllText(p,DateTime.UtcNow.ToString("o")+" "+msg+"\r\n");} }catch{}
 }
}
static class Native {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] public static extern bool MoveFileEx(string oldName,string newName,int flags);
 [DllImport("kernel32.dll")] static extern void GetSystemTimePreciseAsFileTime(out long time);
 public static long Now(){long n;GetSystemTimePreciseAsFileTime(out n);return (n-116444736000000000L)/10000;}
 [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
}
class Settings {
 readonly object sync=new object();
 public string DeviceId=Guid.NewGuid().ToString("N");
 public string LastId="",LastIp="",LastPin="";
 public bool AutoConnect=true,StartWithWindows=false;
 public bool? NativeResolution;
 public bool VideoEncryption=false;
 public bool AllowTearing=false;
 public bool PacketProtection=false;
 public int ProtectionPacketBytes=32768;
 public int SendPacingMbps=0;
 public bool CaptureBackpressure=false;
 public bool PipelinedSend=false;
 public bool SkipImmediateSend=false;
 // Experimental: send only changed regions; requires a receiver that decodes partial frames.
 public bool PartialFrames=false;
 // Experimental: the receiver shows the host pointer with its own hardware cursor.
 public bool ClientCursor=false;
 // Capture a desktop update immediately; fixed-cadence repeats still fill gaps.
 public bool CaptureOnUpdate=true;
 public bool RioSend=false;
 public bool ConnectedUdp=false;
 public int SendBufferBytes=131072;
 public int SendWindow=64;
 public bool RioReceive=false;
 public int ReceiveCpu=-1;
 public bool UdpSegmentation=false;
 public int RendererVendor=0;
 public bool WaitablePresent=false;
 public int Width=2560,Height=1440,Fps=120,BudgetMbps=600;
 public string Backend="vdd";
 public Dictionary<string,string> Peers=new Dictionary<string,string>();
 public static Settings Load(string role){
  string path=Path.Combine(Paths.Root,role+".json");
  try{var value=new JavaScriptSerializer().Deserialize<Settings>(File.ReadAllText(path));if(value==null)throw new IOException("配置内容为空。");return value;}
  catch(FileNotFoundException){return new Settings();}catch(DirectoryNotFoundException){return new Settings();}
  catch(Exception e){throw new IOException("无法读取 "+role+" 配置，请打开诊断。原有配对不会被覆盖。",e);}
 }
 public void Save(string role){lock(sync)Paths.Atomic(Path.Combine(Paths.Root,role+".json"),Json.Write(this));}
 public void ForgetTokens(string role){lock(sync){Peers.Clear();LastId="";LastPin="";LastIp="";Save(role);}}
 public void SaveToken(string id,byte[] token){
  lock(sync)Peers[id]=Convert.ToBase64String(ProtectedData.Protect(token,null,DataProtectionScope.CurrentUser));
 }
 public byte[] Token(string id){
  lock(sync){
  string s;if(!Peers.TryGetValue(id,out s))return null;
  try{return ProtectedData.Unprotect(Convert.FromBase64String(s),null,DataProtectionScope.CurrentUser);}catch{return null;}}
 }
}
static class Secrets {
 public static byte[] Random(int n){byte[] b=new byte[n];using(var r=RandomNumberGenerator.Create())r.GetBytes(b);return b;}
 public static string PairCode(){
  const string alphabet="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  byte[] b=Random(10);var sb=new StringBuilder();int bits=0,buffer=0;
  foreach(byte x in b){buffer=(buffer<<8)|x;bits+=8;while(bits>=5){bits-=5;sb.Append(alphabet[(buffer>>bits)&31]);}}
  return sb.ToString();
 }
 public static byte[] CodeKey(string code){
  string s=(code??"").Replace("-","").Replace(" ","").ToUpperInvariant();
  if(s.Length!=16||s.Any(c=>!"ABCDEFGHJKLMNPQRSTUVWXYZ23456789".Contains(c)))throw new Exception("配对码应为客户端显示的 16 位字符。");
  using(var h=SHA256.Create())return h.ComputeHash(Encoding.UTF8.GetBytes("PyroScreen pairing v1|"+s));
 }
 public static string Pin(X509Certificate c){using(var h=SHA256.Create())return Convert.ToBase64String(h.ComputeHash(c.GetRawCertData()));}
 public static string Mac(byte[]key,string text){using(var h=new HMACSHA256(key))return Convert.ToBase64String(h.ComputeHash(Encoding.UTF8.GetBytes(text)));}
 public static bool Equal(string a,string b){
  if(a==null||b==null)return false;byte[] x=Encoding.UTF8.GetBytes(a),y=Encoding.UTF8.GetBytes(b);
  int diff=x.Length^y.Length;for(int i=0;i<Math.Min(x.Length,y.Length);++i)diff|=x[i]^y[i];return diff==0;
 }
 public static X509Certificate2 Certificate(){
  string p=Path.Combine(Paths.Root,"client-cert.dpapi");
  if(File.Exists(p))return new X509Certificate2(ProtectedData.Unprotect(File.ReadAllBytes(p),null,DataProtectionScope.CurrentUser),"",X509KeyStorageFlags.UserKeySet | X509KeyStorageFlags.PersistKeySet);
  using(var rsa=RSA.Create(2048)){
   var req=new CertificateRequest("CN=PyroScreen LAN client",rsa,HashAlgorithmName.SHA256,RSASignaturePadding.Pkcs1);
   req.CertificateExtensions.Add(new X509BasicConstraintsExtension(false,false,0,true));
   req.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature,true));
   using(var cert=req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-2),DateTimeOffset.UtcNow.AddYears(10))){
    byte[] pfx=cert.Export(X509ContentType.Pfx,"");File.WriteAllBytes(p,ProtectedData.Protect(pfx,null,DataProtectionScope.CurrentUser));
    return new X509Certificate2(pfx,"",X509KeyStorageFlags.UserKeySet | X509KeyStorageFlags.PersistKeySet);
   }
  }
 }
}
class Wire:IDisposable {
 readonly TcpClient tcp;readonly SslStream tls;
 public string Pin;
 public Wire(TcpClient t,bool server,X509Certificate2 cert,string expectedPin){
  tcp=t;tcp.NoDelay=true;tcp.ReceiveTimeout=15000;tcp.SendTimeout=15000;
  tls=new SslStream(tcp.GetStream(),false,(sender,c,chain,error)=>{
   if(server)return true; // Host identity is proved by the paired HMAC below.
   if(c==null)return false;Pin=Secrets.Pin(c);return String.IsNullOrEmpty(expectedPin)||Secrets.Equal(Pin,expectedPin);
  });
  if(server)tls.AuthenticateAsServer(cert,false,SslProtocols.Tls12,false);
  else tls.AuthenticateAsClient("PyroScreen",null,SslProtocols.Tls12,false);
  tls.ReadTimeout=15000;tls.WriteTimeout=15000;
 }
 public void Send(object value){
  byte[] b=Encoding.UTF8.GetBytes(Json.Write(value));if(b.Length>32768)throw new IOException("Control message too large.");
  byte[] h=BitConverter.GetBytes(IPAddress.HostToNetworkOrder(b.Length));tls.Write(h,0,4);tls.Write(b,0,b.Length);tls.Flush();
 }
 void Full(byte[] b){int n=0;while(n<b.Length){int r=tls.Read(b,n,b.Length-n);if(r==0)throw new IOException("Peer disconnected.");n+=r;}}
 public Dictionary<string,object> Read(){
  byte[] h=new byte[4];Full(h);int n=IPAddress.NetworkToHostOrder(BitConverter.ToInt32(h,0));
  if(n<2||n>32768)throw new IOException("Invalid control message length.");byte[] b=new byte[n];Full(b);return Json.Read(Encoding.UTF8.GetString(b));
 }
 public Dictionary<string,object> Call(object v){Send(v);var r=Read();if(Json.S(r,"type")=="error")throw new Exception(Json.S(r,"message"));return r;}
 public void SetReceiveTimeout(int milliseconds){tcp.ReceiveTimeout=milliseconds;tls.ReadTimeout=milliseconds;}
 public void Dispose(){try{tls.Dispose();}catch{}try{tcp.Close();}catch{}}
}
class Peer {
 public string Id{get;set;}public string Name{get;set;}public string Ip{get;set;}public int Width{get;set;}public int Height{get;set;}public int Hz{get;set;}public DateTime Seen{get;set;}
 public override string ToString(){return Name+"   ·   "+Ip+"   ·   "+Width+" × "+Height;}
}
class Discovery:IDisposable {
 UdpClient udp;volatile bool stop;
 readonly object scanGate=new object();
 readonly Dictionary<string,UdpClient> adapters=new Dictionary<string,UdpClient>();
 public event Action<Peer> Found;
 readonly string id;readonly Func<object> info;
 public Discovery(string deviceId,Func<object> clientInfo){
  id=deviceId;info=clientInfo;
  udp=clientInfo!=null?new UdpClient(new IPEndPoint(IPAddress.Any,55167)):new UdpClient(0);
  udp.EnableBroadcast=true;udp.Client.ReceiveTimeout=1000;
  Listen(udp);
 }
 void Listen(UdpClient socket){new Thread(()=>Loop(socket)){IsBackground=true,Name="LAN discovery"}.Start();}
 void Loop(UdpClient socket){
  while(!stop){try{
   IPEndPoint ep=new IPEndPoint(IPAddress.Any,0);byte[] b=socket.Receive(ref ep);if(b.Length>2048)continue;
   var m=Json.Read(Encoding.UTF8.GetString(b));string type=Json.S(m,"type");
   if(Json.I(m,"protocol")!=1)continue;
   if(type=="find"&&info!=null){byte[] reply=Encoding.UTF8.GetBytes(Json.Write(info()));socket.Send(reply,reply.Length,ep);}
   else if(type=="client"&&info==null){
    string peerId=Json.S(m,"id");Guid parsed;if(!Guid.TryParse(peerId,out parsed)||peerId==id)continue;
    var p=new Peer{Id=peerId,Ip=ep.Address.ToString(),Name=Json.S(m,"name").Substring(0,Math.Min(80,Json.S(m,"name").Length)),
     Width=Json.I(m,"width"),Height=Json.I(m,"height"),Hz=Json.I(m,"hz"),Seen=DateTime.UtcNow};
    if(p.Width<640||p.Width>7680||p.Height<480||p.Height>4320)continue;
    var handler=Found;if(handler!=null)handler(p);
   }
  }catch(SocketException){if(socket.Client==null)break;}catch(ObjectDisposedException){break;}catch{}}
 }
 public void Scan(string manual=""){
  byte[] b=Encoding.UTF8.GetBytes(Json.Write(new{type="find",protocol=1,id=id}));
  lock(scanGate){
   if(stop||info!=null)return;
   var targets=new Dictionary<string,IPAddress>();
   try{foreach(var nic in NetworkInterface.GetAllNetworkInterfaces()){
    if(nic.OperationalStatus!=OperationalStatus.Up||nic.NetworkInterfaceType==NetworkInterfaceType.Loopback)continue;
    try{foreach(var a in nic.GetIPProperties().UnicastAddresses){
     if(a.Address.AddressFamily!=AddressFamily.InterNetwork||a.IPv4Mask==null||IPAddress.IsLoopback(a.Address))continue;
     byte[] ip=a.Address.GetAddressBytes(),mask=a.IPv4Mask.GetAddressBytes();
     // /31 and /32 links have no IPv4 broadcast address.
     if(mask[0]==255&&mask[1]==255&&mask[2]==255&&mask[3]>=254)continue;
     for(int i=0;i<4;++i)ip[i]=(byte)(ip[i]|~mask[i]);
     targets[a.Address.ToString()]=new IPAddress(ip);
    }}catch(NetworkInformationException){}
   }}catch(NetworkInformationException){return;}
   foreach(string old in adapters.Keys.Where(k=>!targets.ContainsKey(k)).ToArray()){adapters[old].Close();adapters.Remove(old);}
   foreach(var target in targets)try{
    UdpClient socket;
    if(!adapters.TryGetValue(target.Key,out socket)){
     // Keep replies on the requesting LAN interface, including with VPNs or multiple NICs.
     socket=new UdpClient(new IPEndPoint(IPAddress.Parse(target.Key),0));
     socket.EnableBroadcast=true;socket.Client.ReceiveTimeout=1000;adapters.Add(target.Key,socket);Listen(socket);
    }
    socket.Send(b,b.Length,new IPEndPoint(target.Value,55167));
   }catch(SocketException){}catch(ObjectDisposedException){}
   if(targets.Count==0)try{udp.Send(b,b.Length,new IPEndPoint(IPAddress.Broadcast,55167));}catch(SocketException){}
   IPAddress address;
   if(!String.IsNullOrWhiteSpace(manual)&&IPAddress.TryParse(manual,out address)&&address.AddressFamily==AddressFamily.InterNetwork)
    try{udp.Send(b,b.Length,new IPEndPoint(address,55167));}catch(SocketException){}
   }
 }
 public void Dispose(){lock(scanGate){stop=true;udp.Close();foreach(var socket in adapters.Values)socket.Close();adapters.Clear();}}
}
class EngineExitState {
 public readonly object Gate=new object();public Process Process;public string LastError="";public bool Intentional,Logged;
 readonly ManualResetEventSlim stderrDrained=new ManualResetEventSlim(false);
 public void Error(string line){if(line==null){stderrDrained.Set();return;}lock(Gate)LastError=line.Length>2048?line.Substring(line.Length-2048):line;}
 public void Drain(){stderrDrained.Wait(200);}
 public string Summary(int pid,int code){lock(Gate)return "Video component exited: pid="+pid+" code="+code+" (0x"+unchecked((uint)code).ToString("X8")+") intentional="+Intentional+(LastError==""?"":" last stderr: "+LastError);}
}
class Engine:IDisposable {
 Process process;readonly string exe;readonly string role;EngineExitState diagnostic=new EngineExitState();
 public string ExitDiagnostic{get{
  var run=diagnostic;var current=run.Process;if(current==null)return "";
  try{if(!current.HasExited)return "";int code=current.ExitCode;
   run.Drain();lock(run.Gate){string summary=run.Summary(current.Id,code);
    if(!run.Logged){run.Logged=true;Paths.Log(role,summary);}return summary;}
  }catch{return "";}
 }}
 public bool Alive{get{var current=process;if(current==null)return false;try{return !current.HasExited;}catch{return false;}}}
 public int Pid{get{var current=process;if(current==null)return 0;try{return current.HasExited?0:current.Id;}catch{return 0;}}}
 public int? ExitCode{get{var current=process;if(current==null)return null;try{return current.HasExited?(int?)current.ExitCode:null;}catch{return null;}}}
 public Engine(string name,string r){exe=Path.Combine(Paths.Bin,name);role=r;}
 public void Start(string args){
  Stop();if(!File.Exists(exe))throw new FileNotFoundException("缺少视频组件",exe);
  var si=new ProcessStartInfo(exe,args){UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=Paths.Bin,
    RedirectStandardError=true,RedirectStandardOutput=true};
  si.EnvironmentVariables["PYROSCREEN_RUNTIME"]=Paths.Runtime;
  // The installed RTSS Vulkan manifest defines this per-process opt-out.
  si.EnvironmentVariables["DISABLE_RTSS_LAYER"]="1";
  si.EnvironmentVariables["RTSSHooksProfileOverride"]="EnableHooking,0";
  // Each process owns its diagnostics; late callbacks from a disposed run
  // must never overwrite the restarted sender's last error.
  var run=new EngineExitState();diagnostic=run;
  process=new Process{StartInfo=si};
  run.Process=process;
  process.ErrorDataReceived+=(s,e)=>{run.Error(e.Data);if(e.Data!=null)Paths.Log(role,e.Data);};
  process.OutputDataReceived+=(s,e)=>{};process.Start();
  // Explicitly choose the streaming engine's scheduling class. Scheduled
  // launchers can otherwise propagate BelowNormal into capture and rendering.
  try{process.PriorityClass=ProcessPriorityClass.High;}catch(Exception e){Paths.Log(role,"Native scheduling priority unavailable: "+e.Message);}
  process.BeginErrorReadLine();process.BeginOutputReadLine();
  if(!ChildJob.Attach(process)){process.Kill();process.WaitForExit();throw new Exception("无法建立视频组件的自动清理任务。");}
 }
 public void Ensure(){if(!Alive)throw new Exception("视频组件已退出。"+ExitDiagnostic);}
 public void Stop(){
  if(process==null)return;
  // An already exited process failed before Stop was requested. Keep that
  // distinction when Apply/cleanup subsequently disposes its handle.
  bool alive=Alive;if(alive)lock(diagnostic.Gate)diagnostic.Intentional=true;
  try{if(!process.HasExited){process.CloseMainWindow();if(!process.WaitForExit(1200))process.Kill();process.WaitForExit(3000);}}catch{}
  string exitSummary=ExitDiagnostic;
  process.Dispose();process=null;
 }
 public void Dispose(){Stop();}
 public static string Q(string s){if(s.Contains("\"")||s.Contains("\0"))throw new ArgumentException();return "\""+s+"\"";}
}
// A short-lived successful restart must not reset the crash-loop budget.
// Explicit user Apply/Stop resets it; sustained healthy video resets it later.
class VideoRecovery {
 public int Attempts{get;private set;}
 public bool Pending{get;private set;}
 public bool Exhausted{get{return Attempts>=3;}}
 DateTime next,healthySince=DateTime.MinValue;
 public void Reset(){Attempts=0;Pending=false;healthySince=DateTime.MinValue;}
 public bool Failed(DateTime now){healthySince=DateTime.MinValue;if(Pending)return false;Pending=true;next=now.AddSeconds(1<<Attempts);return true;}
 public bool TryAttempt(DateTime now){if(!Pending||Exhausted||now<next)return false;++Attempts;next=now.AddSeconds(1<<Attempts);return true;}
 public void Recovered(DateTime now){Pending=false;healthySince=now;}
 public void Healthy(DateTime now){if(healthySince==DateTime.MinValue)healthySince=now;if((now-healthySince).TotalSeconds>=60)Reset();}
}
static class NetworkLink {
 public static int Mbps(IPAddress address){
  foreach(var adapter in NetworkInterface.GetAllNetworkInterfaces())try{
   if(adapter.OperationalStatus==OperationalStatus.Up&&adapter.GetIPProperties().UnicastAddresses.Any(a=>a.Address.Equals(address)))
    return (int)Math.Min(100000,Math.Max(0,adapter.Speed/1000000));
  }catch{}
  return 0;
 }
}
static class ChildJob {
 [StructLayout(LayoutKind.Sequential)]struct Basic {
  public long processTime,jobTime;public uint flags;public UIntPtr min,max;public uint count;public UIntPtr affinity;public uint priority,scheduling;
 }
 [StructLayout(LayoutKind.Sequential)]struct Io {public ulong read,write,other,readBytes,writeBytes,otherBytes;}
 [StructLayout(LayoutKind.Sequential)]struct Extended {public Basic basic;public Io io;public UIntPtr processMemory,jobMemory,peakProcess,peakJob;}
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]static extern IntPtr CreateJobObject(IntPtr security,string name);
 [DllImport("kernel32.dll")]static extern bool SetInformationJobObject(IntPtr job,int type,ref Extended info,uint length);
 [DllImport("kernel32.dll")]static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
 static readonly SafeFileHandle job=Create();
 static SafeFileHandle Create(){
  var handle=new SafeFileHandle(CreateJobObject(IntPtr.Zero,null),true);
  var limits=new Extended();limits.basic.flags=0x2000;
  if(handle.IsInvalid||!SetInformationJobObject(handle.DangerousGetHandle(),9,ref limits,(uint)Marshal.SizeOf(limits)))throw new IOException("无法创建视频组件清理任务。");
  return handle;
 }
 public static bool Attach(Process p){return AssignProcessToJobObject(job.DangerousGetHandle(),p.Handle);}
}
class DisplayLease:IDisposable {
 Process lease;
 readonly string deviceId;
 public DisplayLease(string identity){Guid value;if(!Guid.TryParse(identity,out value))throw new ArgumentException("Invalid display identity.");deviceId=value.ToString("N");}
 readonly string status=Path.Combine(Paths.Runtime,"display-"+Guid.NewGuid().ToString("N")+".status");int request;
 public string Display;public int Hz,Width,Height;
 public bool Alive{get{try{return lease!=null&&!lease.HasExited;}catch{return false;}}}
 public void Apply(int w,int h,int fps,string backend){
  int id=Alive?++request:0;
  if(id==0){
   request=0;
   string args="--backend "+backend+" --device-id "+deviceId+" --status-file "+Engine.Q(status)+" --width "+w+" --height "+h+" --refresh "+fps+" --parent "+Process.GetCurrentProcess().Id;
   bool admin=new System.Security.Principal.WindowsPrincipal(System.Security.Principal.WindowsIdentity.GetCurrent()).IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
   var info=new ProcessStartInfo(Path.Combine(Paths.Bin,"PyroScreen-Display.exe"),args){WorkingDirectory=Paths.Bin,WindowStyle=ProcessWindowStyle.Hidden,UseShellExecute=!admin,CreateNoWindow=true};
   if(!admin)info.Verb="runas";
   lease=Process.Start(info);
   if(admin&&!ChildJob.Attach(lease)){lease.Kill();throw new IOException("无法建立虚拟屏清理任务。");}
  }else Paths.Atomic(status+".mode",id+"|"+w+"|"+h+"|"+fps);
  DateTime end=DateTime.UtcNow.AddSeconds(35);
  while(DateTime.UtcNow<end){
   if(File.Exists(status)){
    string[] p=File.ReadAllText(status,Encoding.Unicode).Trim().Split('|');
    if(p.Length>=6&&p[0]=="READY"&&p[5]==id.ToString()){
     Display=p[1];Width=int.Parse(p[2]);Height=int.Parse(p[3]);Hz=int.Parse(p[4]);return;
    }
    if(p.Length>=2&&p[0]=="ERROR"&&(id==0||(p.Length>=3&&p[2]==id.ToString())))throw new Exception("虚拟屏："+String.Join(" · ",p.Skip(1))+"。请安装驱动或选择驱动支持的模式。");
   }Thread.Sleep(100);
   if(!Alive)throw new IOException("虚拟屏组件已退出。");
  }throw new Exception("等待虚拟屏超时。");
 }
 public void Dispose(){
  if(lease!=null){try{File.WriteAllText(status+".stop","stop");lease.WaitForExit(5000);}catch{}lease.Dispose();lease=null;}
  foreach(string path in new[]{status,status+".stop",status+".mode"})try{File.Delete(path);}catch{}
 }
}
static class VideoSecret {
 public static string Save(byte[] secret,string role){
  if(secret.Length!=44)throw new IOException("Invalid video secret.");string p=Path.Combine(Paths.Runtime,role+"-video.dpapi");
  File.WriteAllBytes(p,ProtectedData.Protect(secret,null,DataProtectionScope.CurrentUser));return p;
 }
 // Scales a panel mode into the supported 3840x2160 envelope while keeping its aspect ratio.
 public static void FitMode(ref int w,ref int h){
  if(w>3840||h>2160){double s=Math.Min(3840.0/w,2160.0/h);w=(int)(w*s);h=(int)(h*s);}
  w&=~1;h&=~1;
 }
 public static void ValidateMode(int w,int h,int fps){
  if(w<640||w>3840||h<480||h>2160||w%2!=0||h%2!=0||!new[]{60,120,165,240}.Contains(fps))throw new IOException("Unsupported display mode.");
 }
}
}
