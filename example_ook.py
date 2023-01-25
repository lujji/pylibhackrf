from py_hackrf import py_hackrf
from time import sleep
import numpy as np


class OOK:
    def __init__(self, F_S, F, symbol_len, pause_len=0, amp=127):
        '''
        Initialize OOK processor

            Parameters:
                F_S (int): sample rate in Hz
                F (int): carrier frequency in Hz
                symbol_len (int): symbol length in samples
                pause_len (int): pause ('p') length in samples
                amp (int): signal amplitude
        '''
        self.F_S = F_S
        self.F = F
        self.symbol_len = symbol_len * 2
        self.pause_len = pause_len * 2

        phi = np.pi / 2
        lut = np.array([amp * np.exp(-1j * (2 * np.pi * i * F/F_S + phi))
                       for i in range(round(F_S/F))])
        self.sine_lut = [int(round(i))
                         for _ in list(zip(lut.real, lut.imag)) for i in _]
        print([int(round(i)) for i in lut.real])
        print([int(round(i)) for i in lut.imag])
        print(len([int(round(i)) for i in lut.imag]))
        self.symbol_0 = [0] * self.symbol_len
        self.symbol_p = [0] * self.pause_len

    def generate(self, packet: str):
        '''
        Generate OOK payload

            Packet may contain following symbols:
            '1' - logical 1
            '0' - logical 0
            'p' - pause (logical 0 with pause_len duration)
        '''
        payload = []
        ctr = 0
        for symbol in packet:
            if symbol == '1':
                payload.extend([self.sine_lut[(i + ctr) % len(self.sine_lut)]
                               for i in range(self.symbol_len)])
                ctr += self.symbol_len
            elif symbol == '0':
                payload.extend(self.symbol_0)
                ctr += self.symbol_len
            elif symbol == 'p':
                payload.extend(self.symbol_p)
                ctr += self.pause_len
            else:
                print("unknown symbol:", symbol)
        return payload


if __name__ == "__main__":
    """ transmit all possible addresses for JZFR22 doorbells """
    py_hackrf.init(433920000, 2000000)
    py_hackrf.set_tx_gain(40)

    ook = OOK(F_S=2000000, F=14000, symbol_len=800, pause_len=20000)

    preamble = '10011011 01001001'

    for i in [36, 38, 52, 54, 164, 166, 180]:
        addr = '{0:08b}'.format(i)
        msg = ook.generate(preamble + addr + '1p')

        py_hackrf.start_tx(msg*32)
        while py_hackrf.busy():
            sleep(0.1)
        py_hackrf.stop_transfer()

    py_hackrf.deinit()
