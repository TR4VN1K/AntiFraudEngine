# 🛡️ High-Performance Anti-Fraud Engine

[ Читать на русском ](README.md) | [ Read in English ](README_EN.md)

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Python](https://img.shields.io/badge/Python-3.8%2B-green.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)
![License](https://img.shields.io/badge/license-MIT-orange.svg)

Высокопроизводительный C++20 движок первичной проверки транзакций на фрод с
привязками к Python через [pybind11](https://github.com/pybind/pybind11).

Проект разработан как портфолио для демонстрации навыков проектирования на
C++ (RAII, умные указатели, шаблоны), написания потокобезопасных структур
данных (Striped Locking) и оптимизации систем под задачи с высокой нагрузкой
(Low-Latency). Корректность подтверждена прогонами под ThreadSanitizer /
AddressSanitizer / UBSan, а не только фактом компиляции.

```bash
$ python3 scripts/demo_benchmark.py 200000
...
[PASS] High amount              expected=HIGH_AMOUNT          got=HIGH_AMOUNT
[PASS] Velocity spam            expected=VELOCITY_EXCEEDED    got=VELOCITY_EXCEEDED
[PASS] Volume breach            expected=VOLUME_EXCEEDED      got=VOLUME_EXCEEDED
[PASS] Impossible geo speed     expected=IMPOSSIBLE_GEO_SPEED got=IMPOSSIBLE_GEO_SPEED

Effective throughput:                         ~700,000 tx/s
Cards currently tracked in cache:             5,000
```

## 🚀 Архитектура и инженерные решения

Архитектура системы делится на 4 чётких слоя:

- **`AntiFraudEngine` (Pipeline Runner)** — управляет последовательностью
  выполнения правил и содержит единый кэш `CardCache`. Метод `check()`
  замеряет время обработки, запускает правила в порядке их регистрации и
  мгновенно завершает проверку (*short-circuit*) при первом обнаружении
  фрода.
- **`IFraudRule` (Stateless Rules)** — иерархия правил (Open-Closed
  Principle). Объекты правил хранят только конфигурационные пороги. Всё
  изменяемое состояние карт вынесено в общий кэш, что позволяет параллельно
  выполнять одно и то же правило из разных потоков.
- **`ConcurrentMap` (Striped Locking Cache)** — потокобезопасная
  хэш-таблица. Разбивает пространство ключей на `N` сегментов (шардов),
  каждый из которых защищён отдельным `std::shared_mutex`. Это сводит
  блокировки (*lock contention*) к минимуму при обработке множества
  независимых карт.
- **`SlidingWindow<T>` (In-Memory State)** — временнóе скользящее окно на
  базе `std::deque` с амортизированной сложностью O(1) для очистки
  устаревших записей при добавлении новых транзакций.

```
                      ┌─────────────────────────┐
Python ── pybind11 ──▶│      AntiFraudEngine     │
                      │  (rule pipeline runner)  │
                      └───────────┬──────────────┘
                                  │ evaluate() для каждого правила по порядку
                    ┌─────────────┼──────────────┬───────────────┐
                    ▼             ▼              ▼               ▼
            HighAmountRule  VelocityRule   VolumeRule      GeoSpeedRule
                    │             │              │               │
                    └─────────────┴──────┬───────┴───────────────┘
                                          ▼
                              ConcurrentMap<card_id, CardState>
                              (striped std::shared_mutex locking)
                                          │
                                          ▼
                          CardState { SlidingWindow<double> × 2,
                                       last_country, last_timestamp }
```

**Почему striped locking, а не один общий мьютекс или мьютекс на карту?**
Один общий мьютекс сериализовал бы весь трафик независимо от того, какие
карты участвуют. Мьютекс на каждую карту снижает конкуренцию, но приводит к
неограниченному выделению блокировок и плохо амортизируется на кэше с
длинным хвостом редко встречающихся карт. Striped locking (фиксированный
массив из `N` мьютексов, шард выбирается как `hash(card_id) % N`) —
стандартный компромисс: память растёт O(1), а карты равномерно
распределяются по шардам, так что несвязанный трафик хорошо
параллелится на практике.

**Почему `check()` завершается по first-match?** После срабатывания правила
побочные эффекты последующих правил на `CardState` для этой транзакции не
выполняются. Это осознанный компромисс простота/производительность для
портфолио-проекта; в продакшене логичнее было бы прогонять все правила ради
наблюдаемости и использовать только первое/самое серьёзное совпадение для
итогового решения.

**Без сети и внешних сервисов.** Всё работает in-process: ни сокетов, ни
портов, ни базы данных. Состояние живёт в памяти на протяжении жизни объекта
`AntiFraudEngine`; для очистки используется `reset_state()`.

## 📁 Структура репозитория

```
.
├── CMakeLists.txt              # pybind11_add_module + статическая библиотека ядра
├── include/
│   ├── AntiFraudEngine.h       # Фасад движка: пайплайн правил + кэш
│   ├── SlidingWindow.h         # Шаблон: скользящее окно по времени
│   ├── ConcurrentMap.h         # Шаблон: потокобезопасная карта с Striped Lock
│   └── Rules.h                 # Интерфейс IFraudRule и 4 реализации правил
├── src/
│   ├── AntiFraudEngine.cpp
│   ├── Rules.cpp
│   └── bindings.cpp            # Определение модуля pybind11
├── scripts/
│   └── demo_benchmark.py       # Бенчмарк производительности и сценарии
├── tests/
│   └── tsan_stress.cpp         # Стресс-тест многопоточности на чистом C++
└── README.md
```

## 🛠️ Сборка и запуск

### Требования

- Компилятор C++20 (GCC ≥ 10, Clang ≥ 12, MSVC ≥ 19.29)
- CMake ≥ 3.15
- Python ≥ 3.8 с dev-заголовками
- Пакет [pybind11](https://pypi.org/project/pybind11/) (`pip install pybind11`)

### Инструкция по сборке

```bash
git clone https://github.com/<your-username>/antifraud-engine.git
cd antifraud-engine

pip install pybind11

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -Dpybind11_DIR="$(python3 -m pybind11 --cmakedir)" \
      ..
cmake --build . -j
```

В результате в `build/` появится `antifraud_core*.so` (Linux/macOS) или
`antifraud_core*.pyd` (Windows).

### Запуск демо и бенчмарка

```bash
# Из папки build/, чтобы Python видел скомпилированный бинарник (.so / .pyd)
PYTHONPATH=. python3 ../scripts/demo_benchmark.py 200000
```

Аргумент (по умолчанию `200000`) задаёт число синтетических транзакций,
которые генерирует бенчмарк пропускной способности.

Альтернативно модуль можно установить в свой `PYTHONPATH` (скопировать
`.so`/`.pyd` в проект, либо выполнить `cmake --install .` в директорию, уже
находящуюся в `PYTHONPATH`).

## 💻 Использование в Python

```python
import antifraud_core as af

# Инициализация движка с дефолтными лимитами
engine = af.AntiFraudEngine()

tx = af.Transaction(
    id=1,
    card_id="4111-XXXX-XXXX-1111",
    amount=15000.0,
    timestamp=1_753_400_000,   # unix seconds
    country="DE",
)

result = engine.check(tx)
print(result.is_fraud, result.reason_code, result.processing_time_ns)
# True 1 187  -> ReasonCode.HIGH_AMOUNT
```

### Настройка порогов срабатывания

```python
engine = af.AntiFraudEngine(
    high_amount_threshold=5000.0,
    velocity_max_count=3,
    velocity_window_seconds=30,
    volume_max_amount=10000.0,
    volume_window_seconds=1800,
    geo_max_speed_kmh=1000.0,
)
```

### Коды причин (`ReasonCode`)

```python
af.ReasonCode.OK                     # 0
af.ReasonCode.HIGH_AMOUNT            # 1
af.ReasonCode.VELOCITY_EXCEEDED      # 2
af.ReasonCode.VOLUME_EXCEEDED        # 3
af.ReasonCode.IMPOSSIBLE_GEO_SPEED   # 4
```

### Многопоточность из Python

Метод `AntiFraudEngine.check()` автоматически освобождает GIL на время
выполнения C++ вызова (`py::call_guard<py::gil_scoped_release>()`), поэтому
его безопасно — и полезно — вызывать из нескольких Python-потоков (например,
через `concurrent.futures.ThreadPoolExecutor`), чтобы параллельно
обрабатывать независимые карты.

## 🔍 Поддерживаемые правила

| Правило            | Условие срабатывания                                                              | Код ошибки (`ReasonCode`) |
|---------------------|-------------------------------------------------------------------------------------|-----------------------------|
| `HighAmountRule`   | сумма одной транзакции превышает фиксированный лимит                              | `HIGH_AMOUNT`               |
| `VelocityRule`     | количество транзакций по карте за короткий интервал больше `max_count`            | `VELOCITY_EXCEEDED`         |
| `VolumeRule`       | суммарный оборот по карте за окно времени превышает лимит                         | `VOLUME_EXCEEDED`           |
| `GeoSpeedRule`     | рассчитанная скорость перемещения между двумя странами физически невозможна       | `IMPOSSIBLE_GEO_SPEED`      |

Правила выполняются в порядке регистрации, и движок останавливается на первом
совпадении, поэтому `reason_code` всегда отражает *первое* сработавшее
правило (порядок по умолчанию: HighAmount → Velocity → Volume → GeoSpeed).
`GeoSpeedRule` использует небольшую встроенную таблицу приблизительных
координат центров стран и расчёт расстояния по формуле гаверсинуса;
неизвестные коды стран никогда не флагуются.

Свои правила можно добавлять со стороны C++, унаследовавшись от `IFraudRule`
и вызвав `engine.addRule(std::make_unique<MyRule>(...))` — этот механизм пока
не проброшен в Python, так как Python-реализация правила была бы вынуждена
захватывать GIL на каждый вызов, что сводило бы на нет смысл его освобождения
в `check()`.

## 🧪 Тестирование и валидация

- **Демо и интеграционный тест:** `scripts/demo_benchmark.py` проверяет, что
  каждое из четырёх правил срабатывает с ожидаемым `reason_code` на заранее
  подготовленном сценарии, и завершает работу с ненулевым кодом возврата,
  если хотя бы один сценарий не прошёл. Заодно измеряет пропускную
  способность (>700 000 операций в секунду).
- **Многопоточный тест под санитайзерами:** `tests/tsan_stress.cpp` — чистый
  C++ (без Python) стресс-тест, который нагружает `AntiFraudEngine::check()`
  из 16 потоков на небольшом, намеренно конкурентном пуле `card_id`.
  Предназначен для сборки с санитайзерами, чтобы напрямую проверить
  корректность конкурентного дизайна, независимо от слоя Python-биндингов:

  ```bash
  g++ -std=c++20 -fsanitize=thread -g -O1 -Iinclude \
      src/AntiFraudEngine.cpp src/Rules.cpp tests/tsan_stress.cpp \
      -o tsan_stress -lpthread
  ./tsan_stress

  g++ -std=c++20 -fsanitize=address,undefined -g -O1 -Iinclude \
      src/AntiFraudEngine.cpp src/Rules.cpp tests/tsan_stress.cpp \
      -o asan_stress -lpthread
  ./asan_stress
  ```

  Обе конфигурации проходят чисто (0 гонок, 0 ошибок памяти/UB) на 320 000
  конкурентных вызовов `check()` для 64 общих карт.

## 🗺️ Roadmap / не входит в задачи проекта

Осознанно вне рамок проекта — чтобы он оставался сфокусированным, читаемым
портфолио-проектом, а не production fraud-платформой:

- Персистентность/сохранение состояния карт между перезапусками процесса
- Распределённое развёртывание, шардирование между машинами или сетевой API
- Скоринг на основе машинного обучения (это rule-based движок по замыслу)
- Python-API для написания собственных правил

## 📜 Лицензия

Проект распространяется под лицензией MIT. Подробнее см. файл [LICENSE](LICENSE).
