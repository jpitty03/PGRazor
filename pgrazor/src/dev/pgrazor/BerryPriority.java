package dev.pgrazor;

/** Ordered, unique catch-berry choices; no inventory data is persisted in the policy. */
public final class BerryPriority {
    private BerryPriority() {}
    public static boolean validId(int id) { return id==701||id==703||id==705||id==706||id==707; }
    public static int[] parse(String value) {
        if(value==null||value.length()>11)throw new IllegalArgumentException("Invalid berry priorities");
        if(value.isEmpty())return new int[0];
        String[] parts=value.split(",",-1);
        if(parts.length>3)throw new IllegalArgumentException("Too many berry priorities");
        int[] ids=new int[parts.length];
        for(int i=0;i<parts.length;i++){
            if(!parts[i].matches("[0-9]{3}"))throw new IllegalArgumentException("Invalid berry");
            ids[i]=Integer.parseInt(parts[i]);
            if(!validId(ids[i]))throw new IllegalArgumentException("Invalid berry");
            for(int j=0;j<i;j++)if(ids[j]==ids[i])throw new IllegalArgumentException("Duplicate berry");
        }
        return ids;
    }
    public static String select(String current,int id) {
        if(id==0)return "";
        if(!validId(id))throw new IllegalArgumentException("Invalid berry");
        int[] ids=parse(current);
        if(ids.length==3)return String.valueOf(id);
        for(int chosen:ids)if(chosen==id)return current;
        return current.isEmpty()?String.valueOf(id):current+","+id;
    }
    public interface BerryStock { int count(int id); }
    /** The policy shared by the overlay and the native controller: stocked rule berries
     *  first (unstocked rules never consume a slot), then unique user priorities. */
    public static int[] fallback(int packedRules,int[] priorities,BerryStock stock) {
        int[] out=new int[3];int n=0;
        for(int shift=0;shift<30&&n<3;shift+=10){
            int id=(packedRules>>shift)&1023;
            if(id>0&&stock.count(id)>0)out[n++]=id;
        }
        int rules=n;
        for(int id:priorities){
            if(n>=3)break;
            boolean dup=false;for(int j=0;j<rules;j++)if(out[j]==id)dup=true;
            if(!dup)out[n++]=id;
        }
        int[] result=new int[n];System.arraycopy(out,0,result,0,n);return result;
    }
}
