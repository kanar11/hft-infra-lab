# Multicast Market Data Feed / Kanał danych rynkowych Multicast

UDP multicast sender and receiver for simulating exchange market data feeds.
*Nadajnik i odbiornik UDP multicast do symulowania kanałów danych giełdowych.*

## Components / Komponenty
- `mc_sender.py` — Sends timestamped market data via multicast / Wysyła dane rynkowe ze znacznikami czasu przez multicast
- `mc_receiver.py` — Receives and displays multicast messages / Odbiera i wyświetla wiadomości multicast
- `mc_receiver_latency.py` — Measures per-message latency (send→receive) / Mierzy opóźnienie na wiadomość (wysłanie→odbiór)

## Run / Uruchomienie
Terminal 1: `python3 mc_sender.py`
Terminal 2: `python3 mc_receiver_latency.py`
