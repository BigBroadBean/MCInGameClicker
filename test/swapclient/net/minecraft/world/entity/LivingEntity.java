// 测试用假客户端 (Forge/NeoForge 1.20.1 运行时, Mojang 官方名)
package net.minecraft.world.entity;

import net.minecraft.world.item.ItemStack;

public class LivingEntity extends Entity {
    private ItemStack heldItem;

    public ItemStack getMainHandItem() {
        return heldItem;
    }

    // 测试用: 设置手持物品
    public void setHeldItem(ItemStack stack) { // 测试辅助, 非游戏名
        this.heldItem = stack;
    }
}
