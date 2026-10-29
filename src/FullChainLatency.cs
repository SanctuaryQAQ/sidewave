using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Security.Cryptography;
using System.Threading;

namespace PyroScreen {
// Local current-user timestamps only. Elevated code/binary lives in Program
// Files; the receiver and portable GUI retain their ordinary user token.
static class FullChainLatency {
 const int Slots=1024,Footer=64+Slots*64,Bytes=Footer+128;
 static readonly string Sid=WindowsIdentity.GetCurrent().User.Value;
 static readonly string TaskName="PyroScreen-DisplayLatency-"+Sid;
 static readonly string OwnerFile=Path.Combine(Paths.Root,"latency-owner.json");
 const string PresentMonHash="B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF";
 [DllImport("advapi32.dll",SetLastError=true)]static extern bool OpenProcessToken(IntPtr process,int access,out IntPtr token);
 [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr handle);
 static bool Owned(Process p){IntPtr token;if(!OpenProcessToken(p.Handle,8,out token))return false;try{using(var identity=new WindowsIdentity(token))return identity.User.Value==Sid;}finally{CloseHandle(token);}}
 static string Hash(string path){using(var s=File.OpenRead(path))using(var hash=SHA256.Create())return BitConverter.ToString(hash.ComputeHash(s)).Replace("-","");}
 static dynamic Scheduler(){dynamic s=Activator.CreateInstance(Type.GetTypeFromProgID("Schedule.Service"));s.Connect();return s;}
 public static void Start(){
  Paths.Atomic(OwnerFile,Json.Write(new{pid=Process.GetCurrentProcess().Id,session=Process.GetCurrentProcess().SessionId,runtime=Paths.Runtime}));
  try{dynamic service=Scheduler();dynamic task=service.GetFolder("\\").GetTask(TaskName);task.Run(null);}
  catch(Exception e){Paths.Log("display-latency","Sampler not installed or unavailable; use Network settings. "+e.Message);}
 }
 public static void Stop(){try{var owner=Json.Read(File.ReadAllText(OwnerFile));if(Json.I(owner,"pid")==Process.GetCurrentProcess().Id)File.Delete(OwnerFile);}catch{}}
 public static int Install(){
  if(!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))throw new IOException("Display latency setup requires Windows administrator permission.");
  string source=Process.GetCurrentProcess().MainModule.FileName,pm=Path.Combine(Paths.Bin,"PresentMon.exe");
  if(Hash(pm)!=PresentMonHash)throw new IOException("Unexpected system display sampler binary.");
  string folder=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"PyroScreen","DisplayLatency",Hash(source).Substring(0,16));Directory.CreateDirectory(folder);
  foreach(var pair in new[]{new[]{source,Path.Combine(folder,"PyroScreen.exe")},new[]{pm,Path.Combine(folder,"PresentMon.exe")}})if(!File.Exists(pair[1])||Hash(pair[0])!=Hash(pair[1]))File.Copy(pair[0],pair[1],true);
  dynamic service=Scheduler(),definition=service.NewTask(0);
  // Replace only this user's sampler task. Stop its named ETW sessions after
  // the task is stopped, so a helper upgrade cannot leave an orphan capture.
  var sessions=new HashSet<int>();string protectedRoot=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"PyroScreen","DisplayLatency")+Path.DirectorySeparatorChar;
  foreach(var helper in Process.GetProcessesByName("PyroScreen"))try{using(helper)if(helper.Id!=Process.GetCurrentProcess().Id&&helper.MainModule.FileName.StartsWith(protectedRoot,StringComparison.OrdinalIgnoreCase)&&Owned(helper))sessions.Add(helper.SessionId);}catch{}
  try{service.GetFolder("\\").GetTask(TaskName).Stop(0);}catch{}
  foreach(int oldSession in sessions)using(var stop=Process.Start(new ProcessStartInfo(Path.Combine(folder,"PresentMon.exe"),"--terminate_existing_session --session_name PyroScreenDisplayLatency-"+Sid+"-"+oldSession){UseShellExecute=false,CreateNoWindow=true})){if(!stop.WaitForExit(5000)){stop.Kill();throw new IOException("Previous display sampler did not stop.");}}
  definition.RegistrationInfo.Author="PyroScreen";definition.RegistrationInfo.Description="Current-user video frame to system display timing";
  definition.Principal.UserId=Sid;definition.Principal.LogonType=3;definition.Principal.RunLevel=1;
  definition.Settings.MultipleInstances=2;definition.Settings.ExecutionTimeLimit="PT0S";definition.Settings.DisallowStartIfOnBatteries=false;definition.Settings.StopIfGoingOnBatteries=false;
  dynamic action=definition.Actions.Create(0);action.Path=Path.Combine(folder,"PyroScreen.exe");action.Arguments="--display-latency-broker";action.WorkingDirectory=folder;
  string acl="D:P(A;;FA;;;"+Sid+")(A;;FA;;;SY)(A;;FA;;;BA)";
  dynamic task=service.GetFolder("\\").RegisterTaskDefinition(TaskName,definition,6,Sid,null,3,acl);task.Run(null);return 0;
 }
 sealed class Target:IDisposable {
  public int Pid;public string Runtime;public Process Receiver;public MemoryMappedFile Map;public MemoryMappedViewAccessor View;
  public void Dispose(){if(View!=null)View.Dispose();if(Map!=null)Map.Dispose();if(Receiver!=null)Receiver.Dispose();}
 }
 static Target Find(){
  var owner=Json.Read(File.ReadAllText(OwnerFile));int pid=Json.I(owner,"pid"),session=Json.I(owner,"session");
  string runtime=Path.GetFullPath(Json.S(owner,"runtime")),prefix=Path.GetFullPath(Path.Combine(Paths.Root,"sessions"))+Path.DirectorySeparatorChar;
  if(!runtime.StartsWith(prefix,StringComparison.OrdinalIgnoreCase)||!Path.GetFileName(runtime).StartsWith(pid+"-",StringComparison.Ordinal)||session!=Process.GetCurrentProcess().SessionId)return null;
  using(var parent=Process.GetProcessById(pid))if(parent.HasExited||parent.ProcessName!="PyroScreen"||parent.SessionId!=session||!Owned(parent))return null;
  foreach(string file in Directory.EnumerateFiles(runtime,"display-timing-*.bin")){
   Target target=null;
   try{
    var stream=new FileStream(file,FileMode.Open,FileAccess.ReadWrite,FileShare.ReadWrite|FileShare.Delete);
    if(stream.Length!=Bytes){stream.Dispose();continue;}
    target=new Target{Runtime=runtime};target.Map=MemoryMappedFile.CreateFromFile(stream,null,Bytes,MemoryMappedFileAccess.ReadWrite,null,HandleInheritability.None,false);target.View=target.Map.CreateViewAccessor(0,Bytes,MemoryMappedFileAccess.ReadWrite);
    if(target.View.ReadUInt32(0)!=0x50594c54||target.View.ReadUInt32(4)!=1||target.View.ReadUInt32(12)!=Slots){target.Dispose();continue;}
    target.Pid=(int)target.View.ReadUInt32(8);target.Receiver=Process.GetProcessById(target.Pid);
    if(target.Receiver.HasExited||target.Receiver.ProcessName!="PyroScreen-Receiver"||target.Receiver.SessionId!=session||!Owned(target.Receiver)){target.Dispose();continue;}
    return target;
   }catch{if(target!=null)target.Dispose();}
  }
  return null;
 }
 sealed class Capture:IDisposable {
  readonly Target target;readonly Process process;readonly Thread reader;readonly string session;
  readonly long frequency;long lastValid,lastDisplay,lastPublish;ulong count,unmatched,dropped,repeats,rows;int guard;
  readonly Queue<double> latencies=new Queue<double>();readonly Queue<long> displayed=new Queue<long>(),sources=new Queue<long>();readonly HashSet<long> seen=new HashSet<long>();double bound;
  Dictionary<string,int> columns;
  public bool Alive{get{return reader.IsAlive&&!process.HasExited&&!target.Receiver.HasExited;}}
  public int Pid{get{return target.Pid;}}
  public Capture(Target t){target=t;frequency=t.View.ReadInt64(16);if(frequency!=Stopwatch.Frequency)throw new IOException("Display clock frequency mismatch.");
   session="PyroScreenDisplayLatency-"+Sid+"-"+Process.GetCurrentProcess().SessionId;
   process=Process.Start(new ProcessStartInfo(Path.Combine(Paths.Bin,"PresentMon.exe"),"--process_id "+t.Pid+" --output_stdout --v1_metrics --qpc_time --no_console_stats --no_track_input --no_track_gpu --track_hybrid_present --terminate_on_proc_exit --stop_existing_session --session_name "+session){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true});
   process.ErrorDataReceived+=(s,e)=>{if(!String.IsNullOrEmpty(e.Data))Paths.Log("display-latency",e.Data);};process.BeginErrorReadLine();
   reader=new Thread(Read){IsBackground=true,Name="System display feedback"};reader.Start();
  }
  string Field(string[] values,string key){int at;return columns.TryGetValue(key,out at)&&at<values.Length?values[at].Trim('"'):"";}
  long Match(long qpc){
   // Present QPCs increase with sequence. Binary search avoids walking several
   // hundred delayed ETW markers for every displayed frame. Guard checks and
   // bounded retries protect against the ring wrapping while it is searched.
   for(int retry=0;retry<3;++retry){
    long latest=target.View.ReadInt64(24),low=Math.Max(1,latest-Slots+1),high=latest,best=0,deltaBest=Int64.MaxValue;bool raced=false;
    while(low<=high){
     long seq=low+(high-low)/2,at=64+(seq%Slots)*64;int guardBefore=target.View.ReadInt32(at);Thread.MemoryBarrier();long stamp=target.View.ReadInt64(at+8);Thread.MemoryBarrier();
     if(guardBefore!=(int)(seq*2+2)||guardBefore!=target.View.ReadInt32(at)){raced=true;break;}
     long delta=Math.Abs(stamp-qpc);if(delta<deltaBest){best=seq;deltaBest=delta;}
     if(stamp<qpc)low=seq+1;else high=seq-1;
    }
    if(!raced)return deltaBest<=frequency/2000?best:0;
   }
   return 0;
  }
  void Read(){try{string line;while((line=process.StandardOutput.ReadLine())!=null){
    line=line.TrimStart('\ufeff');var values=line.Split(',');
    if(line.StartsWith("Application,",StringComparison.OrdinalIgnoreCase)){columns=new Dictionary<string,int>(StringComparer.OrdinalIgnoreCase);for(int i=0;i<values.Length;++i)columns[values[i].Trim('"')]=i;continue;}
    if(columns==null)continue;int pid;long qpc;double until;
    string runtime=Field(values,"Runtime");if(runtime=="")runtime=Field(values,"PresentRuntime");if(runtime!=""&&runtime!="DXGI")continue;
    if(!Int32.TryParse(Field(values,"ProcessID"),out pid)||pid!=target.Pid||!Int64.TryParse(Field(values,"QPCTime"),out qpc))continue;
    ++rows;if(Field(values,"Dropped")!="0"){++dropped;continue;}
    if(!Double.TryParse(Field(values,"msUntilDisplayed"),NumberStyles.Float,CultureInfo.InvariantCulture,out until)||Double.IsNaN(until)||Double.IsInfinity(until)||until<0)continue;
    long shown=qpc+(long)(until*frequency/1000.0);displayed.Enqueue(shown);while(displayed.Count>1&&shown-displayed.Peek()>frequency)displayed.Dequeue();
    long best=Match(qpc);
    if(best==0){++unmatched;Publish();continue;}
    long slot=64+(best%Slots)*64;int before=target.View.ReadInt32(slot);Thread.MemoryBarrier();
    long marker=target.View.ReadInt64(slot+8),utc=target.View.ReadInt64(slot+16),source=target.View.ReadInt64(slot+24),offset=target.View.ReadInt64(slot+32),error=target.View.ReadInt64(slot+40);bool calibrated=target.View.ReadUInt32(slot+48)==1,original=target.View.ReadUInt32(slot+52)==0;
    Thread.MemoryBarrier();if(before!=(int)(best*2+2)||before!=target.View.ReadInt32(slot)){++unmatched;continue;}
    double latency=(utc+(qpc-marker)*1000000.0/frequency+until*1000-source-offset)/1000;
    if(!calibrated||!original||source<=0||error<0||error>1000000||latency<0||latency>=5000)continue;
    if(!seen.Add(source)){++repeats;Publish();continue;}sources.Enqueue(source);if(sources.Count>1024)seen.Remove(sources.Dequeue());
    latencies.Enqueue(latency);if(latencies.Count>512)latencies.Dequeue();++count;bound=error/1000.0;lastValid=Stopwatch.GetTimestamp();lastDisplay=shown;Publish();
   }}catch(Exception e){Paths.Log("display-latency","Feedback reader: "+e.Message);}}
  void Publish(){long now=Stopwatch.GetTimestamp();if(now-lastPublish<frequency/4)return;lastPublish=now;
   var sorted=latencies.ToArray();Array.Sort(sorted);var recent=latencies.ToArray();double mean=0;int n=Math.Min(12,recent.Length);for(int i=recent.Length-n;i<recent.Length;++i)mean+=recent[i];if(n>0)mean/=n;
   double fps=0;if(displayed.Count>1){var times=displayed.ToArray();long span=times[times.Length-1]-times[0];if(span>0)fps=(times.Length-1)*frequency/(double)span;}
   var v=target.View;int seq=++guard;v.Write(Footer,seq*2+1);Thread.MemoryBarrier();v.Write(Footer+4,n>0?1u:0u);v.Write(Footer+8,count);v.Write(Footer+16,(ulong)sorted.Length);v.Write(Footer+24,0L);v.Write(Footer+32,lastValid);v.Write(Footer+40,lastDisplay);v.Write(Footer+48,mean);v.Write(Footer+56,sorted.Length>0?sorted[(sorted.Length-1)/2]:0);v.Write(Footer+64,sorted.Length>0?sorted[(sorted.Length-1)*95/100]:0);v.Write(Footer+72,fps);v.Write(Footer+80,unmatched);v.Write(Footer+88,dropped);v.Write(Footer+96,repeats);v.Write(Footer+104,bound);v.Write(Footer+112,0L);v.Write(Footer+120,rows);Thread.MemoryBarrier();v.Write(Footer,seq*2+2);
  }
  public void Dispose(){
   if(!process.HasExited)try{using(var stop=Process.Start(new ProcessStartInfo(Path.Combine(Paths.Bin,"PresentMon.exe"),"--terminate_existing_session --session_name "+session){UseShellExecute=false,CreateNoWindow=true})){stop.WaitForExit(3000);}}catch{}
   if(!process.WaitForExit(5000))process.Kill();reader.Join(2000);process.Dispose();target.Dispose();
  }
 }
 public static int Broker(){
  if(!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator))return 1;
  Process.GetCurrentProcess().PriorityClass=ProcessPriorityClass.BelowNormal;Capture capture=null;long missing=Stopwatch.GetTimestamp();
  try{while(true){
   try{
    if(!File.Exists(OwnerFile)){if(capture!=null){capture.Dispose();capture=null;}if(Stopwatch.GetTimestamp()-missing>15*Stopwatch.Frequency)break;Thread.Sleep(500);continue;}
    Target next=Find();if(next==null){if(capture!=null&&!capture.Alive){capture.Dispose();capture=null;}Thread.Sleep(500);continue;}missing=Stopwatch.GetTimestamp();
    if(capture!=null&&capture.Alive&&capture.Pid==next.Pid){next.Dispose();}else{if(capture!=null)capture.Dispose();capture=new Capture(next);Paths.Log("display-latency","System display sampler started for receiver "+next.Pid);}
   }catch(Exception e){Paths.Log("display-latency",e.Message);if(capture!=null){capture.Dispose();capture=null;}if(Stopwatch.GetTimestamp()-missing>15*Stopwatch.Frequency)break;}
   Thread.Sleep(500);
  }}finally{if(capture!=null)capture.Dispose();}return 0;
 }
}
}
