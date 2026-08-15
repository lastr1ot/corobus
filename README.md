Corobus - Coroutine Message Bus

Шина сообщений для кооперативной многозадачности с поддержкой корутин.

Описание:

Corobus реализует паттерн "Message Bus" для коммуникации между корутинами с автоматической блокировкой при переполнении/опустошении каналов.

Технологии:

    Язык: C (C99)
    Корутины: libcoro (пользовательские корутины)
    Модель: Cooperative multitasking

Возможности:

    Создание каналов с ограниченной емкостью
    Блокирующие send/recv (с yield/wakeup)
    Неблокирующие try_send/try_recv
    Векторные операции (batch send/recv)
    Broadcast рассылка
    Автоматическое управление корутинами

Быстрый старт

Сборка:

gcc -Wall -Wextra -Werror -o test test.c corobus.c libcoro.c

Запуск тестов:

./test

API:

    Создание шины:
    
    struct corobus *bus = corobus_new();
    
    Создание канала:
    
    int channel = corobus_channel_new(bus, capacity);
    
    Отправка/получение:
    
    corobus_send(bus, channel, message);  // блокирует если полно
    corobus_recv(bus, channel);            // блокирует если пусто
    corobus_try_send(bus, channel, msg);   // не блокирует
    
Архитектура:

    Coroutine Scheduler: Интеграция с libcoro
    Channel Queues: Циркулярные буферы для каналов
    Wait Lists: Списки заблокированных корутин
    Yield/Wakeup: Механизм приостановки/возобновления

Особенности:

    Кооперативная многозадачность (не потоки!)
    Отсутствие мьютексов (всё в одном потоке)
    Эффективное переключение контекста
    Поддержка векторных операций

Тесты включают:

    Тесты блокировок
    Тесты broadcast
    Векторные операции
    Стресс-тесты с множеством корутин
