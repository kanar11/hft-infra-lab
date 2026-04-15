# LEARNING.md — Beginner's Guide to HFT Infrastructure Lab
# Przewodnik dla początkujących po laboratorium infrastruktury HFT

---

## Who is this for? / Dla kogo to jest?

This guide is for **you** — someone who knows Linux (terminal, files, processes, networking)
but has **zero experience** with programming languages like Python or C++.

Ten przewodnik jest dla **Ciebie** — kogoś, kto zna Linuxa (terminal, pliki, procesy, sieć),
ale **nie ma doświadczenia** z językami programowania jak Python czy C++.

You already understand:
- how to navigate the filesystem (`cd`, `ls`, `cat`, `grep`)
- how processes and PIDs work
- how sockets and ports work (from the OS side)
- how to read config files and edit them

Już rozumiesz: jak poruszać się po systemie plików, jak działają procesy, sokety i porty,
jak czytać i edytować pliki konfiguracyjne.

Now you'll learn what's **inside** those programs.

Teraz dowiesz się, co jest **wewnątrz** tych programów.

---

## What is HFT? / Co to jest HFT?

**High-Frequency Trading** = buying and selling stocks using computers,
as fast as possible (microseconds = millionths of a second).

**Handel wysokiej częstotliwości** = kupowanie i sprzedawanie akcji za pomocą komputerów,
tak szybko jak to możliwe (mikrosekundy = milionowe części sekundy).

Think of it like this:
- A normal trader sees prices on a screen and clicks "Buy" → **seconds**
- An algorithm reads data from the network and sends orders → **microseconds**
- The faster you are, the better prices you get

Normalny trader widzi ceny na ekranie i klika "Kup" → **sekundy**.
Algorytm odczytuje dane z sieci i wysyła zlecenia → **mikrosekundy**.
Im szybciej, tym lepsze ceny dostajesz.

### Why does this project matter for a recruiter? / Dlaczego ten projekt ma znaczenie?

This project shows you understand:
1. **Low-latency systems** — making software as fast as possible
2. **Network protocols** — how computers talk to stock exchanges
3. **Linux performance tuning** — your strongest skill, applied to trading
4. **System architecture** — how components connect together

Ten projekt pokazuje, że rozumiesz: systemy nisko-opóźnieniowe, protokoły sieciowe,
tuning wydajności Linuxa i architekturę systemów.

---

## Programming Basics You Need / Podstawy programowania, które potrzebujesz

Before diving into the code, here are the key concepts. You don't need to memorize them —
just refer back here when you see something unfamiliar.

Zanim zagłębisz się w kod — oto kluczowe pojęcia. Nie musisz ich zapamiętywać,
po prostu wróć tutaj gdy zobaczysz coś nieznanego.

### Python Basics / Podstawy Pythona

**Python** is like a scripting language (think: Bash, but more powerful).
You already know Bash scripts — Python works similarly but with different syntax.

Python jest jak język skryptowy (pomyśl: Bash, ale potężniejszy).
Znasz już skrypty Bash — Python działa podobnie, ale z inną składnią.

| Concept / Pojęcie | Bash equivalent | Python syntax | What it means / Co to znaczy |
|---|---|---|---|
| Variable / Zmienna | `NAME="hello"` | `name = "hello"` | A named box that holds a value / Nazwane pudełko przechowujące wartość |
| Function / Funkcja | `function greet() { ... }` | `def greet():` | A reusable block of commands / Wielokrotnie używalny blok poleceń |
| If statement / Warunek | `if [ $x -gt 5 ]; then` | `if x > 5:` | Do something only when condition is true / Zrób coś tylko gdy warunek jest prawdziwy |
| Loop / Pętla | `for i in 1 2 3; do` | `for i in [1, 2, 3]:` | Repeat commands for each item / Powtórz polecenia dla każdego elementu |
| List / Lista | `arr=(a b c)` | `items = [a, b, c]` | An ordered collection of items / Uporządkowany zbiór elementów |
| Dictionary / Słownik | (no direct equivalent) | `d = {"key": "value"}` | Key-value pairs, like a config file / Pary klucz-wartość, jak plik konfiguracyjny |
| Import | `source utils.sh` | `import os` | Load code from another file / Załaduj kod z innego pliku |
| Print / Wypisz | `echo "hello"` | `print("hello")` | Show text on screen / Pokaż tekst na ekranie |
| Class / Klasa | (no equivalent) | `class Order:` | A blueprint for creating objects — see below / Szablon do tworzenia obiektów — patrz niżej |

#### What is a Class? / Co to jest klasa?

Think of a Linux **process** — it has a PID, a name, memory, state (running/sleeping).
A **class** is like a template that says: "every Order has an ID, a symbol, a price, a quantity."
Each actual order is an **instance** (object) of that class — like each process is an instance of a program.

Pomyśl o **procesie** w Linuxie — ma PID, nazwę, pamięć, stan (running/sleeping).
**Klasa** to szablon mówiący: "każde zamówienie ma ID, symbol, cenę, ilość."
Każde faktyczne zamówienie to **instancja** (obiekt) tej klasy — jak każdy proces to instancja programu.

```python
# Template / Szablon:
class Order:
    order_id: int       # like PID — unique number / jak PID — unikalny numer
    symbol: str         # stock name like "AAPL" / nazwa akcji jak "AAPL"
    price: float        # price like 150.25 / cena jak 150.25
    quantity: int       # how many shares / ile akcji

# Creating an instance / Tworzenie instancji:
my_order = Order(order_id=1, symbol="AAPL", price=150.25, quantity=100)
# Like launching a process from a program
# Jak uruchomienie procesu z programu
```

#### What is a Decorator (@)? / Co to jest dekorator (@)?

When you see `@something` above a function, it's a **decorator** — a wrapper that adds
extra behavior. Think of it like a `sudo` prefix: `sudo rm` = "run rm with extra powers".

Gdy widzisz `@coś` nad funkcją, to **dekorator** — opakowanie dodające dodatkowe zachowanie.
Pomyśl jak `sudo`: `sudo rm` = "uruchom rm z dodatkowymi uprawnieniami".

```python
@dataclass                 # "Hey Python, auto-generate __init__ for this class"
class Order:               # Hej Python, automatycznie generuj __init__ dla tej klasy
    order_id: int
    symbol: str
```

#### What is `self`? / Co to jest `self`?

In a class method, `self` means "this particular instance." It's like `$$` (current PID)
— it refers to the specific object calling the method.

W metodzie klasy `self` oznacza "ta konkretna instancja". To jak `$$` (bieżący PID)
— odnosi się do konkretnego obiektu wywołującego metodę.

```python
class OMS:
    def submit_order(self, symbol, price):
        self.orders.append(...)   # add to THIS OMS's order list
                                  # dodaj do listy zamówień TEGO systemu OMS
```

#### What are type hints (`: int`, `-> bool`)? / Co to są podpowiedzi typów?

They tell you what kind of data a variable holds. The program works without them,
but they make code easier to read — like comments.

Mówią ci jakiego rodzaju dane przechowuje zmienna. Program działa bez nich,
ale ułatwiają czytanie kodu — jak komentarze.

```python
def check_risk(self, order: Order) -> bool:
#                      ^^^^^ Order   ^^^^^^ returns True/False
#                      przyjmuje Order    zwraca True/False
```

### C++ Basics / Podstawy C++

**C++** is used here for the **fastest** parts (order book, queues) because it compiles
to machine code — much faster than Python.

C++ jest używany do **najszybszych** części (book zleceń, kolejki), ponieważ kompiluje się
do kodu maszynowego — dużo szybszego niż Python.

Think of it this way:
- **Python** = bash script (interpreted, easy to write, slower)
- **C++** = compiled binary like `/usr/bin/grep` (compiled, harder to write, very fast)

Python = skrypt bash (interpretowany, łatwy, wolniejszy).
C++ = skompilowany binarny jak `/usr/bin/grep` (kompilowany, trudniejszy, bardzo szybki).

| Concept / Pojęcie | What it means / Co to znaczy |
|---|---|
| `#include <vector>` | Like `import` — loads a library / Jak `import` — ładuje bibliotekę |
| `struct Order { int id; double price; };` | Like a class — a template for data / Jak klasa — szablon dla danych |
| `std::map<std::string, int>` | A dictionary (key→value) / Słownik (klucz→wartość) |
| `std::vector<Order>` | A resizable list/array / Lista o zmiennym rozmiarze |
| `auto` | Let the compiler figure out the type / Niech kompilator sam określi typ |
| `&` (reference) | Pass the actual data, not a copy (faster) / Przekaż same dane, nie kopię (szybciej) |
| `std::atomic<int>` | Thread-safe variable (like a lock-free counter) / Zmienna bezpieczna wątkowo |
| `constexpr` | Value known at compile time (like a constant) / Wartość znana w czasie kompilacji |
| `template<typename T>` | Generic code that works with any type / Generyczny kod działający z dowolnym typem |
| `namespace std` | Grouping of related functions (like a module) / Grupowanie powiązanych funkcji |

### What does `make build` actually do? / Co właściwie robi `make build`?

You know `make` from Linux. When you run `make build`:
1. `g++` (C++ compiler) reads `.cpp` source files
2. Compiles them to binary executables (like when you compile a kernel module)
3. The `-O2` flag means "optimize for speed" (level 2 of 3)
4. The `-std=c++17` means "use C++17 standard" (version of the language)
5. The `-pthread` enables multi-threading (like POSIX threads)

Znasz `make` z Linuxa. Gdy uruchomisz `make build`:
g++ czyta pliki źródłowe .cpp, kompiluje je do binarnych plików wykonywalnych,
`-O2` oznacza "optymalizuj pod kątem szybkości", `-std=c++17` używa standardu C++17,
`-pthread` włącza wielowątkowość.

---

## Study Order / Kolejność nauki

Start from the simplest and work up. Each module builds on the previous ones.

Zacznij od najprostszego i idź w górę. Każdy moduł bazuje na poprzednich.

### Phase 1: Configuration and Tools / Faza 1: Konfiguracja i narzędzia

These are the simplest files — they set up the project, not the trading logic.
To najprostsze pliki — konfigurują projekt, nie logikę handlową.

#### 1. `config.yaml` + `config_loader.py`

**What:** Central configuration file + Python loader
**Co:** Centralny plik konfiguracyjny + loader Pythona

**Why it matters:** Every system needs configuration. Instead of hardcoding values
(writing numbers directly in code), we put them in a YAML file.
You already know YAML — it's like the config files in `/etc/`.

**Dlaczego to ważne:** Każdy system potrzebuje konfiguracji. Zamiast wpisywać wartości
na sztywno w kodzie, umieszczamy je w pliku YAML — znasz już YAML z plików w `/etc/`.

**How to explore / Jak eksplorować:**
```bash
cat config.yaml                           # Read the config / Przeczytaj konfigurację
python3 -c "from config_loader import load_config; print(load_config())"  # Load it / Załaduj
HFT_RISK_MAX_DAILY_LOSS=50000 python3 -c "from config_loader import load_config; print(load_config()['risk'])"  # Override with env var / Nadpisz zmienną środowiskową
```

**Key concepts to learn / Kluczowe pojęcia:**
- YAML syntax (you know this from Ansible/Docker)
- Environment variable overrides (like `$PATH` overriding defaults)
- Python dictionaries = key-value pairs (like YAML/JSON)

#### 2. `Makefile`

**What:** Build automation — compiles C++, runs tests, lints code
**Co:** Automatyzacja budowania — kompiluje C++, uruchamia testy, sprawdza kod

**You already know this!** Makefiles work the same as in kernel compilation or any Linux project.

**To już znasz!** Pliki Makefile działają tak samo jak przy kompilacji jądra czy dowolnym projekcie Linux.

**How to explore / Jak eksplorować:**
```bash
make build      # Compile C++ binaries / Kompiluj binaria C++
make lint       # Check all Python for syntax errors / Sprawdź składnię Pythona
make test       # Run all 54 tests / Uruchom wszystkie 54 testy
make benchmark  # Run speed measurements / Uruchom pomiary szybkości
make simulate   # Run market data simulator / Uruchom symulator danych rynkowych
```

---

### Phase 2: Market Data (Reading) / Faza 2: Dane rynkowe (odczyt)

Before you can trade, you need to **receive and understand** market data from the exchange.

Zanim zaczniesz handlować, musisz **otrzymać i zrozumieć** dane rynkowe z giełdy.

#### 3. `itch_parser/itch_parser.py` — ITCH 5.0 Protocol

**What:** Reads binary data from NASDAQ's market data feed
**Co:** Odczytuje dane binarne z kanału danych rynkowych NASDAQ

**Real-world analogy:** Imagine the stock exchange sends you a stream of bytes over the network
(like `tcpdump` captures). ITCH is the **format** of those bytes — it tells you where the
price is, where the stock symbol is, how many shares, etc.

**Analogia:** Wyobraź sobie, że giełda wysyła ci strumień bajtów przez sieć
(jak przechwytuje `tcpdump`). ITCH to **format** tych bajtów — mówi ci gdzie jest cena,
gdzie symbol akcji, ile akcji itp.

**Key concepts / Kluczowe pojęcia:**
- `struct.unpack()` = reads raw bytes into values (like reading binary data from a file with `od` or `xxd`)
- Binary protocol = data is packed tightly into bytes, not readable text
- Message types: ADD_ORDER, DELETE_ORDER, TRADE (like packet types in `tcpdump`)

**How to explore / Jak eksplorować:**
```bash
python3 tests/test_itch.py    # See parser handle all message types / Zobacz jak parser obsługuje wszystkie typy wiadomości
```

#### 4. `multicast/` — Network Multicast

**What:** Send/receive market data over UDP multicast (one-to-many network)
**Co:** Wysyłanie/odbieranie danych rynkowych przez UDP multicast (sieć jeden-do-wielu)

**You know this!** Multicast is a Linux networking concept. One sender, many receivers
on the same multicast group (IP). Used in HFT because it's the fastest way to
distribute data to many consumers.

**To znasz!** Multicast to pojęcie z sieci Linux. Jeden nadawca, wielu odbiorców
na tej samej grupie multicast (IP). Używane w HFT bo to najszybszy sposób
dystrybucji danych do wielu konsumentów.

**How to explore / Jak eksplorować:**
```bash
# Terminal 1: start receiver / uruchom odbiornik
python3 multicast/mc_receiver.py &
# Terminal 2: send messages / wyślij wiadomości
python3 multicast/mc_sender.py
# Watch the receiver print what it got / Obserwuj co odbiornik wypisze
```

---

### Phase 3: Order Book (The Core Engine) / Faza 3: Book zleceń (silnik)

This is the **heart** of any exchange — matching buyers with sellers.

To **serce** każdej giełdy — łączenie kupujących ze sprzedającymi.

#### 5. `orderbook/orderbook.cpp` — Basic Order Book

**What:** C++ implementation of a limit order book
**Co:** Implementacja book zleceń (LOB) w C++

**Real-world analogy:** Imagine two lines of people:
- **BID** line: people wanting to BUY, sorted by highest price first
- **ASK** line: people wanting to SELL, sorted by lowest price first
- When the highest bid >= lowest ask → **MATCH** (trade happens)

**Analogia:** Wyobraź sobie dwie kolejki ludzi:
**BID**: chcący KUPIĆ, posortowani od najwyższej ceny.
**ASK**: chcący SPRZEDAĆ, posortowani od najniższej ceny.
Gdy najwyższy bid >= najniższy ask → **TRANSAKCJA**.

**Key concepts / Kluczowe pojęcia:**
- `std::map` = sorted dictionary (prices always in order)
- Price-time priority: best price first, then earliest order first
- Matching engine: the algorithm that finds and executes trades

**How to explore / Jak eksplorować:**
```bash
make build
./orderbook/orderbook       # Run basic order book / Uruchom podstawowy book zleceń
./orderbook/orderbook_v2    # Run optimized version / Uruchom zoptymalizowaną wersję
```

#### 6. `orderbook/benchmark_orderbook.cpp` + `latency_histogram.cpp`

**What:** Measures how fast the order book processes orders (millions per second)
**Co:** Mierzy jak szybko book zleceń przetwarza zlecenia (miliony na sekundę)

**Why it matters:** In HFT, speed = money. These benchmarks prove your code is fast.

**How to explore / Jak eksplorować:**
```bash
./orderbook/benchmark_orderbook    # See orders/second / Zobacz zlecenia/sekundę
./orderbook/latency_histogram      # See latency distribution / Zobacz rozkład opóźnień
```

---

### Phase 4: Sending Orders / Faza 4: Wysyłanie zleceń

Now that you can read market data and have an order book, you need to **send orders**.

Teraz gdy umiesz czytać dane rynkowe i masz book zleceń, musisz **wysyłać zlecenia**.

#### 7. `ouch-protocol/ouch_sender.py` — OUCH 4.2 Protocol

**What:** Builds binary messages to send orders TO the exchange
**Co:** Buduje binarne wiadomości do wysyłania zleceń DO giełdy

**Analogy:** ITCH is for **reading** (exchange → you). OUCH is for **writing** (you → exchange).
Think of it like: `tcpdump` (read) vs `nc` (send).

**Analogia:** ITCH służy do **czytania** (giełda → ty). OUCH służy do **pisania** (ty → giełda).
Pomyśl jak: `tcpdump` (odczyt) vs `nc` (wysyłanie).

#### 8. `fix-protocol/fix_parser.py` — FIX 4.2 Protocol

**What:** Parses and builds FIX messages (text-based trading protocol)
**Co:** Parsuje i buduje wiadomości FIX (tekstowy protokół handlowy)

**Analogy:** FIX is like HTTP for trading — **text-based**, human-readable, but slower than ITCH/OUCH.
Messages look like: `8=FIX.4.2|35=D|49=SENDER|56=TARGET|55=AAPL|...`
The `|` separates fields, each field has a tag number (like HTTP headers).

**Analogia:** FIX jest jak HTTP dla handlu — **tekstowy**, czytelny, ale wolniejszy niż ITCH/OUCH.
Separator `|` rozdziela pola, każde pole ma numer tagu (jak nagłówki HTTP).

---

### Phase 5: Order Management / Faza 5: Zarządzanie zleceniami

#### 9. `oms/oms.py` — Order Management System

**What:** Manages the full lifecycle of an order: submit → risk check → fill → P&L
**Co:** Zarządza pełnym cyklem życia zlecenia: złożenie → kontrola ryzyka → wypełnienie → P&L

**Analogy:** Think of it like `systemd` for orders:
- An order is like a service unit
- It goes through states: NEW → SENT → FILLED (like inactive → activating → active)
- The OMS tracks position (how many shares you hold) and P&L (profit and loss)

**Analogia:** Pomyśl jak `systemd` dla zleceń:
Zlecenie jest jak unit serwisu, przechodzi przez stany: NEW → SENT → FILLED
(jak inactive → activating → active).
OMS śledzi pozycję (ile akcji posiadasz) i P&L (zysk i stratę).

#### 10. `risk/risk_manager.py` — Risk Manager

**What:** Safety system that prevents dangerous trades
**Co:** System bezpieczeństwa zapobiegający niebezpiecznym transakcjom

**Analogy:** Like `iptables`/`firewalld` for trading — every order must pass through
the risk manager before it can be sent. Rules include:
- Max position per stock (don't buy too much of one thing)
- Max daily loss (stop if you're losing too much)
- Rate limiting (don't send too many orders per second)
- Kill switch (emergency stop — like `systemctl stop`)

**Analogia:** Jak `iptables`/`firewalld` dla handlu — każde zlecenie musi przejść
przez menedżera ryzyka zanim może być wysłane. Zasady obejmują: max pozycja na akcję,
max dzienna strata, ograniczenie częstotliwości, wyłącznik awaryjny (jak `systemctl stop`).

---

### Phase 6: Strategy & Routing / Faza 6: Strategia i routing

#### 11. `strategy/mean_reversion.py` — Trading Strategy

**What:** The decision-making logic — when to buy and when to sell
**Co:** Logika decyzyjna — kiedy kupować i kiedy sprzedawać

**How it works:**
1. Calculate the average price over the last N trades (Simple Moving Average / SMA)
2. If current price is much HIGHER than average → SELL (price will come back down)
3. If current price is much LOWER than average → BUY (price will come back up)
4. Otherwise → do nothing (HOLD)

**Jak działa:**
Oblicz średnią cenę z ostatnich N transakcji (SMA).
Jeśli cena jest dużo WYŻSZA niż średnia → SPRZEDAJ.
Jeśli cena jest dużo NIŻSZA niż średnia → KUP.
W przeciwnym razie → nic nie rób (CZEKAJ).

#### 12. `router/smart_router.py` — Smart Order Router

**What:** Chooses which exchange to send each order to
**Co:** Wybiera do której giełdy wysłać każde zlecenie

**Analogy:** Like a **load balancer** (HAProxy/nginx) for trading orders:
- BEST_PRICE: send to exchange with best price (like least-connections)
- LOWEST_LATENCY: send to fastest exchange (like lowest response time)
- SPLIT: split large order across multiple exchanges (like round-robin)

**Analogia:** Jak **load balancer** (HAProxy/nginx) dla zleceń handlowych:
BEST_PRICE = najlepsza cena (jak least-connections),
LOWEST_LATENCY = najszybsza giełda (jak najniższy czas odpowiedzi),
SPLIT = podziel duże zlecenie na wiele giełd (jak round-robin).

---

### Phase 7: Performance & Infrastructure / Faza 7: Wydajność i infrastruktura

This is where your **Linux skills shine** — these modules are all about making the system faster.

Tu **błyszczą Twoje umiejętności Linuxa** — te moduły dotyczą przyspieszania systemu.

#### 13. `lockfree/spsc_queue.cpp` — Lock-Free Queue

**What:** A super-fast message queue between two threads — no mutexes, no locking
**Co:** Superszybka kolejka wiadomości między dwoma wątkami — bez muteksów, bez blokad

**Analogy:** Think of a **pipe** (`|`) between two processes. But instead of going through
the kernel, this queue lives entirely in memory and uses CPU atomics to avoid locks.
SPSC = Single Producer, Single Consumer (one writer, one reader).

**Analogia:** Pomyśl o **pipe** (`|`) między dwoma procesami. Ale zamiast przechodzić
przez jądro, ta kolejka istnieje całkowicie w pamięci i używa atomowych operacji CPU.
SPSC = jeden pisarz, jeden czytelnik.

#### 14. `memory-latency/cache_latency.cpp` — Cache Latency Tester

**What:** Measures how fast different levels of CPU cache are
**Co:** Mierzy jak szybkie są różne poziomy pamięci podręcznej CPU

**You know this!** L1, L2, L3 cache — you've seen these in `/proc/cpuinfo` or `lscpu`.
This program proves that accessing L1 cache is ~100x faster than main RAM.
In HFT, keeping data in cache is crucial.

**To znasz!** L1, L2, L3 cache — widziałeś je w `/proc/cpuinfo` lub `lscpu`.
Ten program dowodzi, że dostęp do L1 cache jest ~100x szybszy niż do RAM.
W HFT trzymanie danych w cache jest kluczowe.

#### 15. `dpdk-bypass/kernel_bypass_sim.py` — Kernel Bypass Simulation

**What:** Simulates DPDK — bypassing the Linux network stack for faster packet processing
**Co:** Symuluje DPDK — omijanie stosu sieciowego Linuxa dla szybszego przetwarzania pakietów

**You know this!** Normal network path: NIC → kernel → socket → userspace.
DPDK bypasses the kernel: NIC → userspace directly. Much faster, but you lose
kernel features like `iptables`. This file is a simulation (real DPDK requires special NIC drivers).

**To znasz!** Normalna ścieżka: NIC → jądro → socket → przestrzeń użytkownika.
DPDK omija jądro: NIC → przestrzeń użytkownika bezpośrednio. Dużo szybciej,
ale tracisz funkcje jądra jak `iptables`.

#### 16. `linux-tuning/` + `kernel-config/` — Linux Performance Tuning

**What:** Shell scripts and sysctl configs for maximum performance
**Co:** Skrypty shell i konfiguracje sysctl dla maksymalnej wydajności

**This is YOUR territory!** These scripts cover:
- `isolcpus` — dedicate CPU cores to the trading process (no interrupts)
- Hugepages — 2MB pages instead of 4KB (fewer TLB misses)
- IRQ affinity — pin network interrupts to specific cores
- FIFO scheduler — real-time scheduling for the trading thread
- `sysctl` tuning — network buffers, TCP settings

**To TWÓJ teren!** Te skrypty obejmują: isolcpus, hugepages, IRQ affinity,
scheduler FIFO, tuning sysctl.

#### 17. `monitoring/infra_monitor.py` — Infrastructure Monitor

**What:** Monitors system health — CPU, memory, network, latency alerts
**Co:** Monitoruje zdrowie systemu — CPU, pamięć, sieć, alerty opóźnień

**Analogy:** Like `top` + `sar` + `netstat` combined into one monitoring dashboard,
specifically designed for HFT systems.

**Analogia:** Jak `top` + `sar` + `netstat` połączone w jeden dashboard monitoringu,
zaprojektowany specjalnie dla systemów HFT.

---

### Phase 8: Testing & Simulation / Faza 8: Testowanie i symulacja

#### 18. `tests/` — All Test Files

**What:** 54 automated tests that verify everything works correctly
**Co:** 54 automatyczne testy weryfikujące, że wszystko działa poprawnie

**Analogy:** Like running `make test` after compiling a kernel — you want to make sure
nothing is broken.

```bash
make test              # Run all 54 tests / Uruchom wszystkie 54 testy
python3 tests/test_oms.py   # Run just OMS tests / Uruchom tylko testy OMS
```

#### 19. `simulator/market_sim.py` — Market Simulator

**What:** Generates fake market data and runs the full trading pipeline
**Co:** Generuje sztuczne dane rynkowe i uruchamia pełny pipeline handlowy

**How to explore / Jak eksplorować:**
```bash
make simulate                       # Run with default 10K messages
python3 simulator/market_sim.py 50000  # Run with 50K messages
```

#### 20. `tests/benchmark.py` + `benchmark_chart.py`

**What:** Measures speed of all Python components and generates charts
**Co:** Mierzy szybkość wszystkich komponentów Python i generuje wykresy

```bash
make benchmark    # Run all benchmarks / Uruchom wszystkie benchmarki
```

---

## The Full Data Flow / Pełny przepływ danych

Here's how everything connects (like how packets flow through a Linux system):

Oto jak wszystko się łączy (jak pakiety przepływają przez system Linux):

```
EXCHANGE (Giełda)
    │
    ▼
[1] ITCH Parser ──── reads binary market data (like tcpdump parses packets)
    │                 odczytuje binarne dane rynkowe
    ▼
[2] Order Book ───── matches buy/sell orders (like a kernel scheduler matches processes)
    │                 łączy zlecenia kupna/sprzedaży
    ▼
[3] Strategy ─────── decides: BUY / SELL / HOLD (like firewall rules: ACCEPT / DROP)
    │                 decyduje: KUP / SPRZEDAJ / CZEKAJ
    ▼
[4] Risk Manager ─── checks safety limits (like iptables checking each packet)
    │                 sprawdza limity bezpieczeństwa
    ▼
[5] OMS ──────────── manages order lifecycle (like systemd managing services)
    │                 zarządza cyklem życia zlecenia
    ▼
[6] Smart Router ─── picks best exchange (like DNS/load balancer)
    │                 wybiera najlepszą giełdę
    ▼
[7] OUCH/FIX ─────── sends order to exchange (like sending HTTP request)
    │                 wysyła zlecenie do giełdy
    ▼
EXCHANGE (Giełda)
```

---

## Running Everything / Uruchamianie wszystkiego

### Quick Start / Szybki start

```bash
# 1. Install dependency / Zainstaluj zależność
pip install -r requirements.txt

# 2. Compile C++ / Kompiluj C++
make build

# 3. Check syntax / Sprawdź składnię
make lint

# 4. Run tests (54 tests) / Uruchom testy (54 testy)
make test

# 5. Run benchmarks / Uruchom benchmarki
make benchmark

# 6. Run simulator / Uruchom symulator
make simulate
```

### Docker (all-in-one) / Docker (wszystko w jednym)

```bash
docker build -t hft-lab .
docker run hft-lab
```

### Customizing via config.yaml / Dostosowywanie przez config.yaml

Edit `config.yaml` to change parameters without touching code:

Edytuj `config.yaml` aby zmienić parametry bez dotykania kodu:

```yaml
risk:
  max_daily_loss: 50000.0     # Lower the loss limit / Obniż limit strat
strategy:
  window: 50                   # Longer SMA period / Dłuższy okres SMA
  threshold_pct: 0.2           # Bigger threshold / Większy próg
```

Or override with environment variables / Lub nadpisz zmiennymi środowiskowymi:

```bash
HFT_RISK_MAX_DAILY_LOSS=25000 python3 simulator/market_sim.py
```

---

## Glossary / Słowniczek

| Term / Termin | Meaning / Znaczenie |
|---|---|
| **Latency** / Opóźnienie | Time between sending a request and getting a response / Czas między wysłaniem żądania a otrzymaniem odpowiedzi |
| **Throughput** / Przepustowość | How many operations per second / Ile operacji na sekundę |
| **Order Book** / Book zleceń | Sorted list of all buy and sell orders for a stock / Posortowana lista wszystkich zleceń kupna i sprzedaży |
| **Bid** / Oferta kupna | An offer to BUY at a specific price / Oferta KUPNA po konkretnej cenie |
| **Ask** / Oferta sprzedaży | An offer to SELL at a specific price / Oferta SPRZEDAŻY po konkretnej cenie |
| **Spread** | Difference between best bid and best ask / Różnica między najlepszym bid i ask |
| **P&L** | Profit and Loss — how much money you made or lost / Zysk i strata |
| **Position** / Pozycja | How many shares of a stock you currently hold / Ile akcji danej spółki posiadasz |
| **Fill** / Wypełnienie | When your order is matched and executed / Gdy twoje zlecenie zostanie dopasowane i zrealizowane |
| **SMA** | Simple Moving Average — average of last N prices / Prosta średnia ruchoma — średnia z ostatnich N cen |
| **SPSC** | Single Producer Single Consumer — one writer, one reader / Jeden producent, jeden konsument |
| **DPDK** | Data Plane Development Kit — kernel bypass for networking / Zestaw do omijania jądra w sieci |
| **Hugepages** | Large memory pages (2MB vs 4KB) — fewer TLB misses / Duże strony pamięci — mniej chybień TLB |
| **Lock-free** / Bezblokadowy | Data structure that works without mutexes (no waiting) / Struktura danych bez muteksów |
| **Nanosecond (ns)** / Nanosekunda | One billionth of a second (10⁻⁹) / Jedna miliardowa sekundy |
| **Microsecond (μs)** / Mikrosekunda | One millionth of a second (10⁻⁶ = 1000 ns) / Jedna milionowa sekundy |

---

## Tips for Learning / Wskazówki do nauki

1. **Read the tests first** — test files show you exactly how each module is used, with simple examples.
   **Czytaj najpierw testy** — pliki testowe pokazują dokładnie jak każdy moduł jest używany.

2. **Run before reading** — `make test`, `make benchmark`, `make simulate` first.
   Then read the code to understand what just happened.
   **Uruchamiaj przed czytaniem** — najpierw uruchom, potem czytaj kod żeby zrozumieć co się stało.

3. **Use Linux analogies** — everything in this project has a Linux equivalent.
   **Używaj analogii do Linuxa** — wszystko w tym projekcie ma odpowiednik w Linuxie.

4. **Change values and see what happens** — edit `config.yaml`, re-run, compare output.
   **Zmieniaj wartości i obserwuj** — edytuj `config.yaml`, uruchom ponownie, porównaj wynik.

5. **One module at a time** — don't try to understand everything at once. Follow the study order above.
   **Jeden moduł naraz** — nie próbuj zrozumieć wszystkiego naraz. Podążaj za kolejnością nauki powyżej.
