#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#define HEAP_SIZE 0x1000  // 4 KB 堆
static unsigned char ucHeap[HEAP_SIZE];
static unsigned char *pucHeap = ucHeap;

/* ------------------ _sbrk ------------------ */
caddr_t _sbrk(int incr)
{
    unsigned char *prev = pucHeap;

    if ((pucHeap + incr) - ucHeap > HEAP_SIZE) {
        // 堆溢出
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    pucHeap += incr;
    return (caddr_t)prev;
}

/* ------------------ _write ------------------ */
int _write(int file, char *ptr, int len)
{
    // QEMU 串口输出
    for (int i = 0; i < len; i++)
        putchar(ptr[i]);
    return len;
}

/* ------------------ _read ------------------ */
int _read(int file, char *ptr, int len)
{
    // 简单模拟，不做真正输入
    return 0;
}

/* ------------------ _close ------------------ */
int _close(int file)
{
    return -1;
}

/* ------------------ _fstat ------------------ */
int _fstat(int file, struct stat *st)
{
    st->st_mode = S_IFCHR;
    return 0;
}

/* ------------------ _isatty ------------------ */
int _isatty(int file)
{
    return 1;
}

/* ------------------ _lseek ------------------ */
int _lseek(int file, int ptr, int dir)
{
    return 0;
}

/* ------------------ _kill ------------------ */
int _kill(int pid, int sig)
{
    errno = EINVAL;
    return -1;
}

/* ------------------ _getpid ------------------ */
int _getpid(void)
{
    return 1;
}

/* ------------------ _exit ------------------ */
void _exit(int status)
{
    while (1); // 停在这里
}
