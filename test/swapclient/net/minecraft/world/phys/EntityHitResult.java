// 测试用假客户端 (Forge/NeoForge 1.20.1 运行时, Mojang 官方名)
package net.minecraft.world.phys;

import net.minecraft.world.entity.Entity;

public class EntityHitResult extends HitResult {
    private final Entity entity;

    public EntityHitResult(Entity entity) {
        this.entity = entity;
    }

    public Entity getEntity() {
        return this.entity;
    }

    public Type getType() {
        return Type.ENTITY;
    }
}
