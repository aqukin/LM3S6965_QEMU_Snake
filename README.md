# LM3S6965_QEMU_Snake
qemu模拟LM3S6965实现贪吃蛇
- 编译
```bash
cmake --preset debug
cmake --build ./build/ -- -j16
```

- 使用QEMU模拟
```bash
qemu-system-arm -kernel build/RTOSDemo.elf -machine lm3s6965evb -serial stdio
```
- 或在Debug模式下模拟
```bash
qemu-system-arm -kernel build/RTOSDemo.elf -machine lm3s6965evb -serial stdio -s -S
```


### main.c
##### 1.相关库引用
其中包含了由FreeRTOS官方Demo程序中的 _"osram128x64x4.h"_，
这可以直接在QEMU中实现OLED的相关操作
- `OSRAM128x64x4Init;`
- `OSRAM128x64x4StringDraw;`
- `OSRAM128x64x4ImageDraw;`
在此基础上实现了一个简单的像素填充函数
- `DefaultBlockDraw;` 

其他包含的库均为FreeRTOS相关资源或LM3S6965驱动。

##### 2. main tasks and data structure
- 1. Main tasks：
     - ___`xTaskCreate(vSnakeTask, "Snake", configMINIMAL_STACK_SIZE , NULL, 2, NULL);`___，
     - ___`xTaskCreate(vDrawTask, "Draw", 1024, NULL, 1, NULL);`___，
     - ___`xTaskCreate(vKeyboardTask, "Keyboard", configMINIMAL_STACK_SIZE , NULL, 3, NULL);`___

- 2. Main data structure:
    ``` c
    typedef struct {
        int x;
        int y;
    } Point;

    typedef enum {
        DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT, R
    } Direction;

    typedef struct {
        Direction dir;
    } KeyMsg;

    // --- 共享的游戏状态结构体 ---
    typedef struct {
        Point snake[MAX_SNAKE_LENGTH];
        int snakeLength;
        Point food;
        tBoolean gameOver;
    } GameState_t;

    QueueHandle_t xKeyQueue;
    SemaphoreHandle_t xGameStateMutex;
    static GameState_t s_gameState;
    static Direction s_currentDir = DIR_RIGHT;
    ```
##### 2.Workflow
main函数创建互斥锁与键盘按键状态队列并创建上述主要任务。
1. 互斥锁 `xGameStateMutex = xSemaphoreCreateMutex()`用于保证不同任务对游戏状态 `s_gameState`的改变与读取是互斥的。在所有需要更改或者读取 `s_gameState`的位置均需要使用互斥锁。
1. 在 ___Snake___ 任务中初始化 `s_gameState`
1. 在 ___Keyboard___ 任务中轮询串口键盘输入，将获取的按键发送到队列 `xKeyQueue`中
1. 在 ___Draw___ 任务中读取 `s_gameState`并绘制图像

##### 3. todo
加上链接服务器上传分数 或增加多人对战能力
或使用rust重建



