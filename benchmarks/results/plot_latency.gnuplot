# Gnuplot script for Ping-Pong Latency Distribution
# Skrypt gnuplot dla rozkładu opóźnień Ping-Pong
#
# Usage / Użycie:
#   gnuplot plot_latency.gnuplot
#
# Produces: latency_distribution.png
# NOTE: gnuplot is optional — the CSV data is the primary output
#       gnuplot jest opcjonalny — dane CSV są głównym wynikiem

set terminal png size 800,500 enhanced font "Arial,12"
set output 'latency_distribution.png'

set title "Ping-Pong Thread-to-Thread Latency Distribution"
set xlabel "Round-trip latency (ns)"
set ylabel "Frequency"
set grid
set style fill solid 0.5

# Read CSV, skip header, plot histogram
set datafile separator ","
set key off

# Histogram with 50ns bins
binwidth = 50
bin(x, width) = width * floor(x / width)

plot 'latency_raw.csv' using (bin($2, binwidth)):(1.0) skip 1 \
     smooth freq with boxes lc rgb "#4472C4" title "Latency"
