package dev.pgrazor;

import java.util.Arrays;

/** Berry fallback policy shared by BerryOverlay and the native transport list. */
public final class TagBerryFallbackTest {
    private static int checks;
    private static void eq(int[] expected,int[] actual){
        checks++;
        if(!Arrays.equals(expected,actual))throw new AssertionError(Arrays.toString(expected)+" != "+Arrays.toString(actual));
    }
    private static BerryPriority.BerryStock stock(final int... stocked){
        return id->{for(int s:stocked)if(s==id)return 5;return 0;};
    }
    public static void main(String[] args){
        int packed=706|(708<<10)|(705<<20);
        // All three rule berries out of stock: the user's stocked fallback must survive.
        eq(new int[]{701},BerryPriority.fallback(packed,new int[]{701},stock(701)));
        // Partial stock: only the stocked rule takes a slot, user priorities fill the rest.
        eq(new int[]{708,701,703},BerryPriority.fallback(packed,new int[]{701,703},stock(708,701,703)));
        // A rule berry repeated in the priority list must not take a second slot.
        eq(new int[]{706,701},BerryPriority.fallback(706,new int[]{706,701},stock(706,701)));
        // Fully stocked rules keep their order and still fill the three slots.
        eq(new int[]{706,708,705},BerryPriority.fallback(packed,new int[]{701},stock(706,708,705)));
        eq(new int[0],BerryPriority.fallback(0,new int[0],stock()));
        // Unknown stock drops the rule and keeps the fallback.
        eq(new int[]{701},BerryPriority.fallback(706,new int[]{701},stock(701)));
        System.out.println("PASS tag berry fallback: exhausted rules, partial stock, duplicates, full rules, empty and unknown stock ("+checks+" checks)");
    }
}
