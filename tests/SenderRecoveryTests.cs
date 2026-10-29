using System;
using System.IO;
using System.Reflection;
using System.Threading;
namespace PyroScreen {
class SenderRecoveryTests {
 static int checks;
 static void Check(bool value,string message){++checks;if(!value)throw new Exception(message);}
 static void Policy(){
  var p=new VideoRecovery();DateTime t=new DateTime(2026,10,7,0,0,0,DateTimeKind.Utc);
  Check(p.Failed(t),"First exit was not observed.");
  Check(!p.Failed(t.AddMilliseconds(50)),"Exit was reported twice.");
  Check(!p.TryAttempt(t.AddMilliseconds(999)),"Restart had no backoff.");
  Check(p.TryAttempt(t.AddSeconds(1)),"First retry missing.");
  Check(!p.TryAttempt(t.AddSeconds(2)),"Second retry was too early.");
  Check(p.TryAttempt(t.AddSeconds(3)),"Second retry missing.");
  Check(!p.TryAttempt(t.AddSeconds(6)),"Third retry was too early.");
  Check(p.TryAttempt(t.AddSeconds(7)),"Third retry missing.");
  Check(p.Exhausted&&!p.TryAttempt(t.AddHours(1)),"Crash loop exceeded three retries.");
  p.Recovered(t.AddSeconds(8));p.Healthy(t.AddSeconds(67));
  Check(p.Attempts==3,"Short-lived recovery reset the retry budget.");
  Check(p.Failed(t.AddSeconds(67))&&!p.TryAttempt(t.AddHours(2)),"Fourth crash restarted automatically.");
  p.Reset();Check(!p.Pending&&!p.Exhausted&&p.Attempts==0,"Explicit stop did not cancel recovery.");
  p.Failed(t);p.TryAttempt(t.AddSeconds(1));p.Recovered(t.AddSeconds(2));p.Healthy(t.AddSeconds(62));
  Check(p.Attempts==0&&!p.Pending,"Healthy video did not replenish retry budget.");
 }
 static void EngineProcess(){
  string folder=Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location),log=Path.Combine(folder,"engine-fixture.log");
  if(File.Exists(log))File.Delete(log);
  using(var engine=new Engine(Assembly.GetExecutingAssembly().Location,Path.Combine(folder,"engine-fixture"))){
   engine.Start("--crash-child");DateTime end=DateTime.UtcNow.AddSeconds(5);
   while(engine.Alive&&DateTime.UtcNow<end)Thread.Sleep(20);
   Check(!engine.Alive,"Crash fixture failed to exit.");
   string diagnostic=engine.ExitDiagnostic;
   Check(engine.ExitCode==22&&diagnostic.Contains("code=22 (0x00000016)"),"Exit code was lost.");
   Check(diagnostic.Contains("intentional=False")&&diagnostic.EndsWith("UDP send failed: 10055"),"Unexpected exit lost stderr/reason.");
   Check(diagnostic.Length<2250,"Exit diagnostic was not bounded.");
   engine.Stop();Check(File.ReadAllText(log).Contains("intentional=False"),"Cleanup relabeled crash as intentional.");
   engine.Start("--wait-child");engine.Stop();
   Check(File.ReadAllText(log).Contains("intentional=True"),"Requested Stop was not distinguished.");
  }
  var oldRun=new EngineExitState();oldRun.Error("old failure");
  var newRun=new EngineExitState();newRun.Error("new failure");oldRun.Error("late old callback");
  Check(newRun.Summary(1,22).EndsWith("new failure"),"Previous process callback contaminated new diagnostics.");
 }
 public static int Main(string[] args){
  if(args.Length>0&&args[0]=="--crash-child"){Console.Error.WriteLine(new String('x',4096)+"UDP send failed: 10055");Thread.Sleep(100);return 22;}
  if(args.Length>0&&args[0]=="--wait-child"){Console.Error.WriteLine("running");Thread.Sleep(15000);return 0;}
  try{Policy();EngineProcess();File.WriteAllText(args[0],Json.Write(new{passed=true,checks=checks,processExitCode=22,maxAutomaticRetries=3}));return 0;}
  catch(Exception e){File.WriteAllText(args[0],Json.Write(new{passed=false,checks=checks,error=e.ToString()}));return 1;}
 }
}
}
