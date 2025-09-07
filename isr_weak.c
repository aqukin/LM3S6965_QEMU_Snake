#include <stdint.h>

/*
 * 这是一个默认的中断处理函数。
 * 如果外设中断被使能，但没有提供具体的中断服务程序（ISR），
 * 程序将跳转到这里并进入死循环。
 * 这有助于在调试时快速定位问题。
 */
static void Default_Handler(void)
{
    while(1);
}

/*
 * 使用 GCC 的 weak 和 alias 属性。
 * 这会为下面列出的中断处理函数创建一个“弱”符号。
 * 如果用户在工程的其他地方定义了同名的函数（强符号），链接器将使用用户的版本。
 * 如果没有提供任何实现，链接器将使用这里的别名，指向 Default_Handler，从而避免链接错误。
 */
void Timer0IntHandler(void) __attribute__ ((weak, alias("Default_Handler")));
void vT2InterruptHandler(void) __attribute__ ((weak, alias("Default_Handler")));
void vT3InterruptHandler(void) __attribute__ ((weak, alias("Default_Handler")));

/*
 * startup.c 中的 IntDefaultHandler 也是一个无限循环。
 * 为了保持一致性，并为其他可能在未来被具体命名的中断提供一个统一的默认处理入口，
 * 我们也为它提供一个弱定义。
 */
void IntDefaultHandler(void) __attribute__ ((weak, alias("Default_Handler")));