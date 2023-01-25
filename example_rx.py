from py_hackrf import py_hackrf
from time import sleep

# frequency = 433.92MHz, sample rate = 2MSPS
py_hackrf.init(433920000, 2000000)
py_hackrf.set_rx_gain(24, 16)

# receive 1024 samples
py_hackrf.start_rx(1024)
while py_hackrf.busy():
    sleep(0.1)

# get the buffer
rx_data = py_hackrf.read()
print(rx_data)

# cleanup
py_hackrf.deinit()
