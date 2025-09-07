# LM3S6965_QEMU_Snake
qemu模拟LM3S6965实现贪吃蛇
编译
```bash
cmake --preset debug
cmake --build ./build/ -- -j16
```

运行
```bash
qemu-system-arm -kernel build/RTOSDemo.elf -machine lm3s6965evb -serial stdio
```
或使用debug运行
```bash
qemu-system-arm -kernel build/RTOSDemo.elf -machine lm3s6965evb -serial stdio -s -S
```