#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
#define KEY_PAUSE 'p'         // 暂停键

/* 数据结构 */
typedef struct {
    int x;
    int y;
} Point;

typedef enum {
    DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT, R, PAUSE
} Direction;

typedef enum {
    FOOD_NORMAL,    // 普通食物 - 绿色
    FOOD_GOLDEN,    // 黄金食物 - 双倍分数，更亮
    FOOD_SPEED_UP,  // 加速食物 - 临时提升速度
    FOOD_SLOW,      // 减速食物 - 临时降低速度
    FOOD_SUPER      // 超级食物 - 大量分数 + 长度增加2
} FoodType;

typedef struct {
    Point position;
    FoodType type;
    uint32_t spawnTime;    // 食物生成时间
    uint32_t lifetime;     // 食物生存时间(毫秒)
} Food;

typedef enum {
    GAME_MENU,      // 游戏菜单状态
    GAME_PLAYING,   // 游戏进行状态
    GAME_PAUSED,    // 游戏暂停状态
    GAME_OVER       // 游戏结束状态
} GameMode;

typedef struct {
    Direction dir;
} KeyMsg;

// --- 共享的游戏状态结构体 ---
typedef struct {
    Point snake[MAX_SNAKE_LENGTH];
    int snakeLength;
    Food food;                  // 更改为 Food 结构体
    tBoolean gameOver;
    uint32_t currentScore;      // 当前得分
    uint32_t highScore;         // 最高得分
    uint32_t level;             // 当前等级
    uint32_t gameTime;          // 游戏时间(秒)
    uint32_t speedBoostEndTime; // 速度提升结束时间
    uint32_t slowEffectEndTime; // 减速效果结束时间
    GameMode mode;              // 游戏模式
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
static uint32_t s_gameStartTime = 0;  // 游戏开始时间(tick计数)


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
    s_gameState.food.position.x = (rand() % (SCREEN_WIDTH / BLOCK_SIZE)) * BLOCK_SIZE;
    s_gameState.food.position.y = (rand() % (SCREEN_HEIGHT / BLOCK_SIZE)) * BLOCK_SIZE;
    
    // 根据概率生成不同类型的食物
    uint32_t randomValue = rand() % 100;
    if (randomValue < 60) {
        s_gameState.food.type = FOOD_NORMAL;
        s_gameState.food.lifetime = 0; // 普通食物不会过期
    } else if (randomValue < 75) {
        s_gameState.food.type = FOOD_GOLDEN;
        s_gameState.food.lifetime = 8000; // 8秒后消失
    } else if (randomValue < 85) {
        s_gameState.food.type = FOOD_SPEED_UP;
        s_gameState.food.lifetime = 6000; // 6秒后消失
    } else if (randomValue < 95) {
        s_gameState.food.type = FOOD_SLOW;
        s_gameState.food.lifetime = 5000; // 5秒后消失
    } else {
        s_gameState.food.type = FOOD_SUPER;
        s_gameState.food.lifetime = 10000; // 10秒后消失
    }
    
    s_gameState.food.spawnTime = xTaskGetTickCount();
}

/*-----------------------------------------------------------*/
/* 检查食物是否过期 */
tBoolean isFoodExpired() {
    if (s_gameState.food.lifetime == 0) return false; // 普通食物不会过期
    
    uint32_t currentTime = xTaskGetTickCount();
    uint32_t elapsedTime = (currentTime - s_gameState.food.spawnTime) * portTICK_PERIOD_MS;
    
    return (elapsedTime >= s_gameState.food.lifetime);
}

/*-----------------------------------------------------------*/
/* 计算得分函数 */
uint32_t calculateScore(int snakeLength, uint32_t level) {
    // 基础分数 + 长度奖励 + 等级奖励
    return 10 + (snakeLength - 3) * 5 + level * 2;
}

/*-----------------------------------------------------------*/
/* 计算等级函数 */
uint32_t calculateLevel(uint32_t score) {
    return (score / 100) + 1;  // 每100分升一级
}

/*-----------------------------------------------------------*/
/* 游戏时间更新函数 */
void updateGameTime() {
    if (s_gameStartTime > 0) {
        s_gameState.gameTime = (xTaskGetTickCount() - s_gameStartTime) / configTICK_RATE_HZ;
    }
}

/*-----------------------------------------------------------*/
/* 处理不同类型食物的效果 */
void processFoodEffect(FoodType foodType) {
    uint32_t currentTime = xTaskGetTickCount();
    
    switch(foodType) {
        case FOOD_NORMAL:
            // 普通食物：标准分数和长度增加
            s_gameState.currentScore += calculateScore(s_gameState.snakeLength, s_gameState.level);
            if(s_gameState.snakeLength < MAX_SNAKE_LENGTH) {
                s_gameState.snakeLength++;
            }
            break;
            
        case FOOD_GOLDEN:
            // 黄金食物：双倍分数
            s_gameState.currentScore += calculateScore(s_gameState.snakeLength, s_gameState.level) * 2;
            if(s_gameState.snakeLength < MAX_SNAKE_LENGTH) {
                s_gameState.snakeLength++;
            }
            break;
            
        case FOOD_SPEED_UP:
            // 加速食物：正常分数 + 5秒速度提升
            s_gameState.currentScore += calculateScore(s_gameState.snakeLength, s_gameState.level);
            s_gameState.speedBoostEndTime = currentTime + pdMS_TO_TICKS(5000);
            if(s_gameState.snakeLength < MAX_SNAKE_LENGTH) {
                s_gameState.snakeLength++;
            }
            break;
            
        case FOOD_SLOW:
            // 减速食物：正常分数 + 3秒减速效果
            s_gameState.currentScore += calculateScore(s_gameState.snakeLength, s_gameState.level);
            s_gameState.slowEffectEndTime = currentTime + pdMS_TO_TICKS(3000);
            if(s_gameState.snakeLength < MAX_SNAKE_LENGTH) {
                s_gameState.snakeLength++;
            }
            break;
            
        case FOOD_SUPER:
            // 超级食物：三倍分数 + 增加2个长度
            s_gameState.currentScore += calculateScore(s_gameState.snakeLength, s_gameState.level) * 3;
            if(s_gameState.snakeLength < MAX_SNAKE_LENGTH - 1) {
                s_gameState.snakeLength += 2;
            } else if(s_gameState.snakeLength < MAX_SNAKE_LENGTH) {
                s_gameState.snakeLength++;
            }
            break;
    }
    
    // 更新等级和最高分
    s_gameState.level = calculateLevel(s_gameState.currentScore);
    if (s_gameState.currentScore > s_gameState.highScore) {
        s_gameState.highScore = s_gameState.currentScore;
    }
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
                case KEY_PAUSE: msg.dir = PAUSE; break;
                default:        continue; // 非方向键忽略
            }
            xQueueSend(xKeyQueue, &msg, 0); // 发送方向消息
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/*-----------------------------------------------------------*/
/* 显示游戏菜单 */
void displayGameMenu() {
    vOLEDStringDraw("SNAKE GAME", 25, 10, 0x0F);
    vOLEDStringDraw("W/A/S/D: Move", 10, 25, 0x0F);
    vOLEDStringDraw("P: Pause", 35, 35, 0x0F);
    vOLEDStringDraw("R: Start/Restart", 5, 45, 0x0F);
    
    char highScoreBuffer[32];
    sprintf(highScoreBuffer, "High: %lu", s_gameState.highScore);
    vOLEDStringDraw(highScoreBuffer, 25, 55, 0x0F);
}

/*-----------------------------------------------------------*/
/* --- 游戏绘图任务 --- */
void vDrawTask(void *pvParameters) {
    (void)pvParameters;
    GameState_t localGameState; // 创建一个本地副本以减少锁的持有时间
    char scoreBuffer[32];       // 用于显示分数信息
    char timeBuffer[16];        // 用于显示时间信息

    vOLEDInit(3500000); // 初始化OLED

    for(;;) {
        // 获取互斥锁，拷贝共享状态到本地
        if (xSemaphoreTake(xGameStateMutex, portMAX_DELAY) == pdTRUE) {
            localGameState = s_gameState; // 结构体赋值是安全的
            if (localGameState.mode == GAME_PLAYING) {
                updateGameTime();  // 更新游戏时间
            }
            xSemaphoreGive(xGameStateMutex);
        }

        vOLEDClear(); // 清屏

        switch(localGameState.mode) {
            case GAME_MENU:
                displayGameMenu();
                break;
                
            case GAME_PLAYING:
                // 绘制蛇
                for(int i = 0; i < localGameState.snakeLength; i++) {
                    vOLEDBlockDraw(localGameState.snake[i].x, localGameState.snake[i].y, BLOCK_SIZE, BLOCK_SIZE);
                }
                
                // 绘制食物 - 根据类型使用不同显示方式
                switch(localGameState.food.type) {
                    case FOOD_NORMAL:
                        // 普通食物 - 单个方块
                        vOLEDBlockDraw(localGameState.food.position.x, localGameState.food.position.y, BLOCK_SIZE, BLOCK_SIZE);
                        break;
                    case FOOD_GOLDEN:
                        // 黄金食物 - 稍大一点
                        vOLEDBlockDraw(localGameState.food.position.x, localGameState.food.position.y, BLOCK_SIZE, BLOCK_SIZE);
                        vOLEDBlockDraw(localGameState.food.position.x+1, localGameState.food.position.y+1, BLOCK_SIZE-2, BLOCK_SIZE-2);
                        break;
                    case FOOD_SPEED_UP:
                        // 加速食物 - 绘制一个小箭头样式
                        vOLEDBlockDraw(localGameState.food.position.x, localGameState.food.position.y, BLOCK_SIZE, BLOCK_SIZE);
                        if (localGameState.food.position.x + BLOCK_SIZE < SCREEN_WIDTH) {
                            vOLEDBlockDraw(localGameState.food.position.x+BLOCK_SIZE, localGameState.food.position.y, BLOCK_SIZE/2, BLOCK_SIZE);
                        }
                        break;
                    case FOOD_SLOW:
                        // 减速食物 - 绘制一个点状
                        vOLEDBlockDraw(localGameState.food.position.x+BLOCK_SIZE/4, localGameState.food.position.y+BLOCK_SIZE/4, BLOCK_SIZE/2, BLOCK_SIZE/2);
                        break;
                    case FOOD_SUPER:
                        // 超级食物 - 绘制一个大的方块
                        vOLEDBlockDraw(localGameState.food.position.x, localGameState.food.position.y, BLOCK_SIZE*2, BLOCK_SIZE*2);
                        break;
                }
                
                // 显示游戏信息
                sprintf(scoreBuffer, "Score:%lu L:%lu", localGameState.currentScore, localGameState.level);
                vOLEDStringDraw(scoreBuffer, 0, 0, 0x0F);
                
                sprintf(timeBuffer, "T:%lus", localGameState.gameTime);
                vOLEDStringDraw(timeBuffer, 0, 8, 0x0F);
                break;
                
            case GAME_PAUSED:
                vOLEDStringDraw("PAUSED", 35, 25, 0x0F);
                vOLEDStringDraw("Press P to resume", 5, 35, 0x0F);
                sprintf(scoreBuffer, "Score: %lu", localGameState.currentScore);
                vOLEDStringDraw(scoreBuffer, 20, 45, 0x0F);
                break;
                
            case GAME_OVER:
                // 游戏结束显示
                vOLEDStringDraw("GAME OVER", 30, 20, 0x0F);
                sprintf(scoreBuffer, "Score: %lu", localGameState.currentScore);
                vOLEDStringDraw(scoreBuffer, 20, 30, 0x0F);
                sprintf(scoreBuffer, "High: %lu", localGameState.highScore);
                vOLEDStringDraw(scoreBuffer, 20, 40, 0x0F);
                sprintf(timeBuffer, "Time: %lus", localGameState.gameTime);
                vOLEDStringDraw(timeBuffer, 20, 50, 0x0F);
                break;
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
        s_gameState.currentScore = 0;   // 初始化分数
        s_gameState.level = 1;          // 初始等级
        s_gameState.gameTime = 0;       // 初始化游戏时间
        s_gameStartTime = xTaskGetTickCount(); // 记录游戏开始时间
        s_gameState.speedBoostEndTime = 0;     // 初始化速度提升结束时间
        s_gameState.slowEffectEndTime = 0;     // 初始化减速效果结束时间
        s_gameState.mode = GAME_MENU;          // 开始时显示菜单

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
            if (xSemaphoreTake(xGameStateMutex, portMAX_DELAY) == pdTRUE) {
                switch(s_gameState.mode) {
                    case GAME_MENU:
                        if (msg.dir == R) {
                            s_gameState.mode = GAME_PLAYING;
                            s_gameStartTime = xTaskGetTickCount();
                        }
                        break;
                        
                    case GAME_PLAYING:
                        if (msg.dir == PAUSE) {
                            s_gameState.mode = GAME_PAUSED;
                        } else if (msg.dir == R) {
                            // 重新开始游戏
                            s_gameState.mode = GAME_MENU;
                        } else if(!((s_currentDir == DIR_UP && msg.dir == DIR_DOWN) ||     // 防止直接掉头
                                   (s_currentDir == DIR_DOWN && msg.dir == DIR_UP) ||
                                   (s_currentDir == DIR_LEFT && msg.dir == DIR_RIGHT) ||
                                   (s_currentDir == DIR_RIGHT && msg.dir == DIR_LEFT))) {
                            s_currentDir = msg.dir;     
                        }
                        break;
                        
                    case GAME_PAUSED:
                        if (msg.dir == PAUSE) {
                            s_gameState.mode = GAME_PLAYING;
                        } else if (msg.dir == R) {
                            s_gameState.mode = GAME_MENU;
                        }
                        break;
                        
                    case GAME_OVER:
                        if (msg.dir == R) {
                            s_gameState.mode = GAME_MENU;
                        }
                        break;
                }
                xSemaphoreGive(xGameStateMutex);
            }
        }

        // 2. 更新游戏状态 (只在游戏进行时)
        if (xSemaphoreTake(xGameStateMutex, portMAX_DELAY) == pdTRUE) {
            if (s_gameState.mode == GAME_PLAYING && !s_gameState.gameOver) {
                // 检查食物是否过期，如果过期则重新生成
                if (isFoodExpired()) {
                    spawnFood();
                }
                
                Point newHead = s_gameState.snake[0];
                switch(s_currentDir) {
                    case DIR_UP:    newHead.y -= BLOCK_SIZE; break;
                    case DIR_DOWN:  newHead.y += BLOCK_SIZE; break;
                    case DIR_LEFT:  newHead.x -= BLOCK_SIZE; break;
                    case DIR_RIGHT: newHead.x += BLOCK_SIZE; break;
                    default: break;     //如果接受到按键r则导致蛇头不动，从而导致身体追上蛇头，游戏结束。
                }                       //本质是个bug，但不影响游戏体验，甚至巧妙做到了"按R键重新开始游戏"的功能

                // 撞墙检测
                if(newHead.x < 0 || newHead.x >= SCREEN_WIDTH || newHead.y < 0 || newHead.y >= SCREEN_HEIGHT) {
                    s_gameState.gameOver = true;
                    s_gameState.mode = GAME_OVER;
                }

                // 撞自己检测
                if (!s_gameState.gameOver) {
                    for (int i = 1; i < s_gameState.snakeLength; i++) {
                        if (s_gameState.snake[i].x == newHead.x && s_gameState.snake[i].y == newHead.y) {
                            s_gameState.gameOver = true;
                            s_gameState.mode = GAME_OVER;
                        }
                    }
                }

                if (!s_gameState.gameOver) {
                    // 1. 检查是否吃到食物
                    if(newHead.x == s_gameState.food.position.x && newHead.y == s_gameState.food.position.y) {
                        // 处理不同类型食物的效果
                        processFoodEffect(s_gameState.food.type);
                        
                        spawnFood(); // 生成新食物
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

        // 根据游戏模式决定延迟时间
        uint32_t taskDelay = 100;
        if (s_gameState.mode == GAME_PLAYING && !s_gameState.gameOver) {
            // 根据等级和特殊效果调整游戏速度
            uint32_t currentTime = xTaskGetTickCount();
            uint32_t baseDelay = 200 - (s_gameState.level - 1) * 15;
            if (baseDelay < 50) baseDelay = 50;  // 最快不超过50ms
            
            taskDelay = baseDelay;
            
            // 检查速度提升效果
            if (s_gameState.speedBoostEndTime > currentTime) {
                taskDelay = taskDelay / 2;  // 速度提升时延迟减半
            }
            
            // 检查减速效果
            if (s_gameState.slowEffectEndTime > currentTime) {
                taskDelay = taskDelay * 2;  // 减速时延迟加倍
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(taskDelay));
    }
}

void vRestart(void *pvParameters) {
    // 此函数在新设计中不再需要，因为游戏状态管理已集成到主任务中
    vTaskDelete(NULL);
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