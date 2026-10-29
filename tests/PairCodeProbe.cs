using System;
using System.IO;
using System.Windows.Automation;
namespace PyroScreenTests {
class PairCodeProbe{
 [STAThread]public static int Main(string[] args){
  if(args.Length!=1)return 1;
  try{
   var code=AutomationElement.RootElement.FindFirst(TreeScope.Descendants,new PropertyCondition(AutomationElement.NameProperty,"客户端配对码"));
   if(code==null)throw new Exception("Client pairing field unavailable.");
   string value=((ValuePattern)code.GetCurrentPattern(ValuePattern.Pattern)).Current.Value;
   File.WriteAllText(args[0],value.Replace(" - ",""),System.Text.Encoding.UTF8);return 0;
  }catch(Exception e){File.WriteAllText(args[0]+".error",e.Message);return 1;}
 }
}
}
