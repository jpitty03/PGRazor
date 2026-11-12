package dev.pgrazor;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;

/** Private-process coordinator: no game session is swapped while Unity is alive. */
final class AccountSessions {
    static boolean recover(Context context) throws IOException {return run(context,0);}
    static void switchPending(Context context) throws IOException {run(context,1);}
    static void cancelSwitch(Context context) throws IOException {run(context,2);}
    static void deleteProfile(Context context,AccountProfileStore profiles,String id)throws IOException{
        File lockFile=new File(context.getNoBackupFilesDir(),"pgrazor-account-switch.lock");
        try(RandomAccessFile lock=new RandomAccessFile(lockFile,"rw");FileLock held=acquire(lock)){
            profiles.requireRemovable(id);
            if(new File(context.getNoBackupFilesDir(),"pgrazor-account-sessions-v1/switch-journal.bin").exists())throw new IOException("Finish switching before deleting an account");
            new AccountSessionVault(context).delete(id);
            AccountProfileData.delete(profiles,context.getFilesDir(),new File(context.getApplicationInfo().dataDir,"shared_prefs"),id);
            profiles.remove(id);
        }
    }
    private static boolean run(Context context,int action) throws IOException {
        File lockFile=new File(context.getNoBackupFilesDir(),"pgrazor-account-switch.lock");
        try(RandomAccessFile lock=new RandomAccessFile(lockFile,"rw");FileLock held=acquire(lock)){
            boolean journal=new File(context.getNoBackupFilesDir(),"pgrazor-account-sessions-v1/switch-journal.bin").exists();
            // Patched APK: a restart with no journal has nothing to finish. PGRazor Root has no
            // separate restart process, so a switch recorded before the cold restart is applied here,
            // in this fresh pre-Unity process, exactly where the restart helper would have applied it.
            if(action==0&&!journal&&!PgAssets.rooted())return true;
            if(action==0&&!journal&&new AccountProfileStore(context.getFilesDir()).pendingId().isEmpty())return true;
            AccountSessionVault vault=new AccountSessionVault(context);
            AccountSessionTransaction transaction=new AccountSessionTransaction(context.getFilesDir(),new File(context.getApplicationInfo().dataDir,"shared_prefs"),context.getPackageName(),new AccountSessionTransaction.Vault(){
                public byte[] read(String id)throws IOException{return vault.read(id);}
                public void save(String id,byte[] bytes)throws IOException{vault.save(id,bytes);}
                public void delete(String id)throws IOException{vault.delete(id);}
            });
            if(action==1)transaction.switchPending();else if(action==2)transaction.cancel();else {
                try{
                    // A leftover journal is a crash mid-swap to replay. Without one on PGRazor Root,
                    // switchPending() applies a pending switch (and is a no-op when none is queued).
                    if(journal)transaction.recover();else transaction.switchPending();
                }
                catch(IOException damagedJournal){
                    if(!journal&&!new File(context.getNoBackupFilesDir(),"pgrazor-account-sessions-v1/switch-journal.bin").exists())throw damagedJournal;
                    // Keep saved profiles and encrypted evidence. A fresh login is safer
                    // than launching with a partially applied session or a crash loop.
                    transaction.resetForSignIn();vault.quarantineJournal();return false;
                }
            }
            return true;
        }
    }
    private static FileLock acquire(RandomAccessFile file) throws IOException {
        long deadline=android.os.SystemClock.elapsedRealtime()+1500;
        do{
            FileLock lock=file.getChannel().tryLock();if(lock!=null)return lock;
            try{Thread.sleep(25);}catch(InterruptedException interrupted){Thread.currentThread().interrupt();throw new IOException("Account switch interrupted");}
        }while(android.os.SystemClock.elapsedRealtime()<deadline);
        throw new IOException("Another account switch is finishing; reopen the game");
    }
}
