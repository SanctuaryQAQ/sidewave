using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
namespace SideWave {
// Drives a real receiver through pairing, display modes, transport options and failure recovery.
// Usage: TwoMachineMatrix <receiver-ip> <results-dir> <workload-exe> <backend> <hook.ps1> [quick]
// The hook script runs receiver-side actions: get-code, kill-receiver, restart-app, flap-network.
class TwoMachineMatrix {
 static string report,ip,workload,hook,results;
 static Settings settings;static HostLink link;
 static int passed,lowFps,failed;
 static void Record(object data){File.AppendAllText(report,Json.Write(data)+"\n");}
 static string Hook(string action){
  var info=new ProcessStartInfo("powershell.exe","-NoProfile -ExecutionPolicy Bypass -File "+Engine.Q(hook)+" -Action "+action){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true};
  using(var process=Process.Start(info)){
   var error=process.StandardError.ReadToEndAsync();string output=process.StandardOutput.ReadToEnd();
   if(!process.WaitForExit(120000)){process.Kill();throw new TimeoutException("Hook timed out: "+action);}
   if(process.ExitCode!=0)throw new IOException("Hook "+action+" failed: "+error.Result);
   return output.Trim();
  }
 }
 static void Step(string name,Action action){
  var timer=Stopwatch.StartNew();
  try{action();passed++;Record(new{test=name,status="pass",ms=timer.ElapsedMilliseconds});}
  catch(Exception e){failed++;Record(new{test=name,status="fail",ms=timer.ElapsedMilliseconds,error=e.GetType().Name+": "+e.Message});}
 }
 static void Reject(string name,Action action){
  Step(name,()=>{
   bool accepted=false;
   try{action();accepted=true;}catch(Exception){}
   if(accepted)throw new Exception("Expected a rejection.");
  });
 }
 static double Number(Dictionary<string,object> data,string key){object value;return data.TryGetValue(key,out value)&&value!=null?Convert.ToDouble(value):0;}
 static void Ensure(){
  if(link.Connected)return;
  Exception last=null;
  for(int attempt=0;attempt<30;attempt++){
   try{link.Connect(ip);return;}catch(Exception e){last=e;Thread.Sleep(2000);}
  }
  throw new IOException("Could not reconnect: "+last.Message);
 }
 // Streams a scrolling document for the given time and checks real receiver telemetry.
 static void Measure(string name,int width,int height,int fps,int budget,int seconds,bool apply=true){
  var timer=Stopwatch.StartNew();Process source=null;
  try{
   Ensure();
   if(apply)link.Apply(width,height,fps,budget);
   string state=Path.Combine(Paths.Runtime,"host-session.json"),display=Json.S(Json.Read(File.ReadAllText(state)),"display");
   source=Process.Start(new ProcessStartInfo(workload,Engine.Q(display)+" "+Engine.Q(Path.Combine(Paths.Bin,"MainWindow.xaml"))+" --seconds "+(seconds+6)){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
   source.BeginOutputReadLine();source.BeginErrorReadLine();
   var samples=new List<Dictionary<string,object>>();DateTime end=DateTime.UtcNow.AddSeconds(seconds);
   while(DateTime.UtcNow<end){
    Thread.Sleep(1000);var response=link.Stats();
    var data=response!=null&&response.ContainsKey("data")?response["data"]as Dictionary<string,object>:null;
    if(data!=null)samples.Add(data);
   }
   if(samples.Count<6)throw new Exception("Only "+samples.Count+" telemetry samples.");
   var first=samples.Skip(3).First();var last=samples.Last();
   double span=(Number(last,"sampled_utc_ms")-Number(first,"sampled_utc_ms"))/1000.0;
   double actual=span>0?(Number(last,"presented_frames")-Number(first,"presented_frames"))/span:0;
   if(actual<=0)throw new Exception("Telemetry shows no presented frames.");
   int target=Math.Min(fps,link.VirtualHz>0?link.VirtualHz:fps);
   bool low=actual<target*.80;
   if(actual>target*1.25+2)throw new Exception("Frame rate "+actual.ToString("F1")+" above the sender limit for "+target+".");
   if(low)lowFps++;else passed++;
   Record(new{test=name,status=low?"low-fps":"pass",ms=timer.ElapsedMilliseconds,width,height,requested=fps,virtualHz=link.VirtualHz,fps=Math.Round(actual,1),
    mbps=Math.Round(samples.Skip(3).Average(s=>Number(s,"receive_mbps"))),incomplete=Number(last,"incomplete_packets")-Number(first,"incomplete_packets"),
    latencyP50=Number(last,"full_chain_latency_recent_p50_ms"),decodeMs=Number(last,"decode_mean_ms"),localMbps=link.LocalLinkMbps,remoteMbps=link.RemoteLinkMbps});
  }catch(Exception e){
   failed++;Record(new{test=name,status="fail",ms=timer.ElapsedMilliseconds,width,height,requested=fps,error=e.GetType().Name+": "+e.Message});
   try{if(link.Connected)link.StopVideo();}catch(Exception){}
  }finally{
   if(source!=null){try{if(!source.HasExited)source.Kill();source.WaitForExit(3000);}catch(Exception){}source.Dispose();}
  }
 }
 static void Option(string name,Action<Settings> enable,Action<Settings> disable){
  enable(settings);
  Measure("option "+name,2560,1440,120,600,10);
  disable(settings);
 }
 public static int Main(string[] args){
  if(args.Length<5)return 2;
  Paths.Init();
  ip=args[0];results=args[1];workload=args[2];hook=args[4];bool quick=args.Length>5&&args[5]=="quick";
  Directory.CreateDirectory(results);report=Path.Combine(results,"matrix.jsonl");
  settings=Settings.Load("host");settings.Backend=args[3];
  settings.VideoEncryption=settings.AllowTearing=settings.PartialFrames=settings.ClientCursor=settings.PacketProtection=false;
  settings.ForgetTokens("host");settings.LastId=settings.LastIp=settings.LastPin="";
  link=new HostLink(settings);
  try{
   // Pairing.
   Reject("pair rejects malformed code",()=>link.Connect(ip,"123"));
   Reject("pair rejects wrong code",()=>link.Connect(ip,"AAAA-AAAA-AAAA-AAAA"));
   Reject("pair requires a code",()=>link.Connect(ip));
   Reject("connect rejects bad address",()=>link.Connect("not-an-ip"));
   Step("pair with correct code",()=>link.Connect(ip,Hook("get-code")));
   if(!link.Connected)throw new IOException("Pairing failed; the remaining tests need a connection.");
   Record(new{test="link",status="info",name=link.Name,nativeWidth=link.NativeWidth,nativeHeight=link.NativeHeight,panelHz=link.PanelHz,localMbps=link.LocalLinkMbps,remoteMbps=link.RemoteLinkMbps});
   string first=link.ConnectionId;
   Step("reconnect without a code",()=>{link.Disconnect();Thread.Sleep(400);link.Connect(ip);if(link.ConnectionId==first)throw new Exception("Connection was not replaced.");});
   // Display modes.
   int[][] modes=quick?new[]{new[]{1920,1080,60,400},new[]{2560,1440,120,600},new[]{2560,1440,165,750}}:new[]{
    new[]{1280,720,60,400},new[]{1600,900,60,400},new[]{1920,1080,60,400},new[]{1920,1080,120,600},new[]{1920,1080,165,600},new[]{1920,1080,240,750},
    new[]{2048,1152,120,600},new[]{2560,1440,60,600},new[]{2560,1440,120,600},new[]{2560,1440,165,750},new[]{2560,1440,240,900},
    new[]{3200,1800,60,750},new[]{3200,1800,120,900},new[]{3840,2160,60,900},new[]{3840,2160,120,900}};
   foreach(var m in modes)Measure("mode "+m[0]+"x"+m[1]+"@"+m[2]+" "+m[3]+"Mbps",m[0],m[1],m[2],m[3],quick?8:10);
   Measure("mode native",link.NativeWidth,link.NativeHeight,link.PanelHz>=60?Math.Min(link.PanelHz,240):60,600,8);
   // Transport options, one at a time and together.
   Option("encryption",s=>s.VideoEncryption=true,s=>s.VideoEncryption=false);
   Option("low latency",s=>s.AllowTearing=true,s=>s.AllowTearing=false);
   Option("changed regions",s=>s.PartialFrames=true,s=>s.PartialFrames=false);
   if(!quick){
    Option("hardware cursor",s=>s.ClientCursor=true,s=>s.ClientCursor=false);
    Option("packet protection",s=>s.PacketProtection=true,s=>s.PacketProtection=false);
    Option("all together",s=>{s.VideoEncryption=s.AllowTearing=s.PartialFrames=s.ClientCursor=s.PacketProtection=true;},s=>{s.VideoEncryption=s.AllowTearing=s.PartialFrames=s.ClientCursor=s.PacketProtection=false;});
   }
   // Lifecycle and recovery.
   Measure("baseline",2560,1440,120,600,6);
   Step("same settings keep the engine",()=>{int pid=link.SenderPid;link.Apply(2560,1440,120,600);if(link.SenderPid!=pid)throw new Exception("Sender restarted.");});
   Step("rapid mode changes",()=>{foreach(var m in new[]{new[]{1920,1080,60},new[]{2560,1440,165},new[]{1280,720,120},new[]{2560,1440,60},new[]{1920,1080,240}})link.Apply(m[0],m[1],m[2],600);});
   Measure("video after rapid changes",1920,1080,240,600,6,false);
   Step("stop video keeps the control connection",()=>{string connection=link.ConnectionId;link.StopVideo();if(link.Streaming)throw new Exception("Sender still running.");if(link.ConnectionId!=connection||!link.Connected)throw new Exception("Connection changed.");});
   Measure("video after stop",2560,1440,120,600,6);
   Step("sender crash",()=>{using(var sender=Process.GetProcessById(link.SenderPid)){sender.Kill();sender.WaitForExit(5000);}Thread.Sleep(1000);if(link.Streaming)throw new Exception("Crash not detected.");link.RecoverVideo();});
   Measure("video after sender crash",2560,1440,120,600,6,false);
   Step("receiver crash",()=>{Hook("kill-receiver");Thread.Sleep(3000);});
   Measure("video after receiver crash",2560,1440,120,600,6);
   Step("disconnect releases the display",()=>{link.Disconnect();if(link.HasDisplay)throw new Exception("Display still leased.");});
   Measure("video after reconnect",2560,1440,120,600,6);
   if(!quick){
    Step("receiver app restart",()=>{Hook("restart-app");Thread.Sleep(2000);link.DropConnection(true);});
    Measure("video after receiver restart",2560,1440,120,600,6);
    Step("network drop",()=>{Hook("flap-network");Thread.Sleep(3000);link.DropConnection(true);});
    Measure("video after network drop",2560,1440,120,600,6);
   }
   // A forgotten pairing needs a new code.
   Step("forget and pair again",()=>{link.Disconnect();settings.ForgetTokens("host");settings.LastId=settings.LastIp=settings.LastPin="";Thread.Sleep(400);
    bool rejected=false;try{link.Connect(ip);}catch(Exception){rejected=true;}
    if(!rejected)throw new Exception("Connected without a pairing.");
    link.Connect(ip,Hook("get-code"));});
  }catch(Exception e){failed++;Record(new{test="aborted",status="fail",error=e.ToString()});}
  finally{try{link.Dispose();}catch(Exception){}}
  Record(new{test="summary",status=failed==0?"pass":"fail",passed,lowFps,failed});
  return failed==0?0:1;
 }
}
}
