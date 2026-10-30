# strip_icon.py <exe> [--list]: delete every icon resource (RT_ICON, RT_GROUP_ICON) from a Windows
# executable, in place. A build made with RECOMP_PC_EXE carries the PC release's icon, which is
# not ours to distribute: run this on the copy that goes into a release. --list only reports.
import ctypes, sys
from ctypes import wintypes as w

k = ctypes.WinDLL('kernel32', use_last_error=True)
RT_ICON, RT_GROUP_ICON = 3, 14
k.LoadLibraryExW.restype = w.HMODULE
k.LoadLibraryExW.argtypes = [w.LPCWSTR, w.HANDLE, w.DWORD]
ENUMNAME = ctypes.WINFUNCTYPE(w.BOOL, w.HMODULE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
ENUMLANG = ctypes.WINFUNCTYPE(w.BOOL, w.HMODULE, ctypes.c_void_p, ctypes.c_void_p, w.WORD, ctypes.c_void_p)
k.EnumResourceNamesW.argtypes = [w.HMODULE, ctypes.c_void_p, ENUMNAME, ctypes.c_void_p]
k.EnumResourceLanguagesW.argtypes = [w.HMODULE, ctypes.c_void_p, ctypes.c_void_p, ENUMLANG, ctypes.c_void_p]
k.BeginUpdateResourceW.restype = w.HANDLE
k.BeginUpdateResourceW.argtypes = [w.LPCWSTR, w.BOOL]
k.UpdateResourceW.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, w.WORD, ctypes.c_void_p, w.DWORD]
k.EndUpdateResourceW.argtypes = [w.HANDLE, w.BOOL]
k.FreeLibrary.argtypes = [w.HMODULE]


def icons(path):
    mod = k.LoadLibraryExW(path, None, 0x2 | 0x20)
    if not mod:
        raise OSError(ctypes.get_last_error())
    found = []
    for rtype in (RT_ICON, RT_GROUP_ICON):
        names = []

        def on_name(m, t, name, p):
            if name >> 16 == 0:
                names.append(name)          # integer id
            else:
                names.append(ctypes.wstring_at(name))
            return True
        cb = ENUMNAME(on_name)
        k.EnumResourceNamesW(mod, rtype, cb, None)
        for name in names:
            langs = []
            lcb = ENUMLANG(lambda m, t, n, lang, p: langs.append(lang) or True)
            nm = name if isinstance(name, int) else ctypes.c_wchar_p(name)
            k.EnumResourceLanguagesW(mod, rtype, nm if isinstance(name, int) else ctypes.cast(nm, ctypes.c_void_p), lcb, None)
            for lang in langs:
                found.append((rtype, name, lang))
    k.FreeLibrary(mod)
    return found


path = sys.argv[1]
found = icons(path)
print(path, 'icon resources:', found)
if '--list' in sys.argv or not found:
    sys.exit(0)
h = k.BeginUpdateResourceW(path, False)
if not h:
    raise OSError(ctypes.get_last_error())
for rtype, name, lang in found:
    nm = name if isinstance(name, int) else ctypes.cast(ctypes.c_wchar_p(name), ctypes.c_void_p)
    if not k.UpdateResourceW(h, rtype, nm, lang, None, 0):
        raise OSError(ctypes.get_last_error())
if not k.EndUpdateResourceW(h, False):
    raise OSError(ctypes.get_last_error())
print('after:', icons(path))
