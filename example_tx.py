from py_hackrf import py_hackrf
from time import sleep

# frequency = 433.92MHz, sample rate = 2MSPS
py_hackrf.init(433920000, 2000000)
py_hackrf.set_tx_gain(40)

payload = [0xff]*32 # fill in tx buffer here

# start tx and wait for it to finish
py_hackrf.start_tx(payload)
while py_hackrf.busy():
    sleep(0.1)

# stop_transfer must be called after each tx
py_hackrf.stop_transfer()

# cleanup
py_hackrf.deinit()
