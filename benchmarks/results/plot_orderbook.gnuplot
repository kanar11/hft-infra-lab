# Gnuplot script for Orderbook Operation Latencies
# Skrypt gnuplot dla opóźnień operacji orderbooka
#
# Usage / Użycie:
#   gnuplot plot_orderbook.gnuplot
#
# Produces: orderbook_latency.png

set terminal png size 800,500 enhanced font "Arial,12"
set output 'orderbook_latency.png'

set title "Orderbook Operation Latency (nanoseconds)"
set xlabel "Operation index"
set ylabel "Latency (ns)"
set grid
set logscale y
set key top left

set datafile separator ","

plot 'orderbook_raw.csv' using 2:3 skip 1 every ::0 \
     with dots lc rgb "#4472C4" title "ADD", \
     '' using 2:3 skip 1 every ::5001::10000 \
     with dots lc rgb "#ED7D31" title "CANCEL", \
     '' using 2:3 skip 1 every ::10001 \
     with dots lc rgb "#70AD47" title "MATCH"
