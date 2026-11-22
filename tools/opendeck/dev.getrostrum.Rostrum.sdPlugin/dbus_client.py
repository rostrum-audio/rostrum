"""Bounded little-endian D-Bus codec and session-bus client.

Body bytes are marshaled as if they begin on an 8-byte boundary, which is
where a message places its body. SessionBus speaks SASL EXTERNAL, then reads
one framed message at a time so a stream can never be handed to the codec.
"""

import asyncio
import os
import struct

MAX_MESSAGE = 1024 * 1024

# Call args s/ss; replies a(ssdb), as, a{sv}; signals sa{sv}as and sss.
# 's' is also the usual error and method-return body.
_ALLOWED = frozenset({
    's', 'ss', 'sss', 'as', 'a{sv}', 'a(ssdb)', 'sa{sv}as',
})

_HEADER_FIELDS = (
    ('path', 1, 'o'),
    ('interface', 2, 's'),
    ('member', 3, 's'),
    ('error_name', 4, 's'),
    ('reply_serial', 5, 'u'),
    ('destination', 6, 's'),
    ('sender', 7, 's'),
    ('signature', 8, 'g'),
)
_FIELD_SIGNATURE = {code: signature for _name, code, signature in _HEADER_FIELDS}
_FIELD_NAME = {code: name for name, code, _signature in _HEADER_FIELDS}


class Message:
    def __init__(self, msg_type, flags, serial, body, reply_serial=None, path=None,
                 interface=None, member=None, destination=None, sender=None,
                 error_name=None, signature=''):
        self.type = msg_type
        self.flags = flags
        self.serial = serial
        self.reply_serial = reply_serial
        self.path = path
        self.interface = interface
        self.member = member
        self.destination = destination
        self.sender = sender
        self.error_name = error_name
        self.signature = signature or ''
        self.body = body


class DBusError(Exception):
    def __init__(self, name, message=''):
        self.name = name or ''
        self.message = message or ''
        detail = self.name if not self.message else f'{self.name}: {self.message}'
        super().__init__(detail)


class SessionBus:
    """Async session bus. One reader task matches replies and dispatches signals."""

    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self._serial = 0
        self._pending = {}
        self.on_signal = None
        self._reader_task = asyncio.create_task(self._read_loop())

    @classmethod
    async def connect(cls, address=None):
        path = _session_socket_path(address)
        reader, writer = await asyncio.open_unix_connection(path)
        try:
            await _authenticate(reader, writer)
        except Exception:
            writer.close()
            await writer.wait_closed()
            raise
        return cls(reader, writer)

    async def call(self, destination, path, interface, member, signature='', *args, timeout=2.0):
        if self._reader_task is None or self._reader_task.done():
            raise ConnectionError('D-Bus connection closed')
        self._serial = (self._serial % 0xffffffff) + 1
        serial = self._serial
        body = encode_body(signature, *args) if signature else b''
        msg = encode_message('l', 1, 0x02, serial, {
            'path': path,
            'interface': interface,
            'member': member,
            'destination': destination,
            'signature': signature,
        }, body)
        fut = asyncio.get_running_loop().create_future()
        self._pending[serial] = fut
        try:
            self.writer.write(msg)
            await self.writer.drain()
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._pending.pop(serial, None)

    async def close(self):
        task = self._reader_task
        self._reader_task = None
        if task is not None and not task.done():
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        self._fail_pending(ConnectionError('D-Bus connection closed'))
        self._close_writer()
        try:
            await self.writer.wait_closed()
        except (BrokenPipeError, ConnectionError, OSError):
            pass

    async def _read_loop(self):
        try:
            while True:
                message = await self._read_frame()
                self._dispatch(message)
        except asyncio.CancelledError:
            raise
        except asyncio.IncompleteReadError:
            self._fail_pending(ConnectionError('D-Bus connection closed'))
        except Exception as exc:
            self._fail_pending(exc)
        finally:
            self._fail_pending(ConnectionError('D-Bus connection closed'))
            self._close_writer()

    async def _read_frame(self):
        # Header first, then exactly the declared remainder. Never accumulate a
        # stream: decode_message rejects any buffer larger than one message.
        header = await self.reader.readexactly(16)
        if header[0:1] != b'l':
            raise ValueError('only little-endian messages are supported')
        body_len, _serial, fields_len = struct.unpack_from('<III', header, 4)
        if body_len > MAX_MESSAGE or fields_len > MAX_MESSAGE:
            raise ValueError('message exceeds maximum size')
        header_end = 16 + fields_len
        total = ((header_end + 7) & ~7) + body_len
        if total > MAX_MESSAGE or total < 16:
            raise ValueError('message exceeds maximum size')
        rest = await self.reader.readexactly(total - 16) if total > 16 else b''
        frame = header + rest
        consumed, message = decode_message(frame)
        if message is None or consumed != len(frame):
            raise ValueError('truncated D-Bus message')
        return message

    def _dispatch(self, message):
        if message.type == 4:
            if self.on_signal is not None:
                self.on_signal(message)
            return
        if message.type not in (2, 3):
            return
        fut = self._pending.get(message.reply_serial)
        if fut is None or fut.done():
            return
        if message.type == 3:
            text = ''
            if message.signature == 's':
                text, = decode_body('s', message.body)
            elif message.signature:
                raise ValueError('unsupported error body')
            fut.set_exception(DBusError(message.error_name or '', text))
            return
        fut.set_result(decode_body(message.signature, message.body))

    def _fail_pending(self, exc):
        pending = list(self._pending.values())
        self._pending.clear()
        for fut in pending:
            if not fut.done():
                fut.set_exception(_copy_error(exc))

    def _close_writer(self):
        if not self.writer.is_closing():
            self.writer.close()


def _copy_error(exc):
    # IncompleteReadError.args is only the message, so reconstructing it
    # raises TypeError and would leave the caller waiting until timeout.
    if isinstance(exc, asyncio.IncompleteReadError):
        return ConnectionError('D-Bus connection closed')
    try:
        return exc.__class__(*exc.args) if exc.args else exc.__class__(str(exc))
    except Exception:
        return ConnectionError('D-Bus connection closed')


def _session_socket_path(address):
    if address is None:
        address = os.environ.get('DBUS_SESSION_BUS_ADDRESS')
    if not isinstance(address, str) or not address:
        raise ValueError('DBUS_SESSION_BUS_ADDRESS is not set')
    for entry in address.split(';'):
        if not entry.startswith('unix:'):
            continue
        for part in entry[len('unix:'):].split(','):
            key, sep, value = part.partition('=')
            if sep and key == 'path' and value:
                return _unescape_address(value)
    raise ValueError('session bus address has no unix path')


def _unescape_address(value):
    out = bytearray()
    index = 0
    while index < len(value):
        if value[index] == '%':
            hex_digits = value[index + 1:index + 3]
            if len(hex_digits) != 2:
                raise ValueError('invalid session bus address escape')
            try:
                out.append(int(hex_digits, 16))
            except ValueError as error:
                raise ValueError('invalid session bus address escape') from error
            index += 3
        else:
            out.extend(value[index].encode('utf-8'))
            index += 1
    return out.decode('utf-8')


async def _read_sasl_line(reader):
    try:
        raw = await reader.readuntil(b'\r\n')
    except asyncio.IncompleteReadError as error:
        raise ConnectionError('D-Bus connection closed during SASL') from error
    except asyncio.LimitOverrunError as error:
        raise ConnectionError('D-Bus SASL line too long') from error
    if raw.startswith(b'\0'):
        raw = raw[1:]
    try:
        return raw[:-2].decode('ascii')
    except UnicodeDecodeError as error:
        raise ConnectionError('D-Bus SASL line is not ASCII') from error


async def _authenticate(reader, writer):
    token = str(os.getuid()).encode('ascii').hex().encode('ascii')
    writer.write(b'\0AUTH EXTERNAL ' + token + b'\r\n')
    await writer.drain()
    while True:
        line = await _read_sasl_line(reader)
        if line.startswith('OK '):
            writer.write(b'BEGIN\r\n')
            await writer.drain()
            return
        if line == 'DATA' or line.startswith('DATA '):
            writer.write(b'DATA\r\n')
            await writer.drain()
            continue
        if line.startswith('REJECTED') or line.startswith('ERROR'):
            raise ConnectionError(line)
        raise ConnectionError(f'unexpected D-Bus SASL line: {line}')


class _Cursor:
    def __init__(self, buf, pos, end):
        self.buf = buf
        self.pos = pos
        self.end = end

    def align(self, boundary):
        aligned = (self.pos + boundary - 1) & ~(boundary - 1)
        if aligned > self.end:
            raise ValueError('truncated D-Bus value')
        self.pos = aligned

    def take(self, size):
        if size < 0 or self.pos + size > self.end:
            raise ValueError('truncated D-Bus value')
        chunk = self.buf[self.pos:self.pos + size]
        self.pos += size
        return chunk


def _align_buf(buf, boundary):
    buf.extend(b'\x00' * ((-len(buf)) % boundary))


def _parse_one(signature, index):
    if index >= len(signature):
        raise ValueError('truncated signature')
    code = signature[index]
    if code == 'h':
        raise ValueError('unix file descriptors are not supported')
    if code in 'sbdg':
        return code, index + 1
    if code == 'v':
        return 'v', index + 1
    if code == 'a':
        element, index = _parse_one(signature, index + 1)
        return ('a', element), index
    if code == '(':
        fields = []
        index += 1
        while index < len(signature) and signature[index] != ')':
            field, index = _parse_one(signature, index)
            fields.append(field)
        if index >= len(signature) or signature[index] != ')':
            raise ValueError('unclosed struct signature')
        return ('(', tuple(fields)), index + 1
    if code == '{':
        key, index = _parse_one(signature, index + 1)
        value, index = _parse_one(signature, index)
        if index >= len(signature) or signature[index] != '}':
            raise ValueError('unclosed dict signature')
        return ('{', key, value), index + 1
    raise ValueError(f'unsupported type {code!r}')


def _parse_signature(signature):
    types = []
    index = 0
    while index < len(signature):
        parsed, index = _parse_one(signature, index)
        types.append(parsed)
    return types


def _alignment(kind):
    if kind in ('s', 'o', 'b', 'u'):
        return 4
    if kind == 'd':
        return 8
    if kind in ('g', 'v'):
        return 1
    if isinstance(kind, tuple) and kind[0] == 'a':
        return 4
    if isinstance(kind, tuple) and kind[0] in ('(', '{'):
        return 8
    raise ValueError(f'unsupported type {kind!r}')


def _check_object_path(path):
    if not isinstance(path, str) or not path.startswith('/') or '//' in path:
        raise ValueError('invalid object path')
    if len(path) > 1 and path.endswith('/'):
        raise ValueError('invalid object path')
    allowed = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/'
    if any(character not in allowed for character in path):
        raise ValueError('invalid object path')


def _write_u32(buf, value):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xffffffff:
        raise ValueError('invalid uint32')
    _align_buf(buf, 4)
    buf.extend(struct.pack('<I', value))


def _write_bool(buf, value):
    if not isinstance(value, bool):
        raise ValueError('invalid boolean')
    _align_buf(buf, 4)
    buf.extend(struct.pack('<I', 1 if value else 0))


def _write_string(buf, text):
    if not isinstance(text, str):
        raise ValueError('invalid string')
    raw = text.encode('utf-8')
    if b'\x00' in raw or len(raw) > 0xffffffff:
        raise ValueError('invalid string')
    _align_buf(buf, 4)
    buf.extend(struct.pack('<I', len(raw)))
    buf.extend(raw)
    buf.append(0)


def _write_signature(buf, text):
    if not isinstance(text, str):
        raise ValueError('invalid signature')
    try:
        raw = text.encode('ascii')
    except UnicodeEncodeError as error:
        raise ValueError('invalid signature') from error
    if len(raw) > 255:
        raise ValueError('invalid signature')
    buf.append(len(raw))
    buf.extend(raw)
    buf.append(0)


def _write_type(buf, kind, value):
    if kind == 's':
        _write_string(buf, value)
    elif kind == 'b':
        _write_bool(buf, value)
    elif kind == 'd':
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError('invalid double')
        _align_buf(buf, 8)
        buf.extend(struct.pack('<d', float(value)))
    elif kind == 'v':
        if isinstance(value, bool):
            _write_signature(buf, 'b')
            _write_bool(buf, value)
        elif isinstance(value, str):
            _write_signature(buf, 's')
            _write_string(buf, value)
        else:
            raise ValueError('variant values must be bool or str')
    elif isinstance(kind, tuple) and kind[0] == 'a':
        _write_array(buf, kind[1], value)
    elif isinstance(kind, tuple) and kind[0] == '(':
        _write_struct(buf, kind[1], value)
    elif isinstance(kind, tuple) and kind[0] == '{':
        _write_dict_entry(buf, kind, value)
    else:
        raise ValueError(f'unsupported type {kind!r}')


def _write_array(buf, element, value):
    # Dict entries are a mapping. Every other array is a list of elements.
    # The length counts element bytes only: the pad up to the element
    # alignment is written even when the array is empty, and is not counted.
    if isinstance(element, tuple) and element[0] == '{':
        if not isinstance(value, dict):
            raise ValueError('dict entry array requires a dict')
        items = list(value.items())
    else:
        if isinstance(value, (str, bytes)) or not isinstance(value, (list, tuple)):
            raise ValueError('array requires a list')
        items = list(value)
    _align_buf(buf, 4)
    length_at = len(buf)
    buf.extend(b'\x00\x00\x00\x00')
    _align_buf(buf, _alignment(element))
    data_at = len(buf)
    for item in items:
        _write_type(buf, element, item)
    struct.pack_into('<I', buf, length_at, len(buf) - data_at)


def _write_struct(buf, fields, value):
    if isinstance(value, (str, bytes)) or not isinstance(value, (list, tuple)):
        raise ValueError('struct requires a sequence')
    if len(value) != len(fields):
        raise ValueError('struct field count mismatch')
    _align_buf(buf, 8)
    for field, item in zip(fields, value):
        _write_type(buf, field, item)


def _write_dict_entry(buf, kind, value):
    if isinstance(value, (str, bytes)) or not isinstance(value, (list, tuple)) or len(value) != 2:
        raise ValueError('dict entry requires a pair')
    _align_buf(buf, 8)
    _write_type(buf, kind[1], value[0])
    _write_type(buf, kind[2], value[1])


def _read_u32(cur):
    cur.align(4)
    return struct.unpack('<I', cur.take(4))[0]


def _read_string(cur, object_path=False):
    length = _read_u32(cur)
    raw = cur.take(length)
    if b'\x00' in raw or cur.take(1) != b'\x00':
        raise ValueError('invalid string')
    try:
        text = raw.decode('utf-8')
    except UnicodeDecodeError as error:
        raise ValueError('invalid string') from error
    if object_path:
        _check_object_path(text)
    return text


def _read_signature(cur):
    length = cur.take(1)[0]
    raw = cur.take(length)
    if cur.take(1) != b'\x00':
        raise ValueError('invalid signature')
    try:
        return raw.decode('ascii')
    except UnicodeDecodeError as error:
        raise ValueError('invalid signature') from error


def _read_bool(cur):
    value = _read_u32(cur)
    if value not in (0, 1):
        raise ValueError('invalid boolean')
    return value == 1


def _read_type(cur, kind):
    if kind == 's':
        return _read_string(cur)
    if kind == 'b':
        return _read_bool(cur)
    if kind == 'd':
        cur.align(8)
        return struct.unpack('<d', cur.take(8))[0]
    if kind == 'v':
        signature = _read_signature(cur)
        if signature == 'b':
            return _read_bool(cur)
        if signature == 's':
            return _read_string(cur)
        raise ValueError('variant values must be bool or str')
    if isinstance(kind, tuple) and kind[0] == 'a':
        return _read_array(cur, kind[1])
    if isinstance(kind, tuple) and kind[0] == '(':
        cur.align(8)
        return tuple(_read_type(cur, field) for field in kind[1])
    if isinstance(kind, tuple) and kind[0] == '{':
        cur.align(8)
        return (_read_type(cur, kind[1]), _read_type(cur, kind[2]))
    raise ValueError(f'unsupported type {kind!r}')


def _read_array(cur, element):
    length = _read_u32(cur)
    if length > MAX_MESSAGE:
        raise ValueError('array exceeds maximum message size')
    cur.align(_alignment(element))
    end = cur.pos + length
    if end > cur.end:
        raise ValueError('truncated array')
    saved = cur.end
    cur.end = end
    if isinstance(element, tuple) and element[0] == '{':
        items = {}
        while cur.pos < end:
            key, value = _read_type(cur, element)
            if not isinstance(key, str):
                raise ValueError('dict key must be a string')
            items[key] = value
        result = items
    else:
        result = []
        while cur.pos < end:
            result.append(_read_type(cur, element))
    if cur.pos != end:
        raise ValueError('array length does not match its elements')
    cur.end = saved
    return result


def _check_signature(signature):
    if signature is None or signature == '':
        return ''
    if not isinstance(signature, str) or 'h' in signature or signature not in _ALLOWED:
        raise ValueError(f'unsupported signature {signature!r}')
    return signature


def encode_body(signature, *values):
    signature = _check_signature(signature)
    if signature == '':
        if values:
            raise ValueError('empty signature has no values')
        return b''
    kinds = _parse_signature(signature)
    if len(values) != len(kinds):
        raise ValueError('value count does not match signature')
    buf = bytearray()
    for kind, value in zip(kinds, values):
        _write_type(buf, kind, value)
    return bytes(buf)


def decode_body(signature, data):
    signature = _check_signature(signature)
    if not isinstance(data, (bytes, bytearray)):
        raise ValueError('body must be bytes')
    if signature == '':
        if data:
            raise ValueError('empty signature has a body')
        return ()
    kinds = _parse_signature(signature)
    cur = _Cursor(data, 0, len(data))
    values = tuple(_read_type(cur, kind) for kind in kinds)
    if cur.pos != cur.end:
        raise ValueError('trailing bytes in body')
    return values


def _write_header_field(buf, code, signature, value):
    _align_buf(buf, 8)
    buf.append(code)
    _write_signature(buf, signature)
    if signature == 'o':
        _check_object_path(value)
        _write_string(buf, value)
    elif signature == 's':
        _write_string(buf, value)
    elif signature == 'g':
        _write_signature(buf, value)
    elif signature == 'u':
        _write_u32(buf, value)
    else:
        raise ValueError(f'unsupported header type {signature!r}')


def encode_message(endian, msg_type, flags, serial, headers, body):
    if endian != 'l':
        raise ValueError('only little-endian messages are supported')
    if isinstance(msg_type, bool) or not isinstance(msg_type, int) or not 0 <= msg_type <= 0xff:
        raise ValueError('invalid message type')
    if isinstance(flags, bool) or not isinstance(flags, int) or not 0 <= flags <= 0xff:
        raise ValueError('invalid flags')
    if isinstance(serial, bool) or not isinstance(serial, int) or not 0 <= serial <= 0xffffffff:
        raise ValueError('invalid serial')
    if not isinstance(headers, dict):
        raise ValueError('headers must be a dict')
    known = {name for name, _code, _signature in _HEADER_FIELDS}
    if not set(headers).issubset(known):
        raise ValueError('unknown header field')
    if body is None:
        body = b''
    if not isinstance(body, (bytes, bytearray)):
        raise ValueError('body must be bytes')
    body = bytes(body)
    signature = _check_signature(headers.get('signature', ''))
    if len(body) > MAX_MESSAGE:
        raise ValueError('message exceeds maximum size')

    buf = bytearray(12)
    length_at = len(buf)
    buf.extend(b'\x00\x00\x00\x00')
    _align_buf(buf, 8)
    data_at = len(buf)
    for name, code, field_signature in _HEADER_FIELDS:
        if name not in headers or headers[name] is None:
            continue
        if name == 'signature' and signature == '':
            continue
        value = signature if name == 'signature' else headers[name]
        _write_header_field(buf, code, field_signature, value)
    struct.pack_into('<I', buf, length_at, len(buf) - data_at)
    _align_buf(buf, 8)
    buf.extend(body)
    if len(buf) > MAX_MESSAGE:
        raise ValueError('message exceeds maximum size')
    struct.pack_into('<cBBBII', buf, 0, b'l', msg_type, flags, 1, len(body), serial)
    return bytes(buf)


def _read_header_value(cur, signature):
    if signature == 'o':
        return _read_string(cur, object_path=True)
    if signature == 's':
        return _read_string(cur)
    if signature == 'g':
        return _read_signature(cur)
    if signature == 'u':
        return _read_u32(cur)
    raise ValueError(f'unsupported header type {signature!r}')


def _read_headers(buf, fields_len):
    end = 16 + fields_len
    cur = _Cursor(buf, 16, end)
    found = {}
    while cur.pos < end:
        aligned = (cur.pos + 7) & ~7
        if aligned >= end:
            raise ValueError('header fields length mismatch')
        cur.pos = aligned
        code = cur.take(1)[0]
        if code == 9:
            raise ValueError('unix file descriptors are not supported')
        signature = _read_signature(cur)
        expected = _FIELD_SIGNATURE.get(code)
        if expected is None or signature != expected or code in found:
            raise ValueError('invalid header field')
        found[code] = _read_header_value(cur, signature)
    if cur.pos != end:
        raise ValueError('header fields length mismatch')
    return found


def decode_message(buf):
    if not isinstance(buf, (bytes, bytearray)):
        raise ValueError('message must be bytes')
    if len(buf) > MAX_MESSAGE:
        raise ValueError('message exceeds maximum size')
    if len(buf) < 16:
        return 0, None
    if buf[0:1] != b'l':
        raise ValueError('only little-endian messages are supported')
    msg_type, flags, version = buf[1], buf[2], buf[3]
    body_len, serial, fields_len = struct.unpack_from('<III', buf, 4)
    if version != 1:
        raise ValueError('unsupported protocol version')
    if body_len > MAX_MESSAGE or fields_len > MAX_MESSAGE:
        raise ValueError('message exceeds maximum size')
    header_end = 16 + fields_len
    body_at = (header_end + 7) & ~7
    total = body_at + body_len
    if total > MAX_MESSAGE or total < 16:
        raise ValueError('message exceeds maximum size')
    if len(buf) < total:
        return 0, None
    fields = _read_headers(buf, fields_len)
    signature = _check_signature(fields.get(8, ''))
    message = Message(
        msg_type, flags, serial, bytes(buf[body_at:total]),
        reply_serial=fields.get(5),
        path=fields.get(1),
        interface=fields.get(2),
        member=fields.get(3),
        destination=fields.get(6),
        sender=fields.get(7),
        error_name=fields.get(4),
        signature=signature,
    )
    return total, message
