# pylibhackrf
Python module for communicating with HackRF

- example_ook.py - OOK (on-off keying) processor
- example_rx/tx - rx/tx examples

## Building
Requires libpython3-dev and libhackrf-dev >= 2022.09.1

```
cd py_hackrf && python3 setup.py build_ext --inplace
```

## Example
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
