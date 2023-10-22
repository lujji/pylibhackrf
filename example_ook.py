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

    def encode(self, message: str, encoding: dict) -> str:
        '''
        Encode payload based on provided encoding

            Parameters:
                message: message to be encoded
                encoding: dictionary used for encoding
        '''
        return ''.join([encoding[i] if i in encoding else i for i in message])

    def generate(self, packet: str) -> list:
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


def de_bruijn(alphabet, n: int) -> str:
    '''
    generate de Bruijn sequence for given alphabet and subsequences of length n
    https://en.wikipedia.org/wiki/De_Bruijn_sequence

        Example:
        # generate binary codes with a length of 8
        msg = de_bruijn("01", 8)
    '''
    k = len(alphabet)

    a = [0] * k * n
    sequence = []

    def db(t, p):
        if t > n:
            if n % p == 0:
                sequence.extend(a[1 : p + 1])
        else:
            a[t] = a[t - p]
            db(t + 1, p)
            for j in range(a[t - p] + 1, k):
                a[t] = j
                db(t + 1, t)

    db(1, 1)
    return ''.join(alphabet[i] for i in sequence)


if __name__ == '__main__':
    py_hackrf.init(433920000, 2000000)
    py_hackrf.set_tx_gain(40)

    ook = OOK(F_S=2000000, F=14000, symbol_len=800, pause_len=20000)

    '''
    transmit all possible addresses for JZFR22 doorbells
    uses the following encoding: 0 = 100, 1 = 110
    last 3 symbols are the address - total of 8 combinations
    message is 8 symbols + 1 sync pulse followed by a pause

    100 110 110 100 100  110 110 100 1p
     0   1   1   0   0    1   1   0
    |     preamble     | |   addr  || s |
    '''
    preamble = '01100'
    enc = {'0' : '100', '1': '110'}

    for i in range(9):
        addr = '{0:03b}'.format(i)
        msg = ook.generate(ook.encode(preamble + addr, enc) + '1p')

        py_hackrf.start_tx(msg*32)
        while py_hackrf.busy():
            sleep(0.1)
        py_hackrf.stop_transfer()

    py_hackrf.deinit()
