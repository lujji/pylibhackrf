# pylibhackrf
Python module for communicating with HackRF

## Building
Requires libpython3-dev and libhackrf-dev >= 2022.09.1

```
python3 setup.py build_ext --inplace
```

## Example
### TX
``` Python
import py_hackrf
from time import sleep

# frequency = 433.92MHz, sample rate = 2MSPS
py_hackrf.init(433920000, 2000000)
py_hackrf.set_tx_gain(40)

payload = [] # fill in tx buffer here

# start tx and wait for it to finish
py_hackrf.start_tx(payload)
while py_hackrf.busy():
    sleep(0.1)

# stop_transfer must be called after each tx
py_hackrf.stop_transfer()

# cleanup
py_hackrf.deinit()
```

### RX
``` Python
import py_hackrf
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
```
