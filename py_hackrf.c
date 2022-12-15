#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <libhackrf/hackrf.h>
#include <Python.h>

#if defined(DEBUG) && (DEBUG == 1)
#include <stdio.h>
#define DEBUG_OUT(...)  printf(__VA_ARGS__);
#else
#define DEBUG_OUT(...)
#endif

static int8_t *buf = NULL;
static size_t tx_buf_len = 0;
static size_t rx_buf_len = 0;
static size_t tx_idx = 0;
static size_t rx_idx = 0;
static volatile bool tx_done = false;
static volatile bool busy = false;
static bool initialized = false;
static hackrf_device* device = NULL;

static void flush_callback(void* flush_ctx, int success) {
    DEBUG_OUT("flush callback: %d\n", success);
    busy = false;
}

static int tx_callback(hackrf_transfer* transfer) {
    if (tx_done) {
        DEBUG_OUT("tx done!\n");
        return -1;
    }

    size_t buffer_length = transfer->buffer_length;
    size_t len = buffer_length;
    if (tx_idx + buffer_length >= tx_buf_len) {
        len = tx_buf_len - tx_idx;
        tx_done = true;
    }

    DEBUG_OUT("buffer_length = %ld, len = %ld\n", buffer_length, len);

    memcpy(transfer->buffer, &buf[tx_idx], len);

    tx_idx += buffer_length;
    return 0;
}

static int rx_callback(hackrf_transfer* transfer) {
    if (!busy) {
        DEBUG_OUT("rx done!\n");
        return -1;
    }

    size_t len = transfer->valid_length;

    if (rx_idx + len >= rx_buf_len) {
        len = rx_buf_len - rx_idx;
        DEBUG_OUT("rx last chunk: buffer_length = %d, len = %ld\n", transfer->valid_length, len);
        memcpy(buf + rx_idx, transfer->buffer, len);
        busy = false;
        return -1;
    }

    DEBUG_OUT("buffer_length = %d, len = %ld\n", transfer->valid_length, len);

    memcpy(buf + rx_idx, transfer->buffer, len);

    rx_idx += len;
    return 0;
}

static PyObject *py_init(PyObject *self, PyObject *args) {
    uint64_t freq, sample_rate;

    if (!PyArg_ParseTuple(args, "KK", &freq, &sample_rate)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a list");
        return NULL;
    }

    if (hackrf_init() != HACKRF_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "unable to init hackrf!");
        return NULL;
    };

    if (hackrf_open(&device) != HACKRF_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "unable to open hackrf!");
        return NULL;
    }

    hackrf_set_sample_rate(device, sample_rate);
    hackrf_set_hw_sync_mode(device, 0);
    hackrf_set_freq(device, freq);
    hackrf_enable_tx_flush(device, flush_callback, NULL);

    initialized = true;

    Py_RETURN_NONE;
}

static PyObject *py_set_tx_gain(PyObject *self, PyObject *args) {
    uint32_t gain;

    if (!PyArg_ParseTuple(args, "I", &gain)) {
        PyErr_SetString(PyExc_TypeError, "argument must be an integer");
        return NULL;
    }

    int ok = hackrf_set_txvga_gain(device, gain);
    return Py_BuildValue("i", ok);
}

static PyObject *py_set_rx_gain(PyObject *self, PyObject *args) {
    uint32_t lna_gain, vga_gain;
    int ok = 0;

    if (!PyArg_ParseTuple(args, "II", &vga_gain, &lna_gain)) {
        PyErr_SetString(PyExc_TypeError, "invalid argument");
        return NULL;
    }

    if (hackrf_set_vga_gain(device, vga_gain) != HACKRF_SUCCESS) {
        ok = -1;
        DEBUG_OUT("could not set vga_gain\n");
    }

    if (hackrf_set_lna_gain(device, lna_gain) != HACKRF_SUCCESS) {
        ok = -1;
        DEBUG_OUT("could not set lna_gain\n");
    }

    return Py_BuildValue("i", ok);
}

static PyObject *py_set_amp(PyObject *self, PyObject *args) {
    uint32_t enable;

    if (!PyArg_ParseTuple(args, "I", &enable)) {
        PyErr_SetString(PyExc_TypeError, "argument must be int");
        return NULL;
    }

    int ok = hackrf_set_amp_enable(device, (uint8_t) enable);
    return Py_BuildValue("i", ok);
}

static PyObject *py_start_rx(PyObject *self, PyObject *args) {
    if (busy) {
        Py_RETURN_FALSE;
    }

    if (!initialized) {
        PyErr_SetString(PyExc_TypeError, "not initialized!");
        return NULL;
    }

    uint64_t samples;
    if (!PyArg_ParseTuple(args, "K", &samples)) {
        PyErr_SetString(PyExc_TypeError, "invalid argument");
        return NULL;
    }

    buf = (int8_t *) malloc(samples);
    if (buf == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    rx_buf_len = samples;
    rx_idx = 0;
    busy = true;

    int ok = hackrf_start_rx(device, rx_callback, NULL);
    return Py_BuildValue("i", ok);
}

static PyObject *py_read(PyObject *self, PyObject *Py_UNUSED(unused)) {
    if (busy) {
        DEBUG_OUT("rx busy\n");
        Py_RETURN_NONE;
    }

    hackrf_stop_rx(device);

    PyObject* array = PyList_New(rx_buf_len);
    for (size_t i = 0; i < rx_buf_len; i++) {
        PyList_SetItem(array, i, PyLong_FromLong((long) buf[i]));
    }

    free(buf);
    buf = NULL;

    return array;
}

static PyObject *py_start_tx(PyObject *self, PyObject *args) {
    if (busy) {
        Py_RETURN_FALSE;
    }

    if (!initialized) {
        PyErr_SetString(PyExc_TypeError, "not initialized!");
        return NULL;
    }

    PyObject *tx_list;
    if (!PyArg_ParseTuple(args, "O", &tx_list)) {
        return NULL;
    }

    if (!PyList_Check(tx_list)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a list");
        return NULL;
    }

    size_t len = PyObject_Length(tx_list);

    DEBUG_OUT("allocating buffer\n");
    buf = (int8_t *) malloc(len);

    if (buf == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    tx_buf_len = len;

    for (size_t i = 0; i < len; i++) {
        PyObject *v = PyList_GetItem(tx_list, i);
        if (!PyLong_Check(v)) {
            PyErr_SetString(PyExc_TypeError, "value is not an integer");
            free(buf);
            buf = NULL;
            return NULL;
        }

        buf[i] = (int8_t) PyLong_AsLong(v);
    }

    busy = true;
    tx_buf_len = len;
    tx_idx = 0;
    tx_done = false;

    int ok = hackrf_start_tx(device, tx_callback, NULL);
    return Py_BuildValue("i", ok);
}

static PyObject *py_deinit(PyObject *self, PyObject *Py_UNUSED(unused)) {
    if (!initialized) {
        PyErr_SetString(PyExc_TypeError, "not initialized!");
        return NULL;
    }

    hackrf_close(device);
    hackrf_exit(device);

    if (buf != NULL) {
        DEBUG_OUT("freeing buffer\n");
        free(buf);
        buf = NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *py_stop_transfer(PyObject *self, PyObject *Py_UNUSED(unused)) {
    if (!initialized) {
        PyErr_SetString(PyExc_TypeError, "not initialized!");
        return NULL;
    }

    hackrf_stop_tx(device); // same code used for rx

    busy = false;
    Py_RETURN_NONE;
}

static PyObject *py_busy(PyObject *self, PyObject *Py_UNUSED(unused)) {
    if (busy) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyMethodDef methodTable[] = {
    {"init", py_init, METH_VARARGS, "initialize hackrf module"},
    {"deinit", py_deinit, METH_NOARGS, "close driver and cleanup memory"},
    {"busy", py_busy, METH_NOARGS, "check if transmission is in progress"},
    {"start_tx", py_start_tx, METH_VARARGS, "start transmission"},
    {"start_rx", py_start_rx, METH_VARARGS, "receive a number of samples"},
    {"read", py_read, METH_NOARGS, "read received data"},
    {"set_tx_gain", py_set_tx_gain, METH_VARARGS, "set tx gain"},
    {"set_rx_gain", py_set_rx_gain, METH_VARARGS, "set rx lna and vga"},
    {"set_amp", py_set_amp, METH_VARARGS, "set rf amp state"},
    {"stop_transfer", py_stop_transfer, METH_NOARGS, "stop transmission"},
    { NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "py_hackrf", "hackrf python wrapper", -1, methodTable
};

PyMODINIT_FUNC
PyInit_py_hackrf() {
    return PyModule_Create(&module);
}
