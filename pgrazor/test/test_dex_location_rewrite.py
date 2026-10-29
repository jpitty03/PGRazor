"""Actual dexlib2 structural roundtrip; no Android build, packaging or devices."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_control_java import java_tools
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dex_bridges

JAVA = r'''
import java.util.*;
import java.io.File;
import org.jf.dexlib2.*;
import org.jf.dexlib2.iface.*;
import org.jf.dexlib2.iface.instruction.*;
import org.jf.dexlib2.iface.reference.*;
import org.jf.dexlib2.immutable.*;
import org.jf.dexlib2.immutable.instruction.*;
import org.jf.dexlib2.immutable.reference.*;
import org.jf.dexlib2.immutable.debug.*;
import org.jf.dexlib2.writer.pool.DexPool;
public class LocationRewriteFixture {
 static void check(boolean value){if(!value)throw new AssertionError();}
 static ImmutableMethodReference ref(String owner,String name,String ret){return new ImmutableMethodReference(owner,name,Collections.emptyList(),ret);}
 static Method method(String name,MethodImplementation body){return new ImmutableMethod("Lfixture/Caller;",name,Collections.emptyList(),"D",9,Collections.emptySet(),Collections.emptySet(),body);}
 static List<Instruction> list(MethodImplementation body){List<Instruction> result=new ArrayList<>();for(Instruction i:body.getInstructions())result.add(i);return result;}
 public static void main(String[] args)throws Exception{
  List<Instruction> ins=Arrays.asList(
   new ImmutableInstruction35c(Opcode.INVOKE_VIRTUAL,1,2,0,0,0,0,ref(BridgeRewriter.LOCATION,"getLatitude","D")),
   new ImmutableInstruction11x(Opcode.MOVE_RESULT_WIDE,0),new ImmutableInstruction11x(Opcode.RETURN_WIDE,0),
   new ImmutableInstruction11x(Opcode.MOVE_EXCEPTION,0),new ImmutableInstruction11x(Opcode.THROW,0));
  ImmutableMethodImplementation body=new ImmutableMethodImplementation(3,ins,
   Collections.singletonList(new ImmutableTryBlock(0,4,Collections.singletonList(new ImmutableExceptionHandler("Ljava/lang/Throwable;",5)))),
   Collections.singletonList(new ImmutableLineNumber(3,123)));
  List<String> inventory=new ArrayList<>();int[] counts=new int[3];Method original=method("read",body);
  Method rewritten=BridgeRewriter.rewriteLocations(original,"classes.dex",inventory,counts);
  List<Instruction> out=list(rewritten.getImplementation());
  check(out.get(0).getOpcode()==Opcode.INVOKE_STATIC&&((FiveRegisterInstruction)out.get(0)).getRegisterC()==2);
  check(out.get(0).getCodeUnits()==3&&out.get(1).getOpcode()==Opcode.MOVE_RESULT_WIDE);
  MethodReference target=(MethodReference)((ReferenceInstruction)out.get(0)).getReference();
  check(target.getDefiningClass().equals(BridgeRewriter.LOCATION_VALUES)&&target.getParameterTypes().equals(Collections.singletonList(BridgeRewriter.LOCATION)));
  check(rewritten.getImplementation().getRegisterCount()==3&&rewritten.getImplementation().getTryBlocks().get(0).getCodeUnitCount()==4);
  check(rewritten.getImplementation().getTryBlocks().get(0).getExceptionHandlers().get(0).getHandlerCodeAddress()==5);
  check(rewritten.getImplementation().getDebugItems().iterator().next().getCodeAddress()==3);
  check(BridgeRewriter.preservedBody(rewritten).getImplementation()==rewritten.getImplementation());
  List<Method> methods=new ArrayList<>();methods.add(rewritten);
  for(String name:new String[]{"getLatitude","getLongitude","getSpeed","getBearing"}){
   String ret=name.equals("getSpeed")||name.equals("getBearing")?"F":"D";
   Method range=method("range"+name,new ImmutableMethodImplementation(301,Arrays.asList(
    new ImmutableInstruction3rc(Opcode.INVOKE_VIRTUAL_RANGE,300,1,ref(BridgeRewriter.LOCATION,name,ret)),
    new ImmutableInstruction11x(ret.equals("D")?Opcode.MOVE_RESULT_WIDE:Opcode.MOVE_RESULT,0),
    new ImmutableInstruction11x(Opcode.RETURN_WIDE,0)),Collections.emptyList(),Collections.emptyList()));
   Method changed=BridgeRewriter.rewriteLocations(range,"classes2.dex",inventory,counts);
   Instruction first=list(changed.getImplementation()).get(0);
   check(first.getOpcode()==Opcode.INVOKE_STATIC_RANGE&&((RegisterRangeInstruction)first).getStartRegister()==300&&((RegisterRangeInstruction)first).getRegisterCount()==1);methods.add(changed);
  }
  // The same rewrite feeds the special render-loop insertion path as in main().
  Method render=method("render",new ImmutableMethodImplementation(5,Arrays.asList(
   new ImmutableInstruction35c(Opcode.INVOKE_VIRTUAL,1,4,0,0,0,0,ref("Lcom/unity3d/player/UnityPlayerForActivityOrService;","nativeRender","Z")),
   new ImmutableInstruction11x(Opcode.MOVE_RESULT,0),
   new ImmutableInstruction35c(Opcode.INVOKE_VIRTUAL,1,4,0,0,0,0,ref(BridgeRewriter.LOCATION,"getLongitude","D")),
   new ImmutableInstruction11x(Opcode.MOVE_RESULT_WIDE,1),new ImmutableInstruction11x(Opcode.RETURN,0)),Collections.emptyList(),Collections.emptyList()));
  int[] renderCounts=new int[3];Method renderRewritten=BridgeRewriter.rewriteLocations(render,"classes.dex",new ArrayList<>(),renderCounts);
  MethodImplementation rendered=BridgeRewriter.renderLoop(renderRewritten);boolean helper=false,locationHelper=false;
  for(Instruction i:rendered.getInstructions())if(i instanceof ReferenceInstruction&&((ReferenceInstruction)i).getReference() instanceof MethodReference){
   MethodReference r=(MethodReference)((ReferenceInstruction)i).getReference();
   check(!BridgeRewriter.exactLocation(r));helper|=r.getDefiningClass().equals("Ldev/pgrazor/NativeRuntimeBootstrap;");
   locationHelper|=r.getDefiningClass().equals(BridgeRewriter.LOCATION_VALUES);
  }
  check(helper&&locationHelper&&renderCounts[0]==1);
  Method foreign=method("foreign",new ImmutableMethodImplementation(3,Collections.singletonList(
   new ImmutableInstruction35c(Opcode.INVOKE_VIRTUAL,1,2,0,0,0,0,ref("Lfixture/SubLocation;","getLatitude","D"))),Collections.emptyList(),Collections.emptyList()));
  check(BridgeRewriter.rewriteLocations(foreign,"classes.dex",inventory,counts)==foreign&&counts[1]==1);
  Method unsupported=method("unsupported",new ImmutableMethodImplementation(3,Collections.singletonList(
   new ImmutableInstruction35c(Opcode.INVOKE_SUPER,1,2,0,0,0,0,ref(BridgeRewriter.LOCATION,"getLatitude","D"))),Collections.emptyList(),Collections.emptyList()));
  check(BridgeRewriter.rewriteLocations(unsupported,"classes.dex",inventory,counts)==unsupported&&counts[2]==1);
  check(counts[0]==5&&inventory.size()==7&&inventory.get(0).contains("\t0\tLandroid/location/Location;->getLatitude()D\t"));
  ClassDef cls=new ImmutableClassDef("Lfixture/Caller;",1,"Ljava/lang/Object;",Collections.emptyList(),null,Collections.emptySet(),Collections.emptyList(),methods);
  DexFile dex=new ImmutableDexFile(Opcodes.getDefault(),Collections.singletonList(cls));BridgeRewriter.verifyLocations(dex);
  DexPool.writeTo(args[0],dex);DexFile reread=DexFileFactory.loadDexFile(new File(args[0]),Opcodes.getDefault());BridgeRewriter.verifyLocations(reread);
  Method decoded=null;for(Method m:reread.getClasses().iterator().next().getMethods())if(m.getName().equals("read"))decoded=m;
  check(decoded!=null&&decoded.getImplementation().getTryBlocks().get(0).getExceptionHandlers().get(0).getHandlerCodeAddress()==5);
  check(decoded.getImplementation().getDebugItems().iterator().next().getCodeAddress()==3);
  System.out.println("PASS actual dexlib Location rewrite: normal/range, registers, results, handlers/debug, preserved body, inventory and DEX roundtrip");
 }
}
'''


class DexLocationRewriteTests(unittest.TestCase):
    def test_actual_dexlib_structural_roundtrip(self):
        compiler, runtime = java_tools()
        jars = dex_bridges.dependencies(download=False)
        with tempfile.TemporaryDirectory(prefix='pgrazor-location-dex-') as directory:
            work = Path(directory)
            source = work / 'LocationRewriteFixture.java'
            source.write_text(JAVA, encoding='utf-8')
            classpath = os.pathsep.join(map(str, jars))
            result = subprocess.run([str(compiler), '-cp', classpath, '-d', str(work),
                                     str(dex_bridges.TOOLS / 'BridgeRewriter.java'), str(source)],
                                    capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), '-ea', '-cp', classpath + os.pathsep + str(work),
                                     'LocationRewriteFixture', str(work / 'fixture.dex')],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS actual dexlib Location rewrite', result.stdout)


if __name__ == '__main__':
    unittest.main()
