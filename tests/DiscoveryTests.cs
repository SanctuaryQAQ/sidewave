using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
namespace PyroScreen {
class DiscoveryTests {
 [STAThread] public static int Main(string[] args) {
  if(args.Length!=3)return 2;
  var replies=new List<object>();var timer=Stopwatch.StartNew();
  bool found=false;int scans=0;
  try {
   var interfaces=NetworkInterface.GetAllNetworkInterfaces().Where(n=>n.OperationalStatus==OperationalStatus.Up)
    .SelectMany(n=>n.GetIPProperties().UnicastAddresses.Where(a=>a.Address.AddressFamily==AddressFamily.InterNetwork&&a.IPv4Mask!=null&&!IPAddress.IsLoopback(a.Address))
     .Select(a=>new {name=n.Name,ip=a.Address.ToString(),mask=a.IPv4Mask.ToString()})).ToArray();
   using(var discovery=new Discovery(Guid.NewGuid().ToString("N"),null)) {
    discovery.Found+=p=>{lock(replies){replies.Add(new {name=p.Name,ip=p.Ip,width=p.Width,height=p.Height,hz=p.Hz,elapsedMs=timer.ElapsedMilliseconds});
     if(p.Name==args[0]&&p.Ip==args[1])found=true;}};
    // Expected identity is only checked on received replies. Never send to the expected IP.
    for(int i=0;i<6;i++){discovery.Scan();scans++;Thread.Sleep(1000);}
   }
   lock(replies)File.WriteAllText(args[2],Json.Write(new {passed=found,broadcastOnly=true,manualTargets=0,scans=scans,interfaces=interfaces,replies=replies}),new System.Text.UTF8Encoding(false));
   return found?0:1;
  }catch(Exception e){File.WriteAllText(args[2]+".error",e.ToString());return 1;}
 }
}
}
