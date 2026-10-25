package dev.pgrazor;

public final class ShinyScannerControllerTest {
    private static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    public static void main(String[] args){
        double[] position={42.65,23.34};
        ShinyScannerController scanner=new ShinyScannerController(()->position[0],()->position[1],s->{});
        check(!scanner.hasResult(),"No initial sighting");
        check(scanner.found("14219003938633576998",597,61,42.65,23.34),"Valid sighting accepted");
        check(scanner.hasResult()&&scanner.species()==597&&scanner.cp()==61,"Result retained");
        check(!scanner.found("bad",597,61,Double.NaN,23.34),"Invalid coordinates rejected");
        check(scanner.cp()==61,"Rejected sighting leaves valid result intact");
        position[0]=43.65;check(!scanner.hasResult(),"Distant result hidden");
        position[0]=42.65;android.os.SystemClock.now+=ShinyScannerController.RESULT_TTL_MS+1;
        check(!scanner.hasResult(),"Expired result hidden");
        scanner.found("14219003938633576998",597,61,42.65,23.34);scanner.clearResult();
        check(!scanner.hasResult(),"Disable clears result");
        System.out.println("PASS: shiny result validation, proximity, expiry and clearing");
    }
}
