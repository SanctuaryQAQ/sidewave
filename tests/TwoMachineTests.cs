using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
namespace PyroScreen {
class TwoMachineTests {
 static void Record(string path,object data){File.AppendAllText(path,Json.Write(data)+"\n");}
 public static int Main(string[] args){
  if(args.Length<3)return 1;Paths.Init();
  string ip=args[0],pairPath=args[1],results=args[2];Directory.CreateDirectory(results);
  string report=Path.Combine(results,"acceptance.jsonl");
  try{
   string code=File.ReadAllText(pairPath).Trim();File.Delete(pairPath);
   var settings=Settings.Load("host");
   settings.Backend=args.Length>=5?args[4]:"vdd";
   using(var link=new HostLink(settings)){
    link.Connect(ip,code);string connection=link.ConnectionId;
    Record(report,new{step="pair",ok=true,connection=connection,nativeWidth=link.NativeWidth,nativeHeight=link.NativeHeight,panelHz=link.PanelHz});
    int[][] modes={new[]{2560,1440,60},new[]{2560,1440,120},new[]{2560,1440,165},new[]{2560,1440,240}};
    if(settings.Backend=="vdd")modes=new[]{new[]{2560,1440,60},new[]{2560,1440,120},new[]{1920,1080,120},new[]{2560,1440,165},new[]{2560,1440,240}};
    foreach(var m in modes){
     link.Apply(m[0],m[1],m[2],600);
     if(link.ConnectionId!=connection)throw new Exception("Mode change replaced control connection.");
     Process workload=null;
     string state=Path.Combine(Paths.Runtime,"host-session.json"),display=Json.S(Json.Read(File.ReadAllText(state)),"display");
     if(args.Length>=4){
      workload=Process.Start(new ProcessStartInfo(args[3],Engine.Q(display)+" "+Engine.Q(Path.Combine(Paths.Bin,"MainWindow.xaml"))+" --seconds 32"){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
      workload.BeginOutputReadLine();workload.BeginErrorReadLine();
     }
     var samples=new List<Dictionary<string,object>>();DateTime end=DateTime.UtcNow.AddSeconds(25);
     while(DateTime.UtcNow<end){
      Thread.Sleep(1000);var response=link.Stats();var data=response.ContainsKey("data")?response["data"]as Dictionary<string,object>:null;
      if(data!=null){samples.Add(data);Record(Path.Combine(results,m[0]+"x"+m[1]+"-"+m[2]+".jsonl"),data);}
      if(DateTime.UtcNow.Second%5==0)link.Calibrate();
     }
     if(workload!=null){if(!workload.HasExited){workload.Kill();workload.WaitForExit();}workload.Dispose();}
     if(samples.Count<10)throw new Exception("Missing real receiver telemetry.");
     var first=samples.Skip(5).First();var last=samples.Last();
     double seconds=(Json.L(last,"sampled_utc_ms")-Json.L(first,"sampled_utc_ms"))/1000.0;
     double fps=(Json.L(last,"presented_frames")-Json.L(first,"presented_frames"))/seconds;
     Record(report,new{step="mode",width=m[0],height=m[1],requested=m[2],virtualHz=link.VirtualHz,panelHz=link.PanelHz,actualFps=fps,sameControlSession=link.ConnectionId==connection});
     if(fps<Math.Min(m[2],link.VirtualHz)*.80||fps>Math.Min(m[2],link.VirtualHz)*1.08)throw new Exception("Actual frame-rate acceptance failed.");
    }
    link.StopVideo();link.Disconnect();Thread.Sleep(400);link.Connect(ip);
    Record(report,new{step="savedReconnect",ok=true,newConnection=link.ConnectionId!=connection});
    link.Apply(2560,1440,120,600);
    Record(report,new{step="completed",ok=true,finalMode="2560x1440@120"});
    // Leave a real stream only when launched by the UI; this test releases its lease.
   }return 0;
  }catch(Exception e){Record(report,new{step="failed",error=e.ToString()});return 1;}
 }
}
}
