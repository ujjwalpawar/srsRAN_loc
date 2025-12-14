export SRSRAN_IQ_UDP_IP="127.0.0.1"
export SRSRAN_IQ_UDP_PORT="12345"
~/srsran_april2025/build/apps/gnb$ sudo taskset -c 11-50 chrt -f 99 ./gnb -c ~/gnb_inf3_old.yml