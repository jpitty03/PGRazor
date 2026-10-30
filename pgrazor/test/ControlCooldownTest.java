package dev.pgrazor;

public final class ControlCooldownTest {
    private static void check(boolean value){if(!value)throw new AssertionError();}
    public static void main(String[] args) throws Exception {
        java.lang.reflect.Field foreground=Control.class.getDeclaredField("foreground");
        foreground.setAccessible(true);foreground.setBoolean(null,true);
        Control.connect();
        Control.setCooldownEncounter("Niantic.Holoholo.Map.WildMapPokemon");
        Control.cooldownAction("Ball throw");Control.cooldownAction("Ball drop");Control.cooldownAction("Berry feeding");
        check(Cooldown.recorded.size()==3);
        Control.setCooldownEncounter("Niantic.Holoholo.Map.RaidMapPokemon");
        Control.cooldownAction("Ball throw");check(Cooldown.recorded.size()==3);
        Control.cooldownAction("Berry feeding");check(Cooldown.recorded.size()==4);
        Control.setCooldownEncounter("");Control.cooldownAction("Ball throw");check(Cooldown.recorded.size()==4);
        Control.setCooldownEncounter("ResearchMapPokemon");Control.cooldownAction("Ball throw");check(Cooldown.recorded.size()==4);
        Control.cooldownAction("Pokestop spin");check(Cooldown.recorded.size()==5);
        foreground.setBoolean(null,false);Control.cooldownAction("Pokestop spin");check(Cooldown.recorded.size()==5);
        System.out.println("PASS catch cooldown wild actions, raid exclusions, exit/unknown exclusion, spins, foreground gating");
    }
}
