# extract_icon.py <exe> <out.ico>: the first icon group of a Windows executable, as an .ico file.
# Used by CMakeLists.txt (RECOMP_PC_EXE) to give turok_evolution.exe the PC release's icon,
# taken from the user's own copy of Turok4.exe -- the icon itself is not in this repository.
import struct
import sys

RT_ICON, RT_GROUP_ICON = 3, 14


def main(path, out):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        sys.exit('not a PE file')
    sections, optional_size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    directories = optional + (96 if magic == 0x10B else 112)
    rsrc_rva = struct.unpack_from('<I', data, directories + 2 * 8)[0]
    table = optional + optional_size
    spans = [struct.unpack_from('<IIII', data, table + i * 40 + 8) for i in range(sections)]

    def offset(rva):
        for size, va, raw_size, raw in spans:
            if va <= rva < va + max(size, raw_size):
                return raw + rva - va
        sys.exit('RVA %08X outside every section' % rva)

    base = offset(rsrc_rva)

    def entries(directory):
        named, ids = struct.unpack_from('<HH', data, base + directory + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from('<II', data, base + directory + 16 + i * 8)
            yield name, target

    def leaf(target):
        while target & 0x80000000:   # descend to the first language
            target = next(entries(target & 0x7FFFFFFF))[1]
        rva, size = struct.unpack_from('<II', data, base + target)
        return data[offset(rva):offset(rva) + size]

    def resource(kind, ident=None):
        for name, target in entries(0):
            if name == kind:
                for name2, target2 in entries(target & 0x7FFFFFFF):
                    if ident is None or name2 == ident:
                        return leaf(target2)
        return None

    group = resource(RT_GROUP_ICON)
    if not group:
        sys.exit('no icon group')
    count = struct.unpack_from('<H', group, 4)[0]
    images, header = [], struct.pack('<HHH', 0, 1, count)
    at = 6 + 16 * count
    for i in range(count):
        width, height, colours, reserved, planes, bits, size, ident = struct.unpack_from('<BBBBHHIH', group, 6 + i * 14)
        image = resource(RT_ICON, ident) or b''
        header += struct.pack('<BBBBHHII', width, height, colours, reserved, planes, bits, len(image), at)
        images.append(image)
        at += len(image)
    open(out, 'wb').write(header + b''.join(images))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
