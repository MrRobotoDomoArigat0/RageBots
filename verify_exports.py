"""Compare the proxy SDL2.dll export table with the game's original SDL2.dll."""
import sys, pefile
orig_path, proxy_path = sys.argv[1], sys.argv[2]
def exports(path):
    p = pefile.PE(path)
    return {e.name.decode(): (e.ordinal, e.forwarder.decode() if e.forwarder else None)
            for e in p.DIRECTORY_ENTRY_EXPORT.symbols if e.name}, p
o, _ = exports(orig_path); x, px = exports(proxy_path)
missing = sorted(set(o) - set(x)); extra = sorted(set(x) - set(o))
ordmis = sorted(n for n in o if n in x and o[n][0] != x[n][0])
fwd = [n for n in x if x[n][1]]; local = sorted(n for n in x if not x[n][1])
badfwd = [n for n in fwd if x[n][1] != "SDL2_real." + n]
print(f"original exports: {len(o)}  proxy exports: {len(x)}")
print(f"missing from proxy: {missing}\nextra in proxy: {extra}\nordinal mismatches: {ordmis}")
print(f"forwarded to SDL2_real: {len(fwd)} (bad targets: {badfwd})\nimplemented in proxy: {local}")
print("machine:", hex(px.FILE_HEADER.Machine), "dll name:", px.DIRECTORY_ENTRY_EXPORT.struct.Name and px.get_string_at_rva(px.DIRECTORY_ENTRY_EXPORT.struct.Name))
sys.exit(1 if missing or extra or ordmis or badfwd else 0)
