"""Focused coordinate parsing and password-manager dialog lifecycle regressions."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


class DialogInputTests(unittest.TestCase):
    def test_inputs_and_pause_resume(self):
        javac, java = java_tools()
        sources = {
            'android/os/Bundle.java': 'package android.os; public class Bundle {}',
            'android/app/Application.java': '''package android.app; public class Application {
                public ActivityLifecycleCallbacks callback; public void registerActivityLifecycleCallbacks(ActivityLifecycleCallbacks c){callback=c;}
                public void unregisterActivityLifecycleCallbacks(ActivityLifecycleCallbacks c){callback=null;}
                public interface ActivityLifecycleCallbacks {
                    void onActivityPaused(Activity a);void onActivityResumed(Activity a);void onActivityDestroyed(Activity a);
                    void onActivityCreated(Activity a,android.os.Bundle b);void onActivityStarted(Activity a);
                    void onActivityStopped(Activity a);void onActivitySaveInstanceState(Activity a,android.os.Bundle b);
                }}''',
            'android/app/Activity.java': '''package android.app; public class Activity {
                public final Application app=new Application();public Application getApplication(){return app;}}''',
            'android/app/Dialog.java': '''package android.app; public class Dialog {
                public boolean showing=true;public interface Listener{void dismiss(Dialog d);}private Listener listener;
                public boolean isShowing(){return showing;}public void setOnDismissListener(Listener l){listener=l;}
                public void dismiss(){if(showing){showing=false;listener.dismiss(this);}}}''',
            'dev/pgrazor/DialogInputsTest.java': '''package dev.pgrazor;
                import android.app.*;
                public class DialogInputsTest {
                    static void check(boolean ok){if(!ok)throw new AssertionError();}
                    public static void main(String[] args){
                        for(String text:new String[]{"42.647372, 23.337947","42.647372,23.337947"," 42.647372 , 23.337947 ","42.647372 23.337947","  42.647372   23.337947 "}){
                            double[] p=CoordinateInput.parse(text);check(p[0]==42.647372&&p[1]==23.337947);
                        }
                        double[] p=CoordinateInput.parse("-90,-180");check(p[0]==-90&&p[1]==-180);
                        double[] found=CoordinateInput.find("-33.8568 151.2153");check(found!=null&&found[0]==-33.8568&&found[1]==151.2153);
                        for(String text:new String[]{"Central Park","1 Main Street","10 Downing Street, London","91 0","",null})check(CoordinateInput.find(text)==null);
                        for(String text:new String[]{"","1","1,2,3","1 2 3","1, 2 3","NaN,2","1,Infinity","91,0","0,-181","1,","0x1,2"}){
                            boolean rejected=false;try{CoordinateInput.parse(text);}catch(IllegalArgumentException e){rejected=true;}check(rejected);
                        }
                        Activity owner=new Activity();Dialog dialog=new Dialog();int[] calls={0,0,0};boolean[] login={true};
                        DialogLifecycle.bind(owner,dialog,()->calls[0]++,()->login[0],()->calls[1]++,()->calls[2]++);
                        Application.ActivityLifecycleCallbacks cb=owner.app.callback;
                        cb.onActivityPaused(new Activity());check(calls[1]==0);
                        cb.onActivityPaused(owner);check(dialog.showing&&calls[1]==1&&calls[0]==0);
                        cb.onActivityResumed(owner);check(calls[2]==1);
                        login[0]=false;cb.onActivityPaused(owner);check(!dialog.showing&&calls[0]==1&&owner.app.callback==null);
                        dialog=new Dialog();DialogLifecycle.bind(owner,dialog,()->calls[0]++,()->true,()->{},()->{});
                        owner.app.callback.onActivityDestroyed(owner);check(!dialog.showing&&calls[0]==2);
                        dialog=new Dialog();DialogLifecycle.bind(owner,dialog,null);owner.app.callback.onActivityPaused(owner);check(!dialog.showing);
                        System.out.println("PASS coordinate input and autofill pause/resume lifecycle");
                    }
                }''',
        }
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            paths = []
            for name, source in sources.items():
                p = work / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(source, encoding='utf-8')
                paths.append(p)
            paths += [ROOT / 'src/dev/pgrazor/CoordinateInput.java', ROOT / 'src/dev/pgrazor/DialogLifecycle.java']
            result = subprocess.run([str(javac), '-d', str(work), *map(str, paths)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(java), '-cp', str(work), 'dev.pgrazor.DialogInputsTest'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
