#include <afina/coroutine/Engine.h>

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

namespace Afina {
namespace Coroutine {

void Engine::Store(context &ctx) {
    char a;        // узнаем где мы
    char* top = &a;
    std::ptrdiff_t len =  StackBottom - top; // размер буфера для локальных переменных
    delete [](std::get<0>(ctx.Stack)); // предотвращаю утечки
    std::get<0>(ctx.Stack) = new char[len]; // в поле тупла который отвечает за буфер создаем буфер
    std::get<1>(ctx.Stack)  = len;
    memcpy(std::get<0>(ctx.Stack), top, len); // копируем наш буфер в кучу

}

void Engine::Restore(context &ctx) {
    volatile char a;             // узнаем где мы
    char* top = (char*)&a;
    if((char*)&a > StackBottom - std::get<1>(ctx.Stack)){
        uint64_t filler_iv =0; //заполнение быстрее чем прсото с рекурсией
        Restore(ctx); // рекурсия чтобы остуупить в безопасную зону
        filler_iv ++; // чтобы обмануть компилятор чтобы он не оптимизировал хваостовую рекурсию (ив)
    }
    char* top_new = StackBottom - std::get<1>(ctx.Stack);
    memcpy(top_new,std::get<0>(ctx.Stack), std::get<1>(ctx.Stack));
    longjmp(ctx.Environment,1); // востанавливаем регистры

}

void Engine::yield() {
    if (alive){
        context * bob = alive; // вытаскиваем из очереди и запуска
        alive -> prev = nullptr;
        alive = alive -> next;
        sched(bob);
    }

}

void Engine::sched(void *routine_) {
    context * ctx = (context*)routine_; // получаем дсотпуп к context
    ctx->caller = cur_routine;
    if (cur_routine != nullptr){ // сохранение контекста
        cur_routine->callee = ctx;  //  делаем список
        Store(*cur_routine); // сохраняем
        if (setjmp(cur_routine -> Environment) !=0){ // если в нас перпрыгнули
            return;
        }
    }
    cur_routine = ctx;
    Restore(*ctx);
}

} // namespace Coroutine
} // namespace Afina
