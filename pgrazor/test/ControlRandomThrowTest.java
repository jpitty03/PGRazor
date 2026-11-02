package dev.pgrazor;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
public final class ControlRandomThrowTest {
    static void check(boolean b,String why){if(!b)throw new AssertionError(why);}
    static final class Exchange implements NativeRuntimeTransport.Exchange {
        int mode=-1,calls;
        public int exchange(ByteBuffer snapshot,int length,ByteBuffer events){
            mode=snapshot.order(ByteOrder.LITTLE_ENDIAN).getInt(80+7*4);calls++;
            events.clear();events.order(ByteOrder.LITTLE_ENDIAN).putInt(0,0x45524750).putInt(4,17).putInt(8,32).putInt(12,0).putLong(16,1).putLong(24,1);return 32;
        }
        public void stop(){}
    }
    public static void main(String[] args)throws Exception{
        android.app.Application app=new android.app.Application(){public java.io.File getFilesDir(){return new java.io.File(args[0]);}};
        java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);load.setAccessible(true);load.invoke(null,app);
        if(args[1].equals("load")){check(Control.throwRandom()&&Control.throwGrade()==4&&Control.throwFixedGrade()==2,"random and fixed preference restored");return;}
        check(!Control.throwRandom()&&Control.throwGrade()==0,"old settings default to OFF");
        Control.setThrowGrade(2);Control.setThrowRandom(true);Control.setThrowCurve(true);Control.setThrowAr(true);Control.setThrowAlwaysHit(true);
        check(Control.throwGrade()==4&&Control.throwFixedGrade()==2,"wire random preserves fixed choice");
        check(Control.throwCurve()&&Control.throwAr()&&Control.throwAlwaysHit(),"random composes with other preferences");
        Exchange fake=new Exchange();NativeRuntimeTransport transport=NativeRuntimeTransport.createForTest(fake);transport.tick();
        check(fake.calls==1&&fake.mode==4,"actual native snapshot carries mode 4");
        Control.setThrowRandom(false);transport.tick();check(fake.calls==2&&fake.mode==2,"OFF restores fixed mode on wire");transport.stop();
        Control.setThrowGrade(0);check(Control.throwGrade()==0,"fixed OFF");Control.setThrowRandom(true);check(Control.throwGrade()==4,"random independent of fixed OFF");
        Control.setThrowGrade(2);Control.flushSettings(5000);
        check(Control.exportSettings().getProperty("grade").equals("2")&&Control.exportSettings().getProperty("throwRandom").equals("true"),"backup stores fixed grade and random separately");
        System.out.println("PASS random throws: persistence, OFF, composition and real binary transport");
    }
}
