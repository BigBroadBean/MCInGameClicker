// 测试用假客户端 (Forge/NeoForge 1.20.1 运行时, Mojang 官方名) — ItemStack
package net.minecraft.world.item;

public class ItemStack {
    private Item item;

    public ItemStack(Item item) {
        this.item = item;
    }

    public Item getItem() {
        return item;
    }
}
