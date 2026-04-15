# How I Tuned Linux for Low-Latency Trading / Jak dostroiłem Linux dla handlu o niskim opóźnieniu

## Why This Matters / Dlaczego to jest ważne

In high-frequency trading, microseconds decide who profits and who loses. The difference between a well-tuned and default Linux server can be 10-100x in latency.
*W handlu o wysokiej częstotliwości mikrosekundy decydują o tym, kto czerpie zysk, a kto traci. Różnica między dobrze dostrojonym a domyślnym serwerem Linux może wynosić 10-100x w opóźnieniu.*

## The Environment / Środowisko

Initially built on Ubuntu 24.04 LTS, later migrated to Red Hat Enterprise Linux 10.1 to work with an enterprise distro used in production trading environments.
*Początkowo zbudowany na Ubuntu 24.04 LTS, później przeniesiony do Red Hat Enterprise Linux 10.1 do pracy z dystrybucją przedsiębiorstwa używaną w środowiskach handlowych produkcji.*

- OS: Red Hat Enterprise Linux 10.1 (Coughlan)
- VM: VirtualBox (2 vCPUs, 4GB RAM, 40GB disk)
- Kernel: 6.12.0-124.8.1.el10_1.x86_64
- Compiler: g++ 14.3.1, -O2 -std=c++17

## Step 1: Hugepages / Krok 1: Ogromne strony

Default Linux uses 4KB memory pages. The CPU has a TLB that caches page mappings. With 4KB pages, the TLB constantly misses — each miss costs ~10-30ns.

I reserved 512 hugepages (2MB each = 1GB total). Result: fewer TLB misses, more predictable memory access.
*Domyślny Linux używa stron pamięci o rozmiarze 4KB. Procesor ma TLB, który buforuje mapowania stron. Na stronach 4KB, TLB nieustannie nie trafia — każdy błąd kosztuje ~10-30ns. Zarezerwowałem 512 ogromnych stron (po 2MB każda = 1GB łącznie). Rezultat: mniej błędów TLB, bardziej przewidywalny dostęp do pamięci.*

## Step 2: CPU Isolation / Krok 2: Izolacja procesora

By default, the scheduler moves processes between CPUs freely, causing cache pollution.

I isolated CPU 1: isolcpus=1 nohz_full=1 rcu_nocbs=1

Result: CPU 1 is completely clean for the trading process.
*Domyślnie harmonogram przesuwa procesy między procesorami swobodnie, powodując zanieczyszczenie pamięci podręcznej. Izolowałem procesor 1. Rezultat: procesor 1 jest całkowicie czysty do procesu handlowego.*

## Step 3: IRQ Affinity / Krok 3: Afinność IRQ

I pinned all hardware interrupts to CPU 0 so CPU 1 receives zero interrupts.
*Przypinałem wszystkie przerwania sprzętowe do CPU 0, aby procesor 1 nie otrzymywał żadnych przerwań.*

## Step 4: Network Stack Tuning / Krok 4: Dostrojenie stosu sieciowego

16MB socket buffers, tcp_low_latency=1, swappiness=0 — everything stays in RAM.
*Bufory gniazd 16MB, tcp_low_latency=1, swappiness=0 — wszystko pozostaje w pamięci RAM.*

## Step 5: Polling vs Interrupts / Krok 5: Sondowanie a przerwania

Poll mode: ~80us avg latency. Interrupt-driven: ~450us. Polling is 5.6x faster.
*Tryb sondowania: ~80us średnie opóźnienie. Oparte na przerwaniach: ~450us. Sondowanie jest 5,6 razy szybsze.*

## Results / Wyniki

| Optimisation | Impact |
|-------------|--------|
| Hugepages | Fewer TLB misses |
| CPU isolation | Zero scheduler interference |
| IRQ affinity | Zero interrupts on trading core |
| Network tuning | No packet drops, lower latency |
| Poll mode | 5.6x latency reduction |

*Ogromne strony: mniej błędów TLB. Izolacja procesora: brak ingerencji harmonogramu. Afinność IRQ: brak przerwań na rdzeniu handlowym. Dostrojenie sieci: brak utraty pakietów, niższe opóźnienie. Tryb sondowania: redukcja opóźnienia 5,6 razy.*

## What I Would Do Next / Co bym zrobił dalej

- Real DPDK with supported NIC
- NUMA-aware memory allocation
- Real-time kernel (PREEMPT_RT)
- Hardware timestamping with PTP

*Rzeczywisty DPDK z obsługiwaną kartą sieciową. Alokacja pamięci uwzględniająca NUMA. Jądro czasu rzeczywistego (PREEMPT_RT). Znaczniki czasu sprzętu z PTP.*

## Key Takeaway / Główny wniosek

Low-latency is not about faster hardware — it is about removing unnecessary work. The fastest code is the code that never runs.
*Niska opóźnienie nie chodzi o szybszy sprzęt — chodzi o usuwanie niepotrzebnej pracy. Najszybszy kod to kod, który nigdy nie jest uruchamiany.*
