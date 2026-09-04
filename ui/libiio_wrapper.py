from abc import get_cache_token
import ctypes

_iio = ctypes.cdll.LoadLibrary("./libiio_wrapper_but_python_shouldnt_import_this.so")

_connect = _iio.connect
_connect.restype = ctypes.c_int
_connect.argtypes = (
    ctypes.c_char_p,
)
def connect(uri: str) -> int:
    return _connect(uri.encode())

_disconnect = _iio.disconnect
def disconnect():
    _disconnect()

_get_next_device = _iio.get_next_device
_get_next_device.restype = ctypes.c_char_p
def get_devices() -> list[str]:
    devices = []
    while dev := _get_next_device():
        devices.append(dev.decode())
    return devices

_set_device = _iio.set_device
_set_device.restype = ctypes.c_int
_set_device.argtypes = (
    ctypes.c_int,
)
def set_device(dev: int) -> int:
    ret = _set_device(dev)
    return ret

_get_next_channel = _iio.get_next_channel
_get_next_channel.restype = ctypes.c_char_p
_get_next_channel.argtypes = (
    ctypes.c_int,
)
def get_channels(dev: int) -> list[str]:
    channels = []
    while chan := _get_next_channel(dev):
        channels.append(chan.decode())
    return channels

_set_channel = _iio.set_channel
_set_channel.restype = ctypes.c_int
_set_channel.argtypes = (
    ctypes.c_int,
)
def set_channel(chan: int) -> int:
    return _set_channel(chan)

_get = _iio.get
_get.restype = ctypes.POINTER(ctypes.c_float)
_get.argtypes = (
    ctypes.POINTER(ctypes.c_size_t),
)
def get() -> list[int]:
    byte_count = ctypes.c_size_t()
    block = _get(ctypes.byref(byte_count))
    sample_count = int(byte_count.value)
    return block[:sample_count]


def get_devices_once(uri: str) -> list[str]:
    if connect(uri) < 0:
        raise ValueError

    devices = get_devices()
    disconnect()
    return devices

def get_channels_once(uri: str, dev: str) -> list[str]:
    if connect(uri) < 0:
        raise ValueError

    devices = get_devices()
    if not devices:
        disconnect()
        return []

    index = devices.index(dev)
    if set_device(index) < 0:
        disconnect()
        raise ValueError

    channels = get_channels(index)
    disconnect()
    return channels
