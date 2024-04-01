#include <Python.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <libhackrf/hackrf.h>
#include "queue.h"

#if defined(DEBUG) && (DEBUG == 1)
#include <stdio.h>
#define DEBUG_OUT(...)  printf(__VA_ARGS__);
#else
#define DEBUG_OUT(...)
#endif

struct packet {
    int8_t *buf;
    size_t size;
};

typedef struct {
    PyObject_HEAD
    hackrf_device *device;
    struct queue pkt_queue;
    struct packet data_pkt;
    size_t tx_len;
    size_t tx_idx;
    size_t rx_idx;
    bool allow_overruns;
    volatile bool busy;
} HackrfObject;

static int pkt_allocate(HackrfObject *self, size_t size) {
    if (self->data_pkt.buf != NULL) {
        DEBUG_OUT("buffer is dirty - reallocation\n");
        self->data_pkt.buf = (int8_t *) realloc(self->data_pkt.buf, size);
    } else {
        DEBUG_OUT("allocating buffer, size %zu\n", size);
        self->data_pkt.buf = (int8_t *) malloc(size);
    }

    if (self->data_pkt.buf == NULL) {
        return -1;
    }

    self->data_pkt.size = size;
    return 0;
}

static void pkt_free(HackrfObject *self) {
    if (self->data_pkt.buf != NULL) {
        DEBUG_OUT("freeing buffer\n");
        free(self->data_pkt.buf);
        self->data_pkt.buf = NULL;
        self->data_pkt.size = 0;
    }
}

static void flush_queue(struct queue *q) {
    struct packet pkt;
    while (queue_pop_noblock(q, &pkt)) {
        if (pkt.buf != NULL) {
            free(pkt.buf);
        }
    }
}

static void flush_callback(void *flush_ctx, int success) {
    DEBUG_OUT("flush callback: %d\n", success);
    HackrfObject *self = (HackrfObject *) flush_ctx;
    self->busy = false;
}

static int tx_callback(hackrf_transfer *transfer) {
    HackrfObject *self = (HackrfObject *) transfer->tx_ctx;

    int ret = 0;
    if (!self->busy) {
        DEBUG_OUT("tx done!\n");
        return -1;
    }

    size_t buffer_length = transfer->buffer_length;
    size_t len = buffer_length;
    if (self->tx_idx + buffer_length >= self->data_pkt.size) {
        len = self->data_pkt.size - self->tx_idx;
        ret = -1;
    }

    DEBUG_OUT("buffer_length = %zu, len = %zu\n", buffer_length, len);

    memcpy(transfer->buffer, self->data_pkt.buf + self->tx_idx, len);
    self->tx_idx += buffer_length;

    return ret;
}

static int rx_callback(hackrf_transfer *transfer) {
    DEBUG_OUT("buffer_length = %d\n", transfer->valid_length);
    HackrfObject *self = (HackrfObject *) transfer->rx_ctx;

    if (!self->busy) {
        DEBUG_OUT("rx done!\n");
        return -1;
    }

    size_t len = transfer->valid_length;
    if (self->rx_idx + len >= self->data_pkt.size) {
        len = self->data_pkt.size - self->rx_idx;
        DEBUG_OUT("rx last chunk, len = %zu\n", len);
        memcpy(self->data_pkt.buf + self->rx_idx, transfer->buffer, len);
        self->busy = false;
        return -1;
    }

    memcpy(self->data_pkt.buf + self->rx_idx, transfer->buffer, len);
    self->rx_idx += len;

    return 0;
}

static int rx_stream_callback(hackrf_transfer *transfer) {
    DEBUG_OUT("rx_len = %d\n", transfer->valid_length);
    HackrfObject *self = (HackrfObject *) transfer->rx_ctx;

    if (!self->busy) {
        DEBUG_OUT("rx done!\n");
        return -1;
    }

    if (transfer->valid_length > 0) {
        struct packet pkt;
        pkt.buf = malloc(transfer->valid_length);
        if (pkt.buf == NULL) {
            DEBUG_OUT("unable to allocate memory for rx stream\n");
            return -1;
        }

        memcpy(pkt.buf, transfer->buffer, transfer->valid_length);
        pkt.size = transfer->valid_length;

        if (!queue_push_noblock(&self->pkt_queue, &pkt)) {
            DEBUG_OUT("rx queue full - dropping pkt\n");
            free(pkt.buf);

            if (!self->allow_overruns) {
                self->busy = false;
                return -1;
            }
        }
    }

    return 0;
}

static int tx_stream_callback(hackrf_transfer *transfer) {
    HackrfObject *self = (HackrfObject *) transfer->tx_ctx;
    size_t idx = 0;

    if (!self->busy) {
        DEBUG_OUT("tx done!\n");
        return -1;
    }

    if (self->data_pkt.buf) {
        if (self->tx_len > (size_t) transfer->buffer_length) {
            DEBUG_OUT("tx draining pkt: %zu\n", self->tx_len);
            memcpy(transfer->buffer, self->data_pkt.buf + self->tx_idx, transfer->buffer_length);
            self->tx_idx += transfer->buffer_length;
            self->tx_len -= transfer->buffer_length;
            return 0;
        } else {
            DEBUG_OUT("tx drained pkt: %zu\n", self->tx_len);
            memcpy(transfer->buffer, self->data_pkt.buf + self->tx_idx, self->tx_len);
            idx = self->tx_len;
            free(self->data_pkt.buf);
            self->data_pkt.buf = NULL;
        }
    }

    /* pop messages from queue until we have enough data to send */
    size_t remaining_bytes = transfer->buffer_length - idx;
    while (remaining_bytes > 0) {
        if (!queue_pop_noblock(&self->pkt_queue, &self->data_pkt)) {
            DEBUG_OUT("tx queue is empty - idling\n");
            memset(transfer->buffer + idx, 0, remaining_bytes);
            return self->allow_overruns ? 0 : -1;
        }

        if (self->data_pkt.size > remaining_bytes) {
            DEBUG_OUT("tx pkt_size = %zu, remaining = %zu\n", self->data_pkt.size, remaining_bytes);
            memcpy(transfer->buffer + idx, self->data_pkt.buf, remaining_bytes);
            self->tx_idx = remaining_bytes;
            self->tx_len = self->data_pkt.size - remaining_bytes;
            return 0;
        }

        DEBUG_OUT("tx copy %zu, idx = %zu\n", self->data_pkt.size, idx);
        memcpy(transfer->buffer + idx, self->data_pkt.buf, self->data_pkt.size);
        idx += self->data_pkt.size;
        free(self->data_pkt.buf);
        self->data_pkt.buf = NULL;
    }

    DEBUG_OUT("tx %d\n", transfer->buffer_length);

    return 0;
}

static PyObject *py_set_sample_rate(HackrfObject *self, PyObject *args) {
    uint64_t sample_rate;
    if (!PyArg_ParseTuple(args, "K", &sample_rate)) {
        PyErr_SetString(PyExc_TypeError, "argument must be unsigned long long");
        Py_RETURN_NONE;
    }

    hackrf_set_sample_rate(self->device, sample_rate);

    Py_RETURN_NONE;
}

static PyObject *py_set_freq(HackrfObject *self, PyObject *args) {
    uint64_t freq;
    if (!PyArg_ParseTuple(args, "K", &freq)) {
        PyErr_SetString(PyExc_TypeError, "argument must be unsigned long long");
        Py_RETURN_NONE;
    }

    hackrf_set_freq(self->device, freq);

    Py_RETURN_NONE;
}

static PyObject *py_set_baseband_filter_bandwidth(HackrfObject *self, PyObject *args) {
    uint32_t freq;
    if (!PyArg_ParseTuple(args, "I", &freq)) {
        PyErr_SetString(PyExc_TypeError, "argument must be an integer");
        Py_RETURN_NONE;
    }

    int ok = hackrf_set_baseband_filter_bandwidth(self->device, freq);
    return PyLong_FromLong(ok);
}

static PyObject *py_set_tx_gain(HackrfObject *self, PyObject *args) {
    uint32_t gain;
    if (!PyArg_ParseTuple(args, "I", &gain)) {
        PyErr_SetString(PyExc_TypeError, "argument must be an integer");
        Py_RETURN_NONE;
    }

    int ok = hackrf_set_txvga_gain(self->device, gain);
    return PyLong_FromLong(ok);
}

static PyObject *py_set_rx_gain(HackrfObject *self, PyObject *args) {
    uint32_t lna_gain, vga_gain;
    int ok = 0;

    if (!PyArg_ParseTuple(args, "II", &vga_gain, &lna_gain)) {
        PyErr_SetString(PyExc_TypeError, "invalid argument");
        Py_RETURN_NONE;
    }

    if (hackrf_set_vga_gain(self->device, vga_gain) != HACKRF_SUCCESS) {
        ok = -1;
        DEBUG_OUT("could not set vga_gain\n");
    }

    if (hackrf_set_lna_gain(self->device, lna_gain) != HACKRF_SUCCESS) {
        ok = -1;
        DEBUG_OUT("could not set lna_gain\n");
    }

    return PyLong_FromLong(ok);
}

static PyObject *py_set_amp(HackrfObject *self, PyObject *args) {
    uint32_t enable;
    if (!PyArg_ParseTuple(args, "I", &enable)) {
        PyErr_SetString(PyExc_TypeError, "argument must be int");
        Py_RETURN_NONE;
    }

    int ok = hackrf_set_amp_enable(self->device, (uint8_t) enable);
    return PyLong_FromLong(ok);
}

static PyObject *py_set_antenna_enable(HackrfObject *self, PyObject *args) {
    int enable;
    if (!PyArg_ParseTuple(args, "p", &enable)) {
        PyErr_SetString(PyExc_TypeError, "argument must be bool");
        Py_RETURN_NONE;
    }

    int ok = hackrf_set_antenna_enable(self->device, (uint8_t) enable);
    return PyLong_FromLong(ok);
}

static PyObject *py_set_hw_sync_mode(HackrfObject *self, PyObject *args) {
    int enable;
    if (!PyArg_ParseTuple(args, "p", &enable)) {
        PyErr_SetString(PyExc_TypeError, "argument must be bool");
        Py_RETURN_NONE;
    }

    int ok = hackrf_set_hw_sync_mode(self->device, (uint8_t) enable);
    return PyLong_FromLong(ok);
}

static PyObject *py_read(HackrfObject *self, PyObject *Py_UNUSED(unused)) {
    if (self->busy) {
        DEBUG_OUT("rx busy\n");
        Py_RETURN_NONE;
    }

    PyObject *array = PyByteArray_FromStringAndSize((const char *) self->data_pkt.buf, self->data_pkt.size);

    pkt_free(self);

    return array;
}

static PyObject *py_pop(HackrfObject *self, PyObject *args) {
    uint32_t timeout = 0;
    if (!PyArg_ParseTuple(args, "|I", &timeout)) {
        PyErr_SetString(PyExc_TypeError, "argument must be int");
        Py_RETURN_NONE;
    }

    if (self->pkt_queue.size == 0) {
        PyErr_SetString(PyExc_RuntimeError, "queue not initialized");
        Py_RETURN_NONE;
    }

    struct packet pkt = {0};
    if (queue_pop(&self->pkt_queue, &pkt, timeout)) {
        if (pkt.buf == NULL) {
            DEBUG_OUT("rx thread: buffer is null\n");
            Py_RETURN_NONE;
        }

        DEBUG_OUT("pop %zu bytes\n", pkt.size);

        PyObject *array = PyByteArray_FromStringAndSize((const char *) pkt.buf, pkt.size);
        free(pkt.buf);
        return array;
    }

    Py_RETURN_NONE;
}

static PyObject *py_push(HackrfObject *self, PyObject *args) {
    uint32_t timeout;
    PyObject *tx_buf;
    if (!PyArg_ParseTuple(args, "OI", &tx_buf, &timeout)) {
        PyErr_SetString(PyExc_TypeError, "invalid argument");
        Py_RETURN_NONE;
    }

    if (!PyByteArray_Check(tx_buf)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a bytearray");
        Py_RETURN_NONE;
    }

    if (self->pkt_queue.size == 0) {
        PyErr_SetString(PyExc_RuntimeError, "queue not initialized");
        Py_RETURN_NONE;
    }

    size_t len = PyByteArray_GET_SIZE(tx_buf);

    struct packet pkt;
    pkt.size = len;
    pkt.buf = malloc(len);
    if (pkt.buf == NULL) {
        PyErr_NoMemory();
        Py_RETURN_NONE;
    }
    memcpy(pkt.buf, PyByteArray_AS_STRING(tx_buf), len);

    if (!queue_push_noblock(&self->pkt_queue, &pkt)) {
        DEBUG_OUT("rx queue full - dropping pkt\n");
        free(pkt.buf);
        Py_RETURN_FALSE;
    }

    Py_RETURN_TRUE;
}

static PyObject *py_start_rx(HackrfObject *self, PyObject *args) {
    if (self->busy) {
        Py_RETURN_FALSE;
    }

    size_t rx_len = 0;
    if (!PyArg_ParseTuple(args, "K", &rx_len)) {
        PyErr_SetString(PyExc_TypeError, "invalid length");
        Py_RETURN_NONE;
    }

    if (pkt_allocate(self, rx_len) != 0) {
        PyErr_NoMemory();
        Py_RETURN_NONE;
    }

    self->rx_idx = 0;
    int ok = hackrf_start_rx(self->device, rx_callback, (void *) self);

    self->busy = (ok == HACKRF_SUCCESS);
    return PyBool_FromLong(ok);
}

static PyObject *py_start_rx_stream(HackrfObject *self, PyObject *Py_UNUSED(unused)) {
    if (self->busy) {
        Py_RETURN_FALSE;
    }

    if (self->pkt_queue.size == 0) {
        PyErr_SetString(PyExc_RuntimeError, "queue not initialized");
        Py_RETURN_NONE;
    }

    flush_queue(&self->pkt_queue);

    int ok = hackrf_start_rx(self->device, rx_stream_callback, (void *) self);
    self->busy = (ok == HACKRF_SUCCESS);

    return PyBool_FromLong(ok);
}

static PyObject *py_start_tx(HackrfObject *self, PyObject *args) {
    if (self->busy) {
        Py_RETURN_FALSE;
    }

    PyObject *tx_buf;
    if (!PyArg_ParseTuple(args, "O", &tx_buf)) {
        Py_RETURN_NONE;
    }

    if (!PyByteArray_Check(tx_buf)) {
        PyErr_SetString(PyExc_TypeError, "argument must be a bytearray");
        Py_RETURN_NONE;
    }

    size_t len = PyByteArray_GET_SIZE(tx_buf);

    if (pkt_allocate(self, len) != 0) {
        PyErr_NoMemory();
        Py_RETURN_NONE;
    }

    memcpy(self->data_pkt.buf, PyByteArray_AS_STRING(tx_buf), len);

    self->busy = true;
    self->tx_idx = 0;

    int ok = hackrf_start_tx(self->device, tx_callback, (void *) self);
    return PyBool_FromLong(ok);
}

static PyObject *py_start_tx_stream(HackrfObject *self, PyObject *Py_UNUSED(unused)) {
    if (self->busy) {
        Py_RETURN_FALSE;
    }

    if (self->pkt_queue.size == 0) {
        PyErr_SetString(PyExc_RuntimeError, "queue not initialized");
        Py_RETURN_NONE;
    }

    flush_queue(&self->pkt_queue);

    self->busy = true;
    self->tx_len = 0;
    self->tx_idx = 0;

    int ok = hackrf_start_tx(self->device, tx_stream_callback, (void *) self);
    return PyBool_FromLong(ok);
}

static PyObject *py_start_sweep(HackrfObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"freqs_list", "chunks", "step_width", "offset", NULL};

    uint16_t frequencies[MAX_SWEEP_RANGES * 2];
    uint32_t chunks;
    uint32_t step_width;
    uint32_t offset;

    PyObject *freqs_list;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OIII", kwlist,
            &freqs_list, &chunks, &step_width, &offset)) {
        PyErr_SetString(PyExc_TypeError, "invalid argument");
        Py_RETURN_NONE;
    }

    Py_ssize_t size = PyList_Size(freqs_list);
    if (size >= MAX_SWEEP_RANGES) {
        PyErr_SetString(PyExc_ValueError, "number of ranges exceeds MAX_SWEEP_RANGES");
        return NULL;
    }

    int ctr = 0;
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject* tuple = PyList_GetItem(freqs_list, i);

        if (!PyTuple_Check(tuple) || PyTuple_Size(tuple) != 2) {
            PyErr_SetString(PyExc_TypeError, "each element must be a tuple with two integers");
            return NULL;
        }

        PyObject* v0 = PyTuple_GetItem(tuple, 0);
        PyObject* v1 = PyTuple_GetItem(tuple, 1);

        if (!PyLong_Check(v0) || !PyLong_Check(v1)) {
            PyErr_SetString(PyExc_TypeError, "both elements in the tuple must be integers");
            return NULL;
        }

        // Extract integer values
        uint32_t freq_min = PyLong_AsLong(v0);
        uint32_t freq_max = PyLong_AsLong(v1);

        frequencies[ctr++] = freq_min;
        frequencies[ctr++] = freq_max;
    }

    int ok = hackrf_init_sweep(
        self->device,
        frequencies,
        size,
        chunks * BYTES_PER_BLOCK,
        step_width,
        offset,
        INTERLEAVED);

    if (ok != HACKRF_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize sweep");
        Py_RETURN_FALSE;
    }

    ok = hackrf_start_rx_sweep(self->device, rx_stream_callback, (void *) self);
    self->busy = (ok == HACKRF_SUCCESS);
    return PyBool_FromLong(ok);
}

static PyObject *py_allow_overruns(HackrfObject *self, PyObject *args) {
    PyObject *val;

    if (!PyArg_ParseTuple(args, "O", &val)) {
        PyErr_SetString(PyExc_TypeError, "argument must be bool");
        Py_RETURN_NONE;
    }

    self->allow_overruns = (PyObject_IsTrue(val) > 0);

    Py_RETURN_NONE;
}

static PyObject *py_stop_transfer(HackrfObject *self, PyObject *Py_UNUSED(unused)) {
    self->busy = false;
    hackrf_stop_tx(self->device); // same code used for rx

    Py_RETURN_NONE;
}

static PyObject *py_busy(HackrfObject *self, PyObject *Py_UNUSED(unused)) {
    return Py_NewRef(self->busy ? Py_True : Py_False);
}

static PyObject *py_device_list(PyObject *Py_UNUSED(unused)) {
    hackrf_device_list_t *list = hackrf_device_list();

    PyObject *devices = PyList_New(0);
    for (int i = 0; i < list->devicecount; i++) {
        PyObject *device = Py_BuildValue("s", list->serial_numbers[i]);
        PyList_Append(devices, device);
        Py_DECREF(device);
    }

    hackrf_device_list_free(list);
    return devices;
}

static PyObject *py_get_bytes_per_block(HackrfObject *self, void *closure) {
    return PyLong_FromLong(BYTES_PER_BLOCK);
}

static PyObject *py_get_blocks_per_transfer(HackrfObject *self, void *closure) {
    return PyLong_FromLong(16); // defined in hackrf_sweep.c
}

static int py_init(HackrfObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"fifo_len", "device_serial", NULL};
    const char *serial = NULL;
    uint32_t q_len = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Is", kwlist, &q_len, &serial)) {
        PyErr_SetString(PyExc_TypeError, "invalid argument");
        return -1;
    }

    if (hackrf_open_by_serial(serial, &self->device) != HACKRF_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "failed to open hackrf");
        return -1;
    }

    if (!queue_init(&self->pkt_queue, sizeof(struct packet), q_len)) {
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize queue");
        return -1;
    }

    hackrf_set_hw_sync_mode(self->device, 0);
    hackrf_enable_tx_flush(self->device, flush_callback, (void*) self);

    self->busy = false;
    self->allow_overruns = false;
    memset(&self->data_pkt, 0, sizeof(struct packet));

    return 0;
}

static void py_dealloc(HackrfObject *self) {
    queue_terminate(&self->pkt_queue);
    hackrf_close(self->device);
    flush_queue(&self->pkt_queue);
    queue_deinit(&self->pkt_queue);
    pkt_free(self);

    Py_TYPE(self)->tp_free((PyObject *) self);
}

static void cleanup() {
    hackrf_exit();
}

static PyGetSetDef hackrf_getsetters[] = {
    {"BYTES_PER_BLOCK", (getter) py_get_bytes_per_block, NULL, "number of bytes per block", NULL},
    {"BLOCKS_PER_TRANSFER", (getter) py_get_blocks_per_transfer, NULL, "number of blocks per transfer", NULL},
    {NULL}
};

static PyMethodDef hackrf_methods[] = {
    {"busy", (PyCFunction) py_busy, METH_NOARGS, "check if transmission is in progress"},
    {"start_tx", (PyCFunction) py_start_tx, METH_VARARGS, "start transmission"},
    {"start_rx", (PyCFunction) py_start_rx, METH_VARARGS, "start reception of fixed length"},
    {"start_rx_stream", (PyCFunction) py_start_rx_stream, METH_NOARGS, "start rx stream"},
    {"start_tx_stream", (PyCFunction) py_start_tx_stream, METH_NOARGS, "start tx stream"},
    {"start_sweep", (PyCFunction) py_start_sweep, METH_VARARGS | METH_KEYWORDS,
        "start rx sweep.\n"
        "frequency_list - list of start-stop frequency pairs in MHz, must be less than 10\n"
        "chunks - number of 16384 byte chunks to capture per tuning\n"
        "step_width - width of each tuning step in Hz\n"
        "offset - frequency offset added to tuned frequencies. sample_rate / 2 is a good value"
    },
    {"allow_overruns", (PyCFunction) py_allow_overruns, METH_VARARGS, "allow dropping packets"},
    {"push", (PyCFunction) py_push, METH_VARARGS, "push data to tx queue"},
    {"pop", (PyCFunction) py_pop, METH_VARARGS, "pop data from rx queue"},
    {"read", (PyCFunction) py_read, METH_NOARGS, "read received data"},
    {"set_sample_rate", (PyCFunction) py_set_sample_rate, METH_VARARGS, "set sample rate"},
    {"set_freq", (PyCFunction) py_set_freq, METH_VARARGS, "set frequency"},
    {"set_baseband_filter_bandwidth", (PyCFunction) py_set_baseband_filter_bandwidth, METH_VARARGS,
        "set baseband filter bandwidth in Hz.\n"
        "Possible values: 1.75, 2.5, 3.5, 5, 5.5, 6, 7, 8, 9, 10, 12, 14, 15, 20, 24, 28MHz"
    },
    {"set_tx_gain", (PyCFunction) py_set_tx_gain, METH_VARARGS, "set tx gain"},
    {"set_rx_gain", (PyCFunction) py_set_rx_gain, METH_VARARGS, "set rx lna and vga"},
    {"set_amp", (PyCFunction) py_set_amp, METH_VARARGS, "set rf amp state"},
    {"set_antenna_enable", (PyCFunction) py_set_antenna_enable, METH_VARARGS, "toggle antenna port power"},
    {"set_hw_sync_mode", (PyCFunction) py_set_hw_sync_mode, METH_VARARGS, "toggle hardware sync"},
    {"stop_transfer", (PyCFunction) py_stop_transfer, METH_NOARGS, "stop rx/tx"},
    {NULL}
};

static PyMethodDef module_method_table[] = {
    {"device_list", (PyCFunction) py_device_list, METH_NOARGS, "list available hackrf devices"},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject HackrfType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "py_hackrf.hackrf",
    .tp_doc = "hackrf object",
    .tp_basicsize = sizeof(HackrfObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) py_init,
    .tp_dealloc = (destructor) py_dealloc,
    .tp_methods = hackrf_methods,
    .tp_getset = hackrf_getsetters
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "py_hackrf", "hackrf python library", -1, module_method_table
};

PyMODINIT_FUNC PyInit_py_hackrf() {
    if (PyType_Ready(&HackrfType) < 0)
        return NULL;

    PyObject *m = PyModule_Create(&module);
    if (m == NULL)
        return NULL;

    Py_INCREF(&HackrfType);
    if (PyModule_AddObject(m, "hackrf", (PyObject *) &HackrfType) < 0) {
        Py_DECREF(&HackrfType);
        Py_DECREF(m);
        return NULL;
    }

    if (hackrf_init() != HACKRF_SUCCESS)
        return NULL;

    atexit(cleanup);

    return m;
}
