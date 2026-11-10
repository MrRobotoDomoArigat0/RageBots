"""Regenerate SDL2.def from the game's original SDL2.dll.
Usage: python3 gen_def.py path/to/original/SDL2.dll > SDL2.def
Every export keeps its name and ordinal. Names in INTERCEPT are implemented in sor4bots.c
(as Proxy_<name>); everything else becomes a PE forwarder to SDL2_real.<name>."""
import sys, pefile
INTERCEPT = {'SDL_Init', 'SDL_InitSubSystem', 'SDL_Quit', 'SDL_QuitSubSystem', 'SDL_PollEvent', 'SDL_GetVersion',
             'SDL_GameControllerGetButton', 'SDL_GameControllerGetAxis'}
p = pefile.PE(sys.argv[1])
print('LIBRARY SDL2.dll\nEXPORTS')
for e in p.DIRECTORY_ENTRY_EXPORT.symbols:
    n = e.name.decode()
    print(f'    {n}=Proxy_{n} @{e.ordinal}' if n in INTERCEPT else f'    {n}=SDL2_real.{n} @{e.ordinal}')
