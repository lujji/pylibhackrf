# pylibhackrf
Python module for communicating with HackRF. This extension is written in C as opposed to various python wrappers around libhackrf that handle libusb callbacks from Python, which quickly becomes unusable at higher sample rates. The library uses a FIFO for rx/tx and supports streaming mode.

## Building
Requires libpython3-dev and libhackrf-dev >= 2022.09.1

```
cd py_hackrf && python3 setup.py build_ext --inplace
```

## Example
``` Python
from py_hackrf import py_hackrf
from time import sleep
import matplotlib.pyplot as plt
import numpy as np

# sample rate 2MHz, frequency 433.92MHz
F_S = 2e6
freq = 433.92e6

# initialize hackrf, FIFO is not used
hackrf = py_hackrf.hackrf(0)

# vga = 24, lna = 16 
hackrf.set_rx_gain(24, 16)

# set sample rate
hackrf.set_sample_rate(int(F_S))

# tune to frequency
hackrf.set_freq(int(freq))

# rx for 10ms (x2 due to I and Q parts)
hackrf.start_rx(2 * int(F_S / 1000))
while hackrf.busy():
    sleep(0.1)
hackrf.stop_transfer()

# read buffer
rx = hackrf.read()
rx = np.frombuffer(rx, dtype=np.int8)

# extract I and Q components
re = rx[0::2]
im = rx[1::2]

# plot I and Q signals
plt.plot(re)
plt.plot(im)
plt.show()
```
