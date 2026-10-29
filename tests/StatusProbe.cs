using System;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
namespace PyroScreenTests {
class StatusProbe {
 static byte[] Read(Stream s,int n){byte[] b=new byte[n];int offset=0;while(offset<n){var pending=s.BeginRead(b,offset,n-offset,null,null);if(!pending.AsyncWaitHandle.WaitOne(3000))throw new IOException("Status reply timed out.");int got=s.EndRead(pending);pending.AsyncWaitHandle.Close();if(got==0)throw new IOException("Status reply incomplete.");offset+=got;}return b;}
 [STAThread]public static int Main(string[] args){if(args.Length!=1)return 1;try{
  using(var pipe=new NamedPipeClientStream(".","PyroScreen-"+WindowsIdentity.GetCurrent().User.Value,PipeDirection.InOut,PipeOptions.Asynchronous)){
   pipe.Connect(3000);byte[] command=Encoding.ASCII.GetBytes("INFO");pipe.Write(command,0,4);
   int length=BitConverter.ToInt32(Read(pipe,4),0);if(length<2||length>32768)throw new IOException("Status size invalid.");
   File.WriteAllText(args[0],Encoding.UTF8.GetString(Read(pipe,length)),new UTF8Encoding(false));
  }return 0;
 }catch(Exception e){File.WriteAllText(args[0]+".error",e.Message);return 1;}}
}
}
