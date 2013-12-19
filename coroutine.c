#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

/**
 *
 */
struct coroutine;

struct schedule {
    char stack[STACK_SIZE]; //正在运行的协程的stack
    ucontext_t main;        //调度内的上下文
    int nco;                //当前coroutine数
    int cap;                //最大coroutine数
    int running;            //正在运行的协程id
    struct coroutine **co;  //协程数组
};

struct coroutine {
    coroutine_func func;    //功能函数
    void *ud;               //功能函数的参数
    ucontext_t ctx;         //context
    struct schedule * sch;  //schedule指针
    ptrdiff_t cap;          //stack指向的空间大小
    ptrdiff_t size;         //stack里实际的堆栈大小
    int status;             //此协程状态
    char *stack;            //保存了协程运行的堆栈信息
};

/**
 * 新建协程，状态设置为ready
 */
struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
    struct coroutine * co = malloc(sizeof(*co));
    co->func = func;
    co->ud = ud;
    co->sch = S;
    co->cap = 0;
    co->size = 0;
    co->status = COROUTINE_READY;
    co->stack = NULL;
    return co;
}

void
_co_delete(struct coroutine *co) {
    free(co->stack);
    free(co);
}

/**
 * 创建schedule
 */
struct schedule * 
coroutine_open(void) {
    struct schedule *S = malloc(sizeof(*S));
    S->nco = 0;
    S->cap = DEFAULT_COROUTINE;
    S->running = -1;
    S->co = malloc(sizeof(struct coroutine *) * S->cap);
    memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
    return S;
}

/**
 * 关闭schedule
 */
void 
coroutine_close(struct schedule *S) {
    int i;
    for (i=0;i<S->cap;i++) {
        struct coroutine * co = S->co[i];
        if (co) {
            _co_delete(co);
        }
    }
    free(S->co);
    S->co = NULL;
    free(S);
}

/**
 * 在schedule内创建coroutine
 * 返回coroutine在数组内的编号
 */
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
    struct coroutine *co = _co_new(S, func , ud);
    //超过最大coroutine数，扩充为原来2倍
    //新建的coroutine就放在新空间的第一个
    if (S->nco >= S->cap) {
        int id = S->cap;
        S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
        memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
        S->co[S->cap] = co;
        S->cap *= 2;
        ++S->nco;
        return id;
    } else {
        //按顺序在数组内找一个空的位置
        int i;
        for (i=0;i<S->cap;i++) {
            int id = (i+S->nco) % S->cap;
            if (S->co[id] == NULL) {
                S->co[id] = co;
                ++S->nco;
                return id;
            }
        }
    }
    assert(0);
    return -1;
}

static void
mainfunc(uint32_t low32, uint32_t hi32) {
    uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
    struct schedule *S = (struct schedule *)ptr;
    int id = S->running;
    struct coroutine *C = S->co[id];
    C->func(S,C->ud);
    _co_delete(C);
    S->co[id] = NULL;
    --S->nco;
    S->running = -1;
}

/**
 * 运行schedule内编号为id的协程
 */
void 
coroutine_resume(struct schedule * S, int id) {
    assert(S->running == -1);
    assert(id >=0 && id < S->cap);
    struct coroutine *C = S->co[id];
    if (C == NULL)
        return;
    int status = C->status;
    switch(status) {
    case COROUTINE_READY:
        //切换到协程，保存当前contenxt到main
        getcontext(&C->ctx);
        C->ctx.uc_stack.ss_sp = S->stack;
        C->ctx.uc_stack.ss_size = STACK_SIZE;
        C->ctx.uc_link = &S->main;
        S->running = id;
        C->status = COROUTINE_RUNNING;
        uintptr_t ptr = (uintptr_t)S;
        makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
        swapcontext(&S->main, &C->ctx);
        break;
    case COROUTINE_SUSPEND:
        //把协程内保存的stack信息放入S->stack
        //注意，是放入S->stack高地址的部分
        memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
        S->running = id;
        C->status = COROUTINE_RUNNING;
        swapcontext(&S->main, &C->ctx);
        break;
    default:
        assert(0);
    }
}

/**
 * 保存协程当前的栈道stack指向的内存中
 */
static void
_save_stack(struct coroutine *C, char *top) {
    char dummy = 0;
    assert(top - &dummy <= STACK_SIZE);
    //stack里的空间不足，扩充
    if (C->cap < top - &dummy) {
        free(C->stack);
        C->cap = top-&dummy;
        C->stack = malloc(C->cap);
    }
    //把从top到&dummy的堆栈信息都存入stack中
    C->size = top - &dummy;
    memcpy(C->stack, &dummy, C->size);
}

/**
 * 让出cpu
 */
void
coroutine_yield(struct schedule * S) {
    int id = S->running;
    assert(id >= 0);
    struct coroutine * C = S->co[id];
    assert((char *)&C > S->stack);
    _save_stack(C,S->stack + STACK_SIZE);
    C->status = COROUTINE_SUSPEND;
    S->running = -1;
    swapcontext(&C->ctx , &S->main);
}

/**
 * 返回协程状态，空则返回COROUTINE_DEAD
 */
int 
coroutine_status(struct schedule * S, int id) {
    assert(id>=0 && id < S->cap);
    if (S->co[id] == NULL) {
        return COROUTINE_DEAD;
    }
    return S->co[id]->status;
}

/**
 * 返回正在运行的协程编号
 */
int 
coroutine_running(struct schedule * S) {
    return S->running;
}

