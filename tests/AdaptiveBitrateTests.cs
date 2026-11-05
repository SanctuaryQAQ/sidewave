using System;
using System.IO;
namespace SideWave {
class AdaptiveBitrateTests {
 static int checks;
 static void Check(bool value,string message){++checks;if(!value)throw new Exception(message);}
 // Feeds one second of receiver frames with the given loss; returns whether Scale changed.
 static long started,incomplete;static DateTime now;
 static bool Second(AdaptiveBitrate p,int frames,int lost){started+=frames;incomplete+=lost;now=now.AddSeconds(1);return p.Observe(now,started,incomplete);}
 static void Start(AdaptiveBitrate p){p.Reset();started=1000;incomplete=10;now=new DateTime(2026,11,3,0,0,0,DateTimeKind.Utc);Check(!p.Observe(now,started,incomplete),"Baseline sample changed the budget.");}
 static void EffectiveCut(){
  var p=new AdaptiveBitrate();Start(p);
  Check(!Second(p,165,2)&&p.Scale==1,"Normal loss lowered the budget.");
  Check(!Second(p,165,17),"One lossy second lowered the budget.");
  Check(Second(p,165,17)&&Math.Abs(p.Scale-0.85)<1e-9,"Sustained loss did not lower the budget.");
  for(int i=0;i<3;++i)Second(p,165,0);
  Check(Math.Abs(p.Scale-0.85)<1e-9,"An effective cut was undone.");
  for(int i=0;i<4;++i)Check(!Second(p,165,0),"Recovery was too fast.");
  Check(Second(p,165,0)&&Math.Abs(p.Scale-0.90)<1e-9,"Clean delivery did not raise the budget.");
  for(int i=0;i<10;++i)Second(p,165,0);
  Check(p.Scale==1,"Budget exceeded or missed the full value.");
 }
 static void IneffectiveCut(){
  var p=new AdaptiveBitrate();Start(p);
  Second(p,165,17);Second(p,165,17);
  Check(p.Scale<1,"Loss did not trigger a trial cut.");
  for(int i=0;i<3;++i)Second(p,165,16);
  Check(p.Scale==1,"A cut that did not reduce loss was kept.");
  for(int i=0;i<50;++i)Second(p,165,17);
  Check(p.Scale==1,"Bandwidth-independent loss was cut again while suspended.");
  for(int i=0;i<12;++i)Second(p,165,17);
  Check(p.Scale<1,"Suspension never expired.");
 }
 static void Guards(){
  var p=new AdaptiveBitrate();Start(p);
  for(int i=0;i<4;++i)Check(!Second(p,10,5),"An idle desktop was treated as loss.");
  Check(p.Scale==1,"Few frames changed the budget.");
  started=5;incomplete=0;now=now.AddSeconds(1);
  Check(!p.Observe(now,started,incomplete),"Receiver restart produced a negative interval.");
  Check(!Second(p,165,0),"Interval after restart was not a clean baseline.");
  // Each round: loss returns, a cut clears it. Repeated cuts stop at the floor.
  for(int round=0;round<8;++round){Second(p,165,33);Second(p,165,33);for(int i=0;i<3;++i)Second(p,165,0);}
  Check(Math.Abs(p.Scale-AdaptiveBitrate.Floor)<1e-9,"Budget left its floor: "+p.Scale);
 }
 public static int Main(string[] args){
  try{EffectiveCut();IneffectiveCut();Guards();File.WriteAllText(args[0],Json.Write(new{passed=true,checks=checks}));return 0;}
  catch(Exception e){File.WriteAllText(args[0],Json.Write(new{passed=false,checks=checks,error=e.ToString()}));return 1;}
 }
}
}
