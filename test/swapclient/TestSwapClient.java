// 端到端测试 (V65 hook 版): 模拟 Forge/NeoForge 1.20.1 (Mojang 官方名)
// 运行时, 并且用一个 Java 线程 ("Client thread") 每 10ms 调一次真实的
// gdi32!SwapBuffers —— 与真机游戏渲染线程的行为一致, 用于验证:
//   1. DLL 不新建线程、不 AttachCurrentThread, 只在渲染帧钩子内 GetEnv 复用 JNIEnv
//   2. 解析/采样/上报链路在帧驱动下正常工作
// 判定预期 (与 TestMC1201F 相同):
//   canAttack: 阶段1=0 阶段2/3/4=1 阶段5=0
//   canPlace : 阶段1=0 阶段2=1 阶段3=0 阶段4=0 阶段5=1
import javax.swing.JFrame;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.phys.EntityHitResult;

public class TestSwapClient {
    static {
        // user.dir = MCCombatStatusJni 仓库根 (smoke_test_hook.py 的 cwd)
        String dir = System.getProperty("user.dir");
        System.load(dir + "/test/swapclient/native/swapstub.dll");
    }

    private static native void swap();

    public static void main(String[] args) throws Exception {
        Minecraft mc = Minecraft.getInstance();
        LocalPlayer player = new LocalPlayer();
        mc.player = player;

        JFrame frame = new JFrame("Minecraft* 1.20.1");
        frame.setSize(400, 300);
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setVisible(true);

        // 模拟游戏渲染线程: 每 10ms 调一次真实 gdi32!SwapBuffers
        Thread render = new Thread(() -> {
            while (true) {
                swap();
                try { Thread.sleep(10); } catch (InterruptedException e) { break; }
            }
        }, "Client thread");
        render.setDaemon(true);
        render.start();

        // 物品: 放置物 (BlockItem) 与 普通物品 (Item)
        ItemStack dirtStack = new ItemStack(new BlockItem());
        ItemStack swordStack = new ItemStack(new Item());

        System.out.println("test(swapclient): 阶段1 未瞄准+空手 (canAttack=0 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.hitResult = new EntityHitResult(new LivingEntity());
        System.out.println("test(swapclient): 阶段2 瞄准生物+手持BlockItem (canAttack=1 canPlace=1) 4 秒...");
        Thread.sleep(4000);

        player.setHeldItem(swordStack);
        System.out.println("test(swapclient): 阶段3 瞄准生物+手持普通Item (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(null);
        System.out.println("test(swapclient): 阶段4 瞄准生物+空手 (canAttack=1 canPlace=0) 3 秒...");
        Thread.sleep(3000);

        player.setHeldItem(dirtStack);
        mc.hitResult = null;
        System.out.println("test(swapclient): 阶段5 未瞄准+手持BlockItem (canAttack=0 canPlace=1) 3 秒...");
        Thread.sleep(3000);

        System.out.println("test(swapclient): 结束");
        System.exit(0);
    }
}
