using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Threading;
using System.Windows;

namespace SideWave {
// The one-file build embeds the complete app bundle. It unpacks to a fixed per-user folder, so
// firewall rules and the startup entry stay valid wherever the downloaded exe is kept.
static class SingleFile {
 public const string Resource="SideWave.bundle.zip";
 const string Marker=".bundle-sha256";
 public static bool TryRun(string[] args,out int exit){
  exit=0;
  using(var bundle=Assembly.GetExecutingAssembly().GetManifestResourceStream(Resource)){
   if(bundle==null)return false;
   try{
    string target=Path.Combine(Paths.Root,"app"),app=Path.Combine(target,"SideWave.exe");
    using(var gate=new Mutex(false,@"Local\SideWave-Unpack-"+WindowsIdentity.GetCurrent().User.Value)){
     bool owned=false;
     try{owned=gate.WaitOne(120000);}catch(AbandonedMutexException){owned=true;}
     try{Unpack(bundle,target,app);}finally{if(owned)gate.ReleaseMutex();}
    }
    var process=Process.Start(new ProcessStartInfo(app,String.Join(" ",args.Select(Engine.Q))){UseShellExecute=false,WorkingDirectory=target});
    // Commands report through the exit code; a normal launch keeps running on its own.
    if(LocalControl.IsRequest(args)){process.WaitForExit();exit=process.ExitCode;}
   }catch(Exception e){
    Paths.Log("startup",e.ToString());Language.Load();
    MessageBox.Show(Language.Tr("无法解压运行文件：")+e.Message,"SideWave");exit=1;
   }
   return true;
  }
 }
 static void Unpack(Stream bundle,string target,string app){
  string hash;
  using(var sha=SHA256.Create())hash=BitConverter.ToString(sha.ComputeHash(bundle)).Replace("-","");
  bundle.Position=0;
  using(var zip=new ZipArchive(bundle,ZipArchiveMode.Read,true)){
   if(Current(target,hash,zip))return;
   Directory.CreateDirectory(Paths.Root);
   string staging=target+".new-"+Guid.NewGuid().ToString("N");
   try{
    zip.ExtractToDirectory(staging);
    File.WriteAllText(Path.Combine(staging,Marker),hash);
    if(Directory.Exists(target))Retire(target,app);
    Directory.Move(staging,target);
   }catch{
    try{if(Directory.Exists(staging))Directory.Delete(staging,true);}catch(IOException){}catch(UnauthorizedAccessException){}
    throw;
   }
  }
  foreach(string old in Directory.GetDirectories(Paths.Root,"app.old-*"))try{Directory.Delete(old,true);}catch(IOException){}catch(UnauthorizedAccessException){}
 }
 static bool Current(string target,string hash,ZipArchive zip){
  string marker=Path.Combine(target,Marker);
  if(!File.Exists(marker)||File.ReadAllText(marker).Trim()!=hash)return false;
  foreach(var entry in zip.Entries){
   if(entry.Name.Length==0)continue;
   var file=new FileInfo(Path.Combine(target,entry.FullName));
   if(!file.Exists||file.Length!=entry.Length)return false;
  }
  return true;
 }
 // Asks a running older copy to exit, then moves it aside so the new folder can take its place.
 static void Retire(string target,string app){
  if(File.Exists(app))try{
   using(var quit=Process.Start(new ProcessStartInfo(app,"--quit"){UseShellExecute=false,CreateNoWindow=true}))quit.WaitForExit(15000);
  }catch(Exception e){Paths.Log("startup","Previous copy did not accept quit: "+e.Message);}
  string old=target+".old-"+Guid.NewGuid().ToString("N");
  for(int attempt=0;;attempt++){
   try{Directory.Move(target,old);return;}
   catch(Exception e){
    if(!(e is IOException||e is UnauthorizedAccessException)||attempt>=40)throw;
    Thread.Sleep(500);
   }
  }
 }
}
}
