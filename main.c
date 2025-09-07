#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* Hardware includes */
#include "hw_memmap.h"
#include "hw_types.h"
#include "sysctl.h"
#include "gpio.h"
#include "uart.h"
#include "grlib.h"
#include "osram128x64x4.h"

/* OLED setup constants */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define BLOCK_SIZE 2          // 一个蛇节占 4x4 像素
#define MAX_SNAKE_LENGTH 256

/* 按键定义：假设用 UART 字母输入控制 */
#define KEY_UP    'w'
#define KEY_DOWN  's'
#define KEY_LEFT  'a'
#define KEY_RIGHT 'd'
#define KEY_R     'r'

/* 数据结构 */
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


#define KEY_QUEUE_LENGTH 5
#define KEY_QUEUE_ITEM_SIZE sizeof(KeyMsg)

QueueHandle_t xKeyQueue;
// --- 用于保护游戏状态的互斥锁 ---
SemaphoreHandle_t xGameStateMutex;

void vRestart(void *pvParameters);
void vDrawTask(void *pvParameters); 

/* --- 将游戏状态变量放入一个全局静态结构体中 --- */
static GameState_t s_gameState;
static Direction s_currentDir = DIR_RIGHT;


/* OLED 函数指针 */
static void (*vOLEDInit)(uint32_t) = OSRAM128x64x4Init;
static void (*vOLEDClear)(void) = OSRAM128x64x4Clear;
// 假设有一个画实心矩形的函数，如果没有，需要自己实现或使用库函数
static void (*vOLEDBlockDraw)(int x, int y, int w, int h) = DefaultBlockDraw;
void ( * vOLEDStringDraw )( const char *,
                                uint32_t,
                                uint32_t,
                                unsigned char ) = OSRAM128x64x4StringDraw;

static void prvPrintString( const char * pcString );

/*-----------------------------------------------------------*/
/* 工具函数：随机生成食物 */
void spawnFood() {
    // 注意：此函数应该在获取互斥锁后调用
    s_gameState.food.x = (rand() % (SCREEN_WIDTH / BLOCK_SIZE)) * BLOCK_SIZE;
    s_gameState.food.y = (rand() % (SCREEN_HEIGHT / BLOCK_SIZE)) * BLOCK_SIZE;
}

/*-----------------------------------------------------------*/
/* 键盘控制任务 */
void vKeyboardTask(void *pvParameters) {
    KeyMsg msg;
    char c;
    while(1) {
        if (UARTCharsAvail(UART0_BASE)) {
            c = UARTCharGetNonBlocking(UART0_BASE);
            switch(c) {
                case KEY_UP:    msg.dir = DIR_UP; break;
                case KEY_DOWN:  msg.dir = DIR_DOWN; break;
                case KEY_LEFT:  msg.dir = DIR_LEFT; break;
                case KEY_RIGHT: msg.dir = DIR_RIGHT; break;
                case KEY_R:     msg.dir = R; break;
                default:        continue; // 非方向键忽略
            }
            xQueueSend(xKeyQueue, &msg, 0); // 发送方向消息
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}


/*-----------------------------------------------------------*/
/* --- 游戏绘图任务 --- */
void vDrawTask(void *pvParameters) {
    (void)pvParameters;
    GameState_t localGameState; // 创建一个本地副本以减少锁的持有时间

    vOLEDInit(3500000); // 初始化OLED

    for(;;) {
        // 获取互斥锁，拷贝共享状态到本地
        if (xSemaphoreTake(xGameStateMutex, portMAX_DELAY) == pdTRUE) {
            localGameState = s_gameState; // 结构体赋值是安全的
            xSemaphoreGive(xGameStateMutex);
        }

        vOLEDClear(); // 清屏

        if (!localGameState.gameOver) {
            // 绘制蛇
            for(int i = 0; i < localGameState.snakeLength; i++) {
                vOLEDBlockDraw(localGameState.snake[i].x, localGameState.snake[i].y, BLOCK_SIZE, BLOCK_SIZE);
            }
            // 绘制食物
            vOLEDBlockDraw(localGameState.food.x, localGameState.food.y, BLOCK_SIZE, BLOCK_SIZE);
        } else {
            // 游戏结束
            // prvPrintString("GAME OVER\n");
            vOLEDStringDraw("GAME OVER", 30, 30, 0x0F);
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // 绘图任务不需要太高的刷新率
    }
}

/*-----------------------------------------------------------*/
/* --- 游戏主逻辑任务 --- */
void vSnakeTask(void *pvParameters) {
    // 初始化游戏状态
    if (xSemaphoreTake(xGameStateMutex, portMAX_DELAY) == pdTRUE) {
        s_gameState.snakeLength = 3;    // 初始长度
        s_currentDir = DIR_RIGHT;
        s_gameState.gameOver = false;

        spawnFood();

        // 将初始Y坐标对齐到网格
        int initial_y = (SCREEN_HEIGHT / 2 / BLOCK_SIZE) * BLOCK_SIZE; // 计算一个居中的、对齐的Y坐标

        for (int i = 0; i < s_gameState.snakeLength; i++) {
            s_gameState.snake[i].x = (s_gameState.snakeLength - i) * BLOCK_SIZE;
            s_gameState.snake[i].y = initial_y; // 使用对齐后的Y坐标
        }
        xSemaphoreGive(xGameStateMutex);
    }

    KeyMsg msg;
    while(1) {
        // 1. 检查是否有新方向
        if(xQueueReceive(xKeyQueue, &msg, 0)) {
            if(!((s_currentDir == DIR_UP && msg.dir == DIR_DOWN) ||     // 防止直接掉头
                 (s_currentDir == DIR_DOWN && msg.dir == DIR_UP) ||
                 (s_currentDir == DIR_LEFT && msg.dir == DIR_RIGHT) ||
                 (s_currentDir == DIR_RIGHT && msg.dir == DIR_LEFT))) {
                s_currentDir = msg.dir;     
            }
        }

        // 2. 更新游戏状态 (在互斥锁保护下)
        if (xSemaphoreTake(xGameStateMutex, portMAX_DELAY) == pdTRUE) {
            if (!s_gameState.gameOver) {
                Point newHead = s_gameState.snake[0];
                switch(s_currentDir) {
                    case DIR_UP:    newHead.y -= BLOCK_SIZE; break;
                    case DIR_DOWN:  newHead.y += BLOCK_SIZE; break;
                    case DIR_LEFT:  newHead.x -= BLOCK_SIZE; break;
                    case DIR_RIGHT: newHead.x += BLOCK_SIZE; break;
                    default: break;     //如果接受到按键r则导致蛇头不动，从而导致身体追上蛇头，游戏结束。
                }                       //本质是个bug，但不影响游戏体验，甚至巧妙做到了“按R键重新开始游戏”的功能

                // 撞墙检测
                if(newHead.x < 0 || newHead.x >= SCREEN_WIDTH || newHead.y < 0 || newHead.y >= SCREEN_HEIGHT) {
                    s_gameState.gameOver = true;
                    // break;
                }

                // 撞自己检测
                if (!s_gameState.gameOver) {
                    for (int i = 1; i < s_gameState.snakeLength; i++) {
                        if (s_gameState.snake[i].x == newHead.x && s_gameState.snake[i].y == newHead.y) {
                            s_gameState.gameOver = true;
                            // break;
                        }
                    }
                }

                if (!s_gameState.gameOver) {
                    // 1. 检查是否吃到食物
                    if(newHead.x == s_gameState.food.x && newHead.y == s_gameState.food.y) {
                        // 如果吃到，先增加长度，再生成新食物
                        if(s_gameState.snakeLength < MAX_SNAKE_LENGTH) {
                            s_gameState.snakeLength++;
                        }
                        spawnFood();
                    }

                    // 2. 移动身体
                    // 这个循环现在可以正确地处理蛇变长的情况
                    for(int i = s_gameState.snakeLength - 1; i > 0; i--) {
                        s_gameState.snake[i] = s_gameState.snake[i-1];
                    }
                    
                    // 3. 更新蛇头
                    s_gameState.snake[0] = newHead;
                }
            }
            xSemaphoreGive(xGameStateMutex);
        }

        // 检查游戏是否结束
        if (s_gameState.gameOver) {
            break; // 退出循环
        }

        vTaskDelay(pdMS_TO_TICKS(100));     // 控制游戏速度
    }

    // 游戏结束后，等待一段时间，然后创建重启任务
    vTaskDelay(pdMS_TO_TICKS(2000));
    xTaskCreate(vRestart, "Restart", configMINIMAL_STACK_SIZE , NULL, 2, NULL);
    vTaskDelete(NULL); // 删除自身
}

void vRestart(void *pvParameters) {
    // 临时挂起绘图任务，以显示重启提示
    TaskHandle_t drawTaskHandle = xTaskGetHandle("Draw");
    if (drawTaskHandle != NULL) {
        vTaskSuspend(drawTaskHandle);
    }

    vOLEDClear();
    vOLEDStringDraw("PRESS KEY \"R\"", 5, 20, 0x0F);
    vOLEDStringDraw("TO RESTART", 5, 40, 0x0F);
    KeyMsg msg;
    while(1){
        if(xQueueReceive(xKeyQueue, &msg, 0)) {
            if(msg.dir == R)  break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 恢复绘图任务
    if (drawTaskHandle != NULL) {
        vTaskResume(drawTaskHandle);
    }
    
    // 创建新的游戏任务，它会自己初始化游戏状态
    xTaskCreate(vSnakeTask, "Snake", configMINIMAL_STACK_SIZE , NULL, 2, NULL);
    vTaskDelete(NULL); // 删除自身任务
}

/*-----------------------------------------------------------*/
/* 硬件初始化 */
void prvSetupHardware(void) {
    SysCtlClockSet(SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL | SYSCTL_OSC_MAIN | SYSCTL_XTAL_8MHZ);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    UARTEnable(UART0_BASE);
}

/*-----------------------------------------------------------*/
int main(void) {
    prvSetupHardware();

    // --- 创建互斥锁和队列 ---
    xGameStateMutex = xSemaphoreCreateMutex();
    xKeyQueue = xQueueCreate(KEY_QUEUE_LENGTH, KEY_QUEUE_ITEM_SIZE);

    if (xGameStateMutex != NULL && xKeyQueue != NULL) {
        // --- 创建任务 ---
        xTaskCreate(vSnakeTask, "Snake", configMINIMAL_STACK_SIZE , NULL, 2, NULL);
        xTaskCreate(vDrawTask, "Draw", 1024, NULL, 1, NULL); // 绘图任务优先级可以低一些
        xTaskCreate(vKeyboardTask, "Keyboard", configMINIMAL_STACK_SIZE , NULL, 3, NULL);

        vTaskStartScheduler();
    }

    while(1);
}


void vAssertCalled(const char *pcFile, uint32_t ulLine)
{
    (void)pcFile;
    (void)ulLine;
    for(;;);
}

/* Tick Hook */
void vApplicationTickHook(void)
{
}

/* Stack Overflow Hook */
void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    (void)pxTask;
    (void)pcTaskName;
    for(;;);
}

/* Idle Task 静态分配 */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];

    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* Timer Task 静态分配 */
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}


static void prvPrintString( const char * pcString )
{
    while( *pcString != 0x00 )
    {
        UARTCharPut( UART0_BASE, *pcString );
        pcString++;
    }
}