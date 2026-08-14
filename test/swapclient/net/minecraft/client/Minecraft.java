// 测试用假客户端 (Forge/NeoForge 1.20.1 运行时, Mojang 官方名)
package net.minecraft.client;

import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.phys.HitResult;

public class Minecraft {
    private static final Minecraft instance = new Minecraft();

    public static Minecraft getInstance() {
        return instance;
    }

    public LocalPlayer player;
    public HitResult hitResult;
}
