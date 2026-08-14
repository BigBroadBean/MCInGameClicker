// 测试用假客户端 (Forge/NeoForge 1.20.1 运行时, Mojang 官方名)
package net.minecraft.world.phys;

public abstract class HitResult {
    public enum Type { MISS, BLOCK, ENTITY }

    public abstract Type getType();
}
